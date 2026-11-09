"""Install / uninstall round trips for scripts/install, and ModManager's download from the mod catalog.

The round trips need a vanilla Cube World Alpha folder: KYUBU_GAME_DIR (default ~/game). The PowerShell variant runs
when `pwsh` is on PATH (or KYUBU_PWSH points at it). The catalog is a local folder signed with a key made for the test
(KYUBU_INSTALL_CATALOG and KYUBU_INSTALL_CATALOG_KEY point the installers at it); building it needs `cryptography`.
"""
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'scripts', 'install'))
import kyubu_install as ki  # noqa: E402

GAME = os.environ.get('KYUBU_GAME_DIR', os.path.expanduser('~/game'))
PWSH = os.environ.get('KYUBU_PWSH') or shutil.which('pwsh')
GAME_FILES = ['Cube.exe', 'Server.exe', 'zlib1.dll', 'data1.db', 'data2.db', 'data3.db', 'data4.db']

needs_game = pytest.mark.skipif(not os.path.isfile(os.path.join(GAME, 'Cube.exe')), reason='no Cube World folder')


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


@pytest.fixture
def env(tmp_path):
    game = tmp_path / 'game'
    game.mkdir()
    for f in GAME_FILES:
        shutil.copy2(os.path.join(GAME, f), game / f)
    # a release folder: proxy + kyubu/ + install scripts
    rel = tmp_path / 'kyubu-release'
    (rel / 'kyubu' / 'symbols').mkdir(parents=True)
    (rel / 'install').mkdir()
    proxy = os.path.join(ROOT, 'build', 'loader', 'zlib1.dll')
    if os.path.isfile(proxy):
        shutil.copy2(proxy, rel / 'zlib1.dll')
    else:
        (rel / 'zlib1.dll').write_bytes(b'MZ fake proxy ' + ki.PROXY_MARKER)
    for f in [f for f in os.listdir(os.path.join(ROOT, 'symbols')) if f.endswith('.toml')]:
        shutil.copy2(os.path.join(ROOT, 'symbols', f), rel / 'kyubu' / 'symbols' / f)
    shutil.copy2(os.path.join(ROOT, 'dist-files', 'config.default.toml'), rel / 'kyubu' / 'config.default.toml')
    for f in ('kyubu_install.py', 'kyubu_install.ps1'):
        shutil.copy2(os.path.join(ROOT, 'scripts', 'install', f), rel / 'install' / f)
    cat = Catalog(tmp_path / 'catalog')
    cat.publish('0.1.0', b'MZ v1')
    return game, rel, cat


class Catalog:
    """A catalog folder with ModManager versions, signed with a key of its own; `env` points an installer at it."""

    def __init__(self, folder):
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        self.folder, self.versions = folder, []
        self.key = Ed25519PrivateKey.generate()
        pub = self.key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.env = dict(os.environ, KYUBU_INSTALL_CATALOG=str(folder), KYUBU_INSTALL_CATALOG_KEY=pub.hex())
        (folder / 'mods' / 'mod_manager').mkdir(parents=True)

    def publish(self, version, dll, kyubu='^1.2', sha=None):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            z.writestr('mod.toml', f'id = "mod_manager"\nversion = "{version}"\n')
            z.writestr('config.toml', f'shipped = "{version}"\n')
            z.writestr('bin/mod_manager.dll', dll)
        url = f'mods/mod_manager/mod_manager-{version}.zip'
        (self.folder / url).write_bytes(buf.getvalue())
        self.versions.append({'version': version, 'kyubu': kyubu, 'url': url, 'size': len(buf.getvalue()),
                              'sha256': sha or hashlib.sha256(buf.getvalue()).hexdigest()})
        self.sign()

    def sign(self, tamper=False):
        index = json.dumps({'format': 1, 'mods': [{'id': 'mod_manager', 'versions': self.versions}]}).encode()
        (self.folder / 'index.json').write_bytes(index)
        sig = bytearray(self.key.sign(index))
        if tamper:
            sig[0] ^= 1
        (self.folder / 'index.json.sig').write_bytes(bytes(sig))


