"""kmap push and pull against the real Ghidra project (add-mapping-workspace tasks 2.6, 2.7).

Needs a workspace built by `kmap ghidra install` and `kmap init` (KMAP_HOME, or the default location) and
KMAP_GHIDRA_TESTS=1. The tests rename functions in that project and put them back afterwards; mapping/ is
edited only in a throw-away copy of the repository.
"""
import os
import shutil

import pytest

from conftest import ROOT

pytestmark = pytest.mark.skipif(os.environ.get('KMAP_GHIDRA_TESTS') != '1',
                                reason='set KMAP_GHIDRA_TESTS=1 with a kmap workspace to run')
CUBE = 'cube-0x51D2CE9C'
# A function nobody has mapped (FUN_0049d810 in the pinned analysis). The spec's example, FUN_0049a2f0, is not a
# function start there: 0x49A2F0 lies inside cube::GameController::update (0x489130).
UNNAMED = 0x49D810


@pytest.fixture
def ctx():
    from kmap.config import Context
    c = Context(ROOT)
    if not (c.project_dir / 'kyubu.gpr').is_file():
        pytest.skip('no Ghidra project in the workspace')
    return c


def run(repo, *args, capsys=None):
    from kmap.cli import main
    code = main(['--repo', str(repo), *args])
    out = capsys.readouterr() if capsys else None
    return code, (out.out + out.err) if out else ''


def rename(ctx, address, name):
    from kmap import ghidra
    return ghidra.run_worker(ctx, 'rename', {'project': str(ctx.project_dir), 'build': CUBE, 'address': address,
                                             'name': name}, quiet=True)['name']


@pytest.fixture
def repo_copy(tmp_path):
    repo = tmp_path / 'repo'
    (repo / '.git').mkdir(parents=True)
    for d in ('mapping', 'symbols', 'tools'):
        shutil.copytree(os.path.join(ROOT, d), repo / d)
    return repo


def test_idempotent_push(ctx, capsys):
    # covers: mapping-database/Idempotent push
    run(ROOT, 'push', capsys=capsys)
    code, out = run(ROOT, 'push', capsys=capsys)
    assert code == 0 and 'kmap push: 0 change(s)' in out, out


def test_rename_in_the_gui(ctx, repo_copy, capsys):
    # covers: mapping-database/Rename in the GUI (done by a headless script, as a GUI user would)
    run(ROOT, 'push', capsys=capsys)
    before = rename(ctx, UNNAMED, None)
    assert before.startswith('FUN_')
    try:
        rename(ctx, UNNAMED, 'cube::Creature::takeDamage')
        target = repo_copy / 'mapping' / CUBE / 'functions' / 'creatures.toml'
        old = target.read_text(encoding='utf-8')
        code, out = run(repo_copy, 'pull', capsys=capsys)
        assert '+name = "cube::Creature::takeDamage"' in out and f'+address = 0x{UNNAMED:X}' in out, out
        assert f'mapping/{CUBE}/functions/creatures.toml' in out
        assert target.read_text(encoding='utf-8') == old            # shown, not written
        code, out = run(repo_copy, 'pull', '--write', capsys=capsys)
        assert code == 0, out
        text = target.read_text(encoding='utf-8')
        entry = text[text.index('name = "cube::Creature::takeDamage"'):]
        assert f'address = 0x{UNNAMED:X}' in entry and 'status = "guess"' in entry
        code, out = run(repo_copy, 'check', '--offline', capsys=capsys)
        assert 'takeDamage' not in out, out                        # a valid guess entry
    finally:
        rename(ctx, UNNAMED, None)


def test_conflict(ctx, repo_copy, capsys):
    # covers: mapping-database/Conflict
    run(ROOT, 'push', capsys=capsys)
    f = repo_copy / 'mapping' / CUBE / 'functions' / 'world.toml'
    f.write_text(f.read_text(encoding='utf-8').replace('name = "cube::World::generateZone"', 'name = "cube::World::generateZoneX"'),
                 encoding='utf-8')
    snapshot = {p: p.read_bytes() for p in (repo_copy / 'mapping').rglob('*.toml')}
    try:
        rename(ctx, 0x5E3280, 'cube::Thing::resolve')
        code, out = run(repo_copy, 'pull', '--write', capsys=capsys)
        assert code == 1
        line = next(l for l in out.splitlines() if l.startswith('CONFLICT'))
        assert "'cube::World::generateZoneX'" in line and "'cube::Thing::resolve'" in line and '0x5E3280' in line
        assert {p: p.read_bytes() for p in (repo_copy / 'mapping').rglob('*.toml')} == snapshot
    finally:
        rename(ctx, 0x5E3280, 'cube::World::generateZone')
