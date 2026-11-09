"""The pinned Ghidra: download, verify and unpack it, find a JDK for it, and run headless work in a
separate Python process (the PyGhidra venv) so the JVM never lives inside the kmap process."""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

from .util import KmapError, say, sha256_file, warn, toml_str

KMAP_PKG_ROOT = Path(__file__).resolve().parents[1]   # tools/kmap: put on PYTHONPATH of the worker


def installed_version(ghidra_dir):
    props = Path(ghidra_dir) / 'Ghidra' / 'application.properties'
    if not props.is_file():
        return None
    for line in props.read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith('application.version='):
            return line.split('=', 1)[1].strip()
    return None


def require_ghidra(ctx):
    """Refuses to go on unless the Ghidra kmap would use is exactly the pinned version."""
    pin = ctx.ws.ghidra.version
    found = installed_version(ctx.ghidra_dir)
    where = 'KMAP_GHIDRA' if ctx.env_ghidra else '$KMAP_HOME'
    if found is None:
        raise KmapError(f'no Ghidra found at {ctx.ghidra_dir} ({where}). The pinned version is {pin}: '
                        'run `kmap ghidra install`.')
    if found != pin:
        raise KmapError(f'Ghidra {found} found at {ctx.ghidra_dir} ({where}), but mapping/workspace.toml pins '
                        f'Ghidra {pin}. Analysis results differ between versions, so kmap only runs with the '
                        'pinned one: run `kmap ghidra install` (and unset KMAP_GHIDRA).')
    return ctx.ghidra_dir


def check_ghidra_if_configured(ctx):
    """Every command refuses an explicitly configured Ghidra of the wrong version (KMAP_GHIDRA)."""
    if ctx.env_ghidra:
        found = installed_version(ctx.ghidra_dir)
        if found is not None and found != ctx.ws.ghidra.version:
            require_ghidra(ctx)


# --- JDK ---------------------------------------------------------------------------------------------

def _jdk_version(home):
    home = Path(home)
    release = home / 'release'
    if release.is_file():
        m = re.search(r'^JAVA_VERSION="(\d+)', release.read_text(encoding='utf-8', errors='replace'), re.M)
        if m:
            return int(m.group(1))
    return None


def _is_jdk(home):
    exe = '.exe' if os.name == 'nt' else ''
    return (Path(home) / 'bin' / f'java{exe}').is_file() and (Path(home) / 'bin' / f'javac{exe}').is_file()


def _java_home_from_path():
    java = shutil.which('java')
    if not java:
        return None
    try:
        out = subprocess.run([java, '-XshowSettings:properties', '-version'], capture_output=True, text=True,
                             timeout=60).stderr
    except (OSError, subprocess.TimeoutExpired):
        return None
    m = re.search(r'^\s*java\.home = (.+)$', out, re.M)
    return Path(m.group(1).strip()) if m else None


def find_jdk(ctx):
    """KMAP_JDK, then the JDK recorded by `kmap ghidra install --jdk`, JAVA_HOME, and `java` on PATH."""
    need = ctx.ws.ghidra.jdk
    tried = []
    for label, home in (('KMAP_JDK', os.environ.get('KMAP_JDK')), ('local.toml', ctx.local_settings.get('jdk')),
                        ('JAVA_HOME', os.environ.get('JAVA_HOME')), ('PATH', _java_home_from_path())):
        if not home:
            continue
        v = _jdk_version(home)
        if v is not None and v >= need and _is_jdk(home):
            return Path(home)
        tried.append(f'{label}: {home} ({"JDK " + str(v) if v else "version unknown"}'
                     f'{"" if _is_jdk(home) else ", not a JDK"})')
    raise KmapError(f'Ghidra {ctx.ws.ghidra.version} needs a JDK {need} or newer, and none was found.\n'
                    + ''.join(f'  tried {t}\n' for t in tried)
                    + 'Install one (for example Eclipse Temurin 21) and set KMAP_JDK to its folder, or run '
                    '`kmap ghidra install --jdk <folder>` to remember it.')


# --- install -------------------------------------------------------------------------------------------

def _download(url, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + '.part')
    say(f'downloading {url}')
    with urllib.request.urlopen(url) as r, open(part, 'wb') as f:
        total = int(r.headers.get('Content-Length') or 0)
        done, step = 0, 0
        while True:
            block = r.read(1 << 20)
            if not block:
                break
            f.write(block)
            done += len(block)
            if total and done * 10 // total > step:
                step = done * 10 // total
                warn(f'  {done // (1 << 20)} / {total // (1 << 20)} MB')
    os.replace(part, dest)


def _unpack(archive, home, folder):
    """Unpacks into a temporary folder first, so a failed unpack never leaves a half-installed Ghidra."""
    tmp = Path(tempfile.mkdtemp(prefix='unpack-', dir=home))
    try:
        with zipfile.ZipFile(archive) as z:
            names = z.namelist()
            if not names or not all(n.startswith(folder + '/') for n in names):
                raise KmapError(f'{archive.name} does not contain a single {folder}/ folder')
            for info in z.infolist():
                target = tmp / info.filename
                if info.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, open(target, 'wb') as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                mode = (info.external_attr >> 16) & 0o777
                if mode and os.name != 'nt':
                    os.chmod(target, mode)
        os.replace(tmp / folder, home / folder)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def venv_python(ctx):
    return ctx.home / 'venv' / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')