def run_py(rel, *args, env=None):
    return subprocess.run([sys.executable, str(rel / 'install' / 'kyubu_install.py'), *map(str, args)],
                          capture_output=True, text=True, env=env)


def run_ps(rel, action, game, *extra, env=None):
    return subprocess.run([PWSH, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                           str(rel / 'install' / 'kyubu_install.ps1'), '-Action', action, '-GameDir', str(game), *extra],
                          capture_output=True, text=True, env=env)


RUNNERS = ['py'] + (['ps'] if PWSH else [])


def do(kind, rel, action, game, *extra, env=None):
    if kind == 'py':
        return run_py(rel, action, game, *extra, env=env)
    ps_extra = ['-' + ''.join(w.capitalize() for w in e.lstrip('-').split('-')) for e in extra]
    return run_ps(rel, action, game, *ps_extra, env=env)


@needs_game
@pytest.mark.parametrize('kind', RUNNERS)
def test_round_trip_restores_vanilla(env, kind):
    # covers: loader-bootstrap/Uninstall
    game, rel, cat = env
    before = {f: sha(game / f) for f in GAME_FILES}

    r = do(kind, rel, 'install', game, env=cat.env)
    assert r.returncode == 0, r.stdout + r.stderr
    s = ki.state(str(game))
    assert s['state'] == 'installed'
    assert sha(game / 'zlib1_vanilla.dll') == ki.VANILLA_ZLIB
    assert (game / 'kyubu' / 'symbols' / 'cube-0x51D2CE9C.toml').is_file()
    assert (game / 'kyubu' / 'config.toml').is_file()
    assert (game / 'kyubu' / 'backup' / 'zlib1.dll').is_file()
    assert (game / 'mods').is_dir()

    # re-running upgrades in place and keeps the user's config
    (game / 'kyubu' / 'config.toml').write_text('[loader]\ndev_mode = true\n')
    r = do(kind, rel, 'install', game, env=cat.env)
    assert r.returncode == 0, r.stdout + r.stderr
    assert 'dev_mode = true' in (game / 'kyubu' / 'config.toml').read_text()

    r = do(kind, rel, 'uninstall', game)
    assert r.returncode == 0, r.stdout + r.stderr
    after = {f: sha(game / f) for f in GAME_FILES}
    assert after == before                       # data1-4.db, executables and zlib1.dll unchanged
    assert not (game / 'zlib1_vanilla.dll').exists()
    assert (game / 'kyubu').is_dir()             # inert without the proxy

    r = do(kind, rel, 'uninstall', game, '--purge')
    assert r.returncode == 0, r.stdout + r.stderr
    assert not (game / 'kyubu').exists()
    assert (game / 'mods').is_dir()


@needs_game
@pytest.mark.parametrize('kind', RUNNERS)
def test_mod_manager_comes_from_the_catalog(env, kind):
    # the newest ModManager that fits the loader, replaced whole on an upgrade, left alone when up to date
    game, rel, cat = env
    r = do(kind, rel, 'install', game, env=cat.env)
    assert r.returncode == 0, r.stdout + r.stderr
    mm = game / 'mods' / 'mod_manager'
    assert (mm / 'bin' / 'mod_manager.dll').read_bytes() == b'MZ v1' and ki.installed_version(str(mm)) == '0.1.0'
    cat.publish('0.2.0', b'MZ v2')
    cat.publish('0.9.0', b'MZ too new', kyubu='^1.99')          # needs a newer loader: passed over
    (mm / 'stale.txt').write_text('left by 0.1.0')
    r = do(kind, rel, 'install', game, env=cat.env)
    assert r.returncode == 0, r.stdout + r.stderr
    assert (mm / 'bin' / 'mod_manager.dll').read_bytes() == b'MZ v2' and ki.installed_version(str(mm)) == '0.2.0'
    assert not (mm / 'stale.txt').exists() and (mm / 'config.toml').read_text() == 'shipped = "0.2.0"\n'
    r = do(kind, rel, 'install', game, env=cat.env)
    assert r.returncode == 0 and 'up to date' in r.stdout, r.stdout + r.stderr


