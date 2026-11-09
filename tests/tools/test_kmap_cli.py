"""kmap CLI, workspace location, Ghidra pin and verified game inputs (add-mapping-workspace tasks 1.1-1.3).

Runs anywhere: Ghidra itself is replaced by small fake archives and fake game files pinned in a fixture
workspace.toml. The real install and import are exercised by `kmap ghidra install` / `kmap init`.
"""
import hashlib
import os
import zipfile
from pathlib import Path

import pytest

from conftest import ROOT

COMMANDS = [['check', '--offline'], ['ghidra', 'status'], ['ghidra', 'install'], ['init', '--game', '.'], ['push'],
            ['pull'], ['survey'], ['report'], ['import-cuwo', '.'], ['export'], ['decompile', 'x'], ['probe', 'list'],
            ['lock']]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def test_every_subcommand_is_offered():
    from kmap.cli import build_parser
    text = build_parser().format_help()
    for cmd in ('ghidra', 'init', 'push', 'pull', 'survey', 'report', 'import-cuwo', 'export', 'check', 'decompile',
                'probe', 'lock'):
        assert cmd in text


@pytest.mark.parametrize('args', COMMANDS, ids=lambda a: ' '.join(a))
def test_workspace_inside_the_repository(kmap_repo, args):
    # covers: mapping-workspace/Workspace inside the repository
    kmap_repo.monkeypatch.setenv('KMAP_HOME', str(kmap_repo.root / 'work'))
    code, out, err = kmap_repo.run(*args)
    assert code == 1
    assert 'inside the git working tree' in err
    assert 'never end up in a repository' in err
    from kmap.config import default_home
    assert str(default_home()) in err


def test_default_home(monkeypatch):
    from kmap.config import Context, default_home
    monkeypatch.delenv('KMAP_HOME', raising=False)
    home = Context(ROOT).home
    assert home == default_home()
    if os.name == 'nt':
        assert home.name == 'kyubu-mapping'
        assert str(home).lower().startswith(os.environ['LOCALAPPDATA'].lower())
    else:
        assert home.name == '.kyubu-mapping' and home.parent == Path(os.path.expanduser('~'))


def fake_ghidra(folder, version):
    props = folder / 'Ghidra' / 'application.properties'
    props.parent.mkdir(parents=True)
    props.write_text(f'application.name=Ghidra\napplication.version={version}\n')
    return folder


def test_wrong_ghidra_version(kmap_repo, tmp_path):
    # covers: mapping-workspace/Wrong Ghidra version
    kmap_repo.monkeypatch.setenv('KMAP_GHIDRA', str(fake_ghidra(tmp_path / 'ghidra_11.4_PUBLIC', '11.4')))
    code, out, err = kmap_repo.run('survey')
    assert code == 1
    assert '11.4' in err and '12.1.4' in err and 'kmap ghidra install' in err
    assert not (kmap_repo.home / 'survey').exists()


def test_corrupted_download(kmap_repo):
    # covers: mapping-workspace/Corrupted download
    from kmap.config import Workspace
    pin = Workspace(kmap_repo.mapping / 'workspace.toml').ghidra
    archive = kmap_repo.home / 'downloads' / pin.archive
    archive.parent.mkdir(parents=True)
    truncated = b'PK\x03\x04' + b'\0' * 1000          # a download cut short
    archive.write_bytes(truncated)
    code, out, err = kmap_repo.run('ghidra', 'install', '--skip-python')
    assert code == 1
    assert pin.sha256 in err and sha(truncated) in err
    assert not archive.exists()
    assert not (kmap_repo.home / pin.folder).exists()


def pin_fake_ghidra(kmap_repo, tmp_path, version='9.9'):
    """Replaces the [ghidra] pin with a small local archive (file:// URL)."""
    src = tmp_path / 'ghidra-release.zip'
    with zipfile.ZipFile(src, 'w') as z:
        z.writestr(f'ghidra_{version}_PUBLIC/Ghidra/application.properties', f'application.version={version}\n')
        z.writestr(f'ghidra_{version}_PUBLIC/support/analyzeHeadless', '#!/bin/sh\n')
    ws = (kmap_repo.mapping / 'workspace.toml').read_text()
    head, _, rest = ws.partition('[ghidra]')
    _, _, rest = rest.partition('\n[analysis]')
    ws = (head + '[ghidra]\n'
          f'version = "{version}"\narchive = "ghidra_{version}_PUBLIC_test.zip"\n'
          f'url = "{src.as_uri()}"\nsha256 = "{sha(src.read_bytes())}"\nfolder = "ghidra_{version}_PUBLIC"\n'
          'max_heap = "1G"\njdk = 21\n\n[analysis]' + rest)
    (kmap_repo.mapping / 'workspace.toml').write_text(ws)
    return src