def setup_python(ctx, ghidra_dir):
    """A venv with Ghidra's bundled PyGhidra and JPype wheels, installed offline."""
    dist = ghidra_dir / 'Ghidra' / 'Features' / 'PyGhidra' / 'pypkg' / 'dist'
    if not dist.is_dir():
        raise KmapError(f'{dist} not found: this Ghidra has no bundled PyGhidra')
    py = venv_python(ctx)
    if not py.is_file():
        say(f'creating the PyGhidra environment in {ctx.home / "venv"}')
        subprocess.run([sys.executable, '-m', 'venv', str(ctx.home / 'venv')], check=True)
    r = subprocess.run([str(py), '-m', 'pip', 'install', '--disable-pip-version-check', '-q', '--no-index',
                        '-f', str(dist), 'pyghidra'], capture_output=True, text=True)
    if r.returncode:
        raise KmapError('installing PyGhidra from the Ghidra distribution failed:\n' + r.stdout + r.stderr)
    r = subprocess.run([str(py), '-c', 'import pyghidra, jpype; print(pyghidra.__version__)'],
                       capture_output=True, text=True)
    if r.returncode:
        raise KmapError('PyGhidra does not import in the venv:\n' + r.stderr)
    say(f'PyGhidra {r.stdout.strip()} ready')


def cmd_install(ctx, jdk=None, skip_python=False):
    pin = ctx.ws.ghidra
    home = ctx.home
    home.mkdir(parents=True, exist_ok=True)
    if jdk:
        jdk = Path(jdk).expanduser().resolve()
        if not _is_jdk(jdk) or (_jdk_version(jdk) or 0) < pin.jdk:
            raise KmapError(f'{jdk} is not a JDK {pin.jdk} or newer (needs bin/java, bin/javac and a release file)')
        (home / 'local.toml').write_text(f'jdk = {toml_str(str(jdk))}\n', encoding='utf-8')
        say(f'JDK {_jdk_version(jdk)} at {jdk} recorded in {home / "local.toml"}')

    target = home / pin.folder
    if ctx.env_ghidra and ctx.ghidra_dir != target:
        warn(f'note: KMAP_GHIDRA is set to {ctx.ghidra_dir}; installing the pin into {target} anyway')
    if installed_version(target) == pin.version:
        say(f'Ghidra {pin.version} is already installed at {target}')
    else:
        if target.exists():
            raise KmapError(f'{target} exists but is not Ghidra {pin.version}; remove it and run again')
        archive = home / 'downloads' / pin.archive
        if not archive.is_file():
            _download(pin.url, archive)
        actual = sha256_file(archive)
        if actual != pin.sha256:
            archive.unlink()
            raise KmapError(f'{pin.archive}: SHA-256 mismatch, the archive was deleted and nothing was installed.\n'
                            f'  expected {pin.sha256}\n  actual   {actual}')
        say(f'{pin.archive}: SHA-256 ok')
        _unpack(archive, home, pin.folder)
        found = installed_version(target)
        if found != pin.version:
            shutil.rmtree(target, ignore_errors=True)
            raise KmapError(f'the archive unpacked Ghidra {found}, not the pinned {pin.version}')
        say(f'Ghidra {pin.version} installed at {target}')
    if not skip_python:
        setup_python(ctx, target)
        say(f'JDK: {find_jdk(ctx)}')
    return 0


def cmd_status(ctx):
    pin = ctx.ws.ghidra
    found = installed_version(ctx.ghidra_dir)
    say(f'pinned:  Ghidra {pin.version} ({pin.archive}, sha256 {pin.sha256[:16]}...)')
    say(f'ghidra:  {ctx.ghidra_dir} -> {found or "not installed"}')
    say(f'pyghidra: {venv_python(ctx) if venv_python(ctx).is_file() else "not set up"}')
    try:
        say(f'jdk:     {find_jdk(ctx)}')
    except KmapError as e:
        say(f'jdk:     none ({e.args[0].splitlines()[0]})')
    say(f'KMAP_HOME: {ctx.home}')
    return 0 if found == pin.version else 1


# --- running headless work ------------------------------------------------------------------------------

def run_worker(ctx, task, payload, quiet=False):
    """Runs kmap.worker `task` inside the PyGhidra venv and returns its JSON result."""
    ghidra_dir = require_ghidra(ctx)
    py = venv_python(ctx)
    if not py.is_file():
        raise KmapError('the PyGhidra environment is missing: run `kmap ghidra install`')
    jdk = find_jdk(ctx)
    tmp = ctx.home / 'tmp'
    tmp.mkdir(parents=True, exist_ok=True)
    req = tmp / f'{task}-request.json'
    resp = tmp / f'{task}-result.json'
    resp.unlink(missing_ok=True)
    req.write_text(json.dumps({'task': task, 'ghidra': str(ghidra_dir), 'home': str(ctx.home),
                               'repo': str(ctx.repo), 'heap': ctx.ws.ghidra.max_heap, 'args': payload}),
                   encoding='utf-8')
    env = dict(os.environ)
    env['PYTHONPATH'] = str(KMAP_PKG_ROOT) + os.pathsep + env.get('PYTHONPATH', '')
    env['GHIDRA_INSTALL_DIR'] = str(ghidra_dir)
    env['JAVA_HOME_OVERRIDE'] = str(jdk)
    env['JAVA_HOME'] = str(jdk)
    # the JVM runs inside python: its native libraries (awt.dll ...) resolve their siblings through PATH
    env['PATH'] = str(jdk / 'bin') + os.pathsep + env.get('PATH', '')
    env['PYTHONUTF8'] = '1'
    r = subprocess.run([str(py), '-m', 'kmap.worker', str(req), str(resp)], env=env,
                       stdout=subprocess.DEVNULL if quiet else None)
    if r.returncode or not resp.is_file():
        raise KmapError(f'Ghidra task {task!r} failed (exit code {r.returncode}); see the output above')
    result = json.loads(resp.read_text(encoding='utf-8'))
    if result.get('error'):
        raise KmapError(f'Ghidra task {task!r}: {result["error"]}')
    return result