@needs_game
@pytest.mark.parametrize('kind', RUNNERS)
@pytest.mark.parametrize('fault', ['bad_signature', 'bad_zip', 'no_catalog', 'skipped'])
def test_kyubu_installs_without_mod_manager(env, kind, fault):
    # a catalog that cannot be trusted or reached leaves ModManager out, never the loader
    game, rel, cat = env
    extra = []
    if fault == 'bad_signature':
        cat.sign(tamper=True)
    elif fault == 'bad_zip':
        cat.publish('0.3.0', b'MZ v3', sha='0' * 64)
    elif fault == 'no_catalog':
        shutil.rmtree(cat.folder)
    else:
        extra = ['--no-mod-manager']
    r = do(kind, rel, 'install', game, *extra, env=cat.env)
    assert r.returncode == 0, r.stdout + r.stderr
    assert ki.state(str(game))['state'] == 'installed'
    assert not (game / 'mods' / 'mod_manager').exists()
    if fault != 'skipped':
        assert 'ModManager was not installed' in r.stdout + r.stderr


def test_ed25519_matches_a_real_implementation():
    pytest.importorskip('cryptography')
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    for n in range(8):
        k = Ed25519PrivateKey.generate()
        pub = k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        msg = os.urandom(n * 53)
        sig = k.sign(msg)
        assert ki.ed25519_verify(pub, msg, sig)
        assert not ki.ed25519_verify(pub, msg + b'!', sig)
        assert not ki.ed25519_verify(pub, msg, sig[:10] + bytes([sig[10] ^ 1]) + sig[11:])


def test_loader_api_requirements():
    assert ki.fits_loader('^1.2', (1, 4)) and ki.fits_loader('^1.4', (1, 4)) and ki.fits_loader('*', (1, 4))
    assert not ki.fits_loader('^1.5', (1, 4)) and not ki.fits_loader('^2.0', (1, 4)) and not ki.fits_loader('>=1', (1, 4))


def test_pinned_values_match_the_sources():
    # the catalog key is mod_manager's release key, the API is the proxy's, and both installers pin the same values
    keys = open(os.path.join(ROOT, 'mods-gameplay', 'mod_manager', 'src', 'keys.h')).read()
    release = re.findall(r'0x([0-9a-f]{2})', keys[keys.index('#endif'):])[:32]
    assert ki.CATALOG_KEY == ''.join(release)
    api = open(os.path.join(ROOT, 'sdk', 'include', 'kyubu_api.h')).read()
    major = int(re.search(r'#define KYUBU_API_VERSION_MAJOR (\d+)', api).group(1))
    minor = int(re.search(r'#define KYUBU_API_VERSION_MINOR (\d+)', api).group(1))
    assert ki.LOADER_API == (major, minor)
    ps = open(os.path.join(ROOT, 'scripts', 'install', 'kyubu_install.ps1'), encoding='utf-8').read()
    assert f"$CatalogKey = '{ki.CATALOG_KEY}'" in ps and f"$CatalogUrl = '{ki.CATALOG_URL}'" in ps
    assert f'$LoaderApi = @({major}, {minor})' in ps


@needs_game
@pytest.mark.parametrize('kind', RUNNERS)
def test_refuses_unknown_executable(env, kind):
    game, rel, cat = env
    data = bytearray((game / 'Cube.exe').read_bytes())
    data[0x2000] ^= 0xFF
    (game / 'Cube.exe').write_bytes(bytes(data))
    r = do(kind, rel, 'install', game)
    assert r.returncode != 0
    assert 'not the Cube World Alpha v0.1.1 file' in r.stdout + r.stderr
    assert ki.state(str(game))['state'] == 'vanilla'   # nothing was touched
    r = do(kind, rel, 'install', game, '--force')
    assert r.returncode == 0, r.stdout + r.stderr


@needs_game
@pytest.mark.parametrize('kind', RUNNERS)
def test_refuses_foreign_zlib(env, kind):
    game, rel, cat = env
    (game / 'zlib1.dll').write_bytes(b'MZ some other proxy')
    r = do(kind, rel, 'install', game)
    assert r.returncode != 0
    assert 'neither the original' in r.stdout + r.stderr
    assert not (game / 'zlib1_vanilla.dll').exists()