def test_install_verifies_then_unpacks(kmap_repo, tmp_path):
    pin_fake_ghidra(kmap_repo, tmp_path)
    code, out, err = kmap_repo.run('ghidra', 'install', '--skip-python')
    assert code == 0, err
    assert 'SHA-256 ok' in out and 'Ghidra 9.9 installed' in out
    assert (kmap_repo.home / 'ghidra_9.9_PUBLIC' / 'Ghidra' / 'application.properties').is_file()
    code, out, err = kmap_repo.run('ghidra', 'install', '--skip-python')
    assert code == 0 and 'already installed' in out
    assert not list(kmap_repo.home.glob('unpack-*'))


# --- verified game inputs -------------------------------------------------------------------------------

CUBE = b'MZ' + bytes(range(256)) * 4
SERVER = b'MZ' + bytes(reversed(range(256))) * 4


def pin_fake_builds(kmap_repo):
    ws = (kmap_repo.mapping / 'workspace.toml').read_text()
    ws = ws.replace('a4eeb3606ad2b82e4c9b3d0db6f9472ffa6e89a4d56084a9114ff1fb3c812699', sha(CUBE))
    ws = ws.replace('dde8e7082907246d309e07d5a3ad4f230960e7a617bd19fb17f28a55b8c2e7b9', sha(SERVER))
    (kmap_repo.mapping / 'workspace.toml').write_text(ws)


def listing(folder):
    return sorted((str(p.relative_to(folder)), sha(p.read_bytes()), p.stat().st_mtime_ns)
                  for p in folder.rglob('*') if p.is_file())


def make_game(tmp_path, cube=CUBE, server_zipped=True):
    game = tmp_path / 'game'
    game.mkdir()
    (game / 'Cube.exe').write_bytes(cube)
    if server_zipped:
        with zipfile.ZipFile(game / 'Server.zip', 'w') as z:
            z.writestr('Server.exe', SERVER)
    else:
        (game / 'Server.exe').write_bytes(SERVER)
    return game


def test_modified_executable(kmap_repo, tmp_path):
    # covers: mapping-workspace/Modified executable
    pin_fake_builds(kmap_repo)
    bad = bytearray(CUBE)
    bad[100] ^= 0xFF
    game = make_game(tmp_path, bytes(bad))
    code, out, err = kmap_repo.run('init', '--game', game)
    assert code == 1
    assert sha(CUBE) in err and sha(bytes(bad)) in err
    assert not (kmap_repo.home / 'ghidra').exists()
    assert not (kmap_repo.home / 'inputs').exists()


def test_server_only_inside_the_zip(kmap_repo, tmp_path):
    # covers: mapping-workspace/Server only inside the zip (the import itself needs Ghidra: see kmap init)
    pin_fake_builds(kmap_repo)
    game = make_game(tmp_path)
    before = listing(game)
    code, out, err = kmap_repo.run('init', '--game', game)
    assert 'Server.zip!Server.exe verified' in out
    assert code == 1 and 'kmap ghidra install' in err      # no Ghidra in this test
    assert (kmap_repo.home / 'inputs' / 'server-0x51D2CE9B' / 'Server.exe').read_bytes() == SERVER
    assert (kmap_repo.home / 'inputs' / 'cube-0x51D2CE9C' / 'Cube.exe').read_bytes() == CUBE
    assert listing(game) == before
    assert not (game / 'Server.exe').exists()


def test_plain_server_exe_is_accepted(kmap_repo, tmp_path):
    pin_fake_builds(kmap_repo)
    game = make_game(tmp_path, server_zipped=False)
    code, out, err = kmap_repo.run('init', '--game', game)
    assert f'{os.sep}Server.exe verified' in out
