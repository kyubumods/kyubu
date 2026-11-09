"""Install / uninstall round trips for scripts/install (task 2.7).

Needs a vanilla Cube World Alpha folder: KYUBU_GAME_DIR (default ~/game). The PowerShell variant runs
when `pwsh` is on PATH (or KYUBU_PWSH points at it).
"""
import hashlib
import os
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'scripts', 'install'))
import kyubu_install as ki  # noqa: E402

GAME = os.environ.get('KYUBU_GAME_DIR', os.path.expanduser('~/game'))
PWSH = os.environ.get('KYUBU_PWSH') or shutil.which('pwsh')
GAME_FILES = ['Cube.exe', 'Server.exe', 'zlib1.dll', 'data1.db', 'data2.db', 'data3.db', 'data4.db']

pytestmark = pytest.mark.skipif(not os.path.isfile(os.path.join(GAME, 'Cube.exe')), reason='no Cube World folder')


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
    (rel / 'mods' / 'mod_manager' / 'bin').mkdir(parents=True)   # the mod the release carries
    (rel / 'mods' / 'mod_manager' / 'mod.toml').write_text('id = "mod_manager"\n')
    (rel / 'mods' / 'mod_manager' / 'config.toml').write_text('shipped = true\n')
    (rel / 'mods' / 'mod_manager' / 'bin' / 'mod_manager.dll').write_bytes(b'MZ v1')
    return game, rel


def run_py(rel, *args):
    return subprocess.run([sys.executable, str(rel / 'install' / 'kyubu_install.py'), *map(str, args)],
                          capture_output=True, text=True)


def run_ps(rel, action, game, *extra):
    return subprocess.run([PWSH, '-NoProfile', '-File', str(rel / 'install' / 'kyubu_install.ps1'),
                           '-Action', action, '-GameDir', str(game), *extra], capture_output=True, text=True)


RUNNERS = ['py'] + (['ps'] if PWSH else [])


def do(kind, rel, action, game, *extra):
    if kind == 'py':
        return run_py(rel, action, game, *extra)
    ps_extra = ['-' + e.lstrip('-').capitalize() for e in extra]
    return run_ps(rel, action, game, *ps_extra)


@pytest.mark.parametrize('kind', RUNNERS)
def test_round_trip_restores_vanilla(env, kind):
    # covers: loader-bootstrap/Uninstall
    game, rel = env
    before = {f: sha(game / f) for f in GAME_FILES}

    r = do(kind, rel, 'install', game)
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
    r = do(kind, rel, 'install', game)
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


@pytest.mark.parametrize('kind', RUNNERS)
def test_installs_the_bundled_mods(env, kind):
    # ModManager comes with the loader: install puts it in mods/, an upgrade replaces it but keeps the player's config
    game, rel = env
    r = do(kind, rel, 'install', game)
    assert r.returncode == 0, r.stdout + r.stderr
    mm = game / 'mods' / 'mod_manager'
    assert (mm / 'mod.toml').is_file() and (mm / 'bin' / 'mod_manager.dll').read_bytes() == b'MZ v1'
    (mm / 'config.toml').write_text('mine = true\n')
    (rel / 'mods' / 'mod_manager' / 'bin' / 'mod_manager.dll').write_bytes(b'MZ v2')
    r = do(kind, rel, 'install', game)
    assert r.returncode == 0, r.stdout + r.stderr
    assert (mm / 'bin' / 'mod_manager.dll').read_bytes() == b'MZ v2'
    assert (mm / 'config.toml').read_text() == 'mine = true\n'


@pytest.mark.parametrize('kind', RUNNERS)
def test_refuses_unknown_executable(env, kind):
    game, rel = env
    data = bytearray((game / 'Cube.exe').read_bytes())
    data[0x2000] ^= 0xFF
    (game / 'Cube.exe').write_bytes(bytes(data))
    r = do(kind, rel, 'install', game)
    assert r.returncode != 0
    assert 'not the Cube World Alpha v0.1.1 file' in r.stdout + r.stderr
    assert ki.state(str(game))['state'] == 'vanilla'   # nothing was touched
    r = do(kind, rel, 'install', game, '--force')
    assert r.returncode == 0, r.stdout + r.stderr


@pytest.mark.parametrize('kind', RUNNERS)
def test_refuses_foreign_zlib(env, kind):
    game, rel = env
    (game / 'zlib1.dll').write_bytes(b'MZ some other proxy')
    r = do(kind, rel, 'install', game)
    assert r.returncode != 0
    assert 'neither the original' in r.stdout + r.stderr
    assert not (game / 'zlib1_vanilla.dll').exists()
