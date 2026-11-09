"""`kmap check --offline` in the public CI workflow (add-mapping-workspace task 4.8).

The workflow's own command runs here the way CI runs it: no workspace, no game files, no Ghidra and no third-party
packages. Then the same command runs on a copy of the checkout with broken files, and must fail and name them.
The full check, for the private contract level, reads the builds straight from a game folder (`--game`).
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from conftest import ROOT

WORKFLOW = Path(ROOT) / '.github' / 'workflows' / 'ci.yml'
DEV_MODS = sorted(f'mods-dev/{p.parent.name}' for p in (Path(ROOT) / 'mods-dev').glob('*/mod.toml'))   # probes name them
GAMEPLAY_MODS = sorted(f'mods-gameplay/{p.parent.name}' for p in (Path(ROOT) / 'mods-gameplay').glob('*/mod.toml'))
CHECKOUT = ['mapping', 'symbols', 'sdk', 'loader/lua', 'docs', 'mods-examples', *DEV_MODS,
            *GAMEPLAY_MODS, 'tests/fixtures/catalog-src', 'CMakeLists.txt']   # what kmap check reads (probes may name fixture mods)


def jobs():
    """{job name: its lines} from ci.yml (the workflow is plain enough to read without a YAML parser)."""
    out, name = {}, None
    in_jobs = False
    for line in WORKFLOW.read_text(encoding='utf-8').splitlines():
        if line.startswith('jobs:'):
            in_jobs = True
            continue
        if not in_jobs:
            continue
        m = re.match(r'  ([\w-]+):\s*$', line)
        if m:
            name = m.group(1)
            out[name] = []
        elif name:
            out[name].append(line)
    return out


def offline_job():
    found = [(name, lines) for name, lines in jobs().items()
             if any(re.search(r'run:\s*python3 tools/kmap check --offline\s*$', l) for l in lines)]
    assert len(found) == 1, 'ci.yml must run `python3 tools/kmap check --offline` in exactly one job'
    return found[0]


def ci_command():
    name, lines = offline_job()
    run = next(l for l in lines if 'tools/kmap check --offline' in l)
    return run.split('run:', 1)[1].split()          # ['python3', 'tools/kmap', 'check', '--offline']


def run_like_ci(tmp_path, repo=None):
    """The workflow's command in a clean environment: an empty home, no KMAP_* variables, and pefile and pyghidra
    made unimportable, so anything that needs the game files or Ghidra would fail."""
    home = tmp_path / 'home'
    blocker = tmp_path / 'blocked-modules'
    for d in (home, blocker):
        d.mkdir(exist_ok=True)
    for mod in ('pefile', 'pyghidra'):
        (blocker / f'{mod}.py').write_text(f'raise ImportError("{mod} is not installed in the CI mapping job")\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('KMAP_', 'KYUBU_', 'PYTHON'))}
    env.update(HOME=str(home), USERPROFILE=str(home), LOCALAPPDATA=str(home / 'AppData' / 'Local'),
               PYTHONPATH=str(blocker), PYTHONDONTWRITEBYTECODE='1')
    if repo:
        env['KMAP_REPO'] = str(repo)
    cmd = ci_command()
    assert cmd[0] == 'python3'
    r = subprocess.run([sys.executable, *cmd[1:]], cwd=ROOT, env=env, capture_output=True, text=True,
                       encoding='utf-8', timeout=600)
    return r, home


def test_offline_check_runs_without_game_files(tmp_path):
    name, lines = offline_job()
    text = '\n'.join(lines)
    assert re.search(r'runs-on:\s*ubuntu-', text), 'a hosted runner, not the private game runner'
    for private in ('secrets.', 'KMAP_HOME', 'KYUBU_GAME_ROOT', 'cubeworld-gpu', 'self-hosted', 'pip install'):
        assert private not in text, f'the {name} job must not use {private}'
    assert not any('needs:' in l for l in lines)

    r, home = run_like_ci(tmp_path)
    assert r.returncode == 0, r.stdout + r.stderr
    assert re.search(r'kmap offline check: .* ok$', r.stdout + r.stderr, re.M), r.stdout + r.stderr
    assert not [p for p in home.rglob('*') if p.is_file()], 'the offline check wrote into the (empty) home'


def copy_checkout(dest):
    for rel in CHECKOUT:
        src = Path(ROOT) / rel
        if src.is_dir():
            shutil.copytree(src, dest / rel, ignore=shutil.ignore_patterns('__pycache__'))
        else:
            (dest / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest / rel)


def test_offline_check_fails_on_broken_fixture(tmp_path):
    repo = tmp_path / 'checkout'
    copy_checkout(repo)
    r, _ = run_like_ci(tmp_path, repo)
    assert r.returncode == 0, 'the copy is complete: ' + r.stdout + r.stderr

    # 1. a hand edit in a generated header
    h = repo / 'sdk' / 'include' / 'cubeworld' / 'types.h'
    h.write_text(h.read_text(encoding='utf-8').replace('#ifndef', '/* hand edit */\n#ifndef', 1),
                 encoding='utf-8')
    # 2. a locked name that no longer exists in mapping/
    lock = repo / 'mapping' / 'exported-names.lock'
    text = lock.read_text(encoding='utf-8')
    assert '"Database::open" = "1.0.1"' in text
    lock.write_text(text.replace('"Database::open" = "1.0.1"', '"Database::openGone" = "1.0.1"', 1), encoding='utf-8')
    # 3. a mapping file that is no longer valid TOML
    broken = sorted((repo / 'mapping').glob('types/*.toml'))[0]
    broken.write_text(broken.read_text(encoding='utf-8') + '\n[[type]\n', encoding='utf-8')

    r, _ = run_like_ci(tmp_path, repo)
    out = r.stdout + r.stderr
    assert r.returncode == 1, out
    fails = [l for l in out.splitlines() if l.startswith('FAIL ')]
    assert any('sdk/include/cubeworld/types.h' in l and 'source of truth is mapping/' in l for l in fails), out
    assert any('exported-names.lock' in l and 'Database::openGone' in l for l in fails), out
    assert any(broken.name in l for l in fails), out
    assert re.search(r'kmap offline check: .* problem\(s\)$', out, re.M), out


# --- the full check in the private contract level: `kmap check --game <install>` -------------------------------

GAME = os.environ.get('KYUBU_GAME_DIR', os.path.expanduser('~/game'))


def full_check(tmp_path, game):
    """`kmap check --game` with an empty workspace, as the contract level runs it."""
    home = tmp_path / 'kmap-home'
    env = {k: v for k, v in os.environ.items() if not k.startswith('KMAP_')}
    env.update(KMAP_HOME=str(home), PYTHONDONTWRITEBYTECODE='1')
    r = subprocess.run([sys.executable, 'tools/kmap', 'check', '--game', str(game)], cwd=ROOT, env=env,
                       capture_output=True, text=True, encoding='utf-8', timeout=1200)
    return r, home


def hashes(folder):
    import hashlib
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in Path(folder).iterdir() if p.is_file()}


def test_full_check_refuses_another_build(tmp_path):
    game = tmp_path / 'game'
    game.mkdir()
    (game / 'Cube.exe').write_bytes(b'MZ' + bytes(1000))
    (game / 'Server.exe').write_bytes(b'MZ' + bytes(1000))
    r, home = full_check(tmp_path, game)
    out = r.stdout + r.stderr
    assert r.returncode == 1, out
    assert 'is not the supported cube-0x51D2CE9C build' in out and 'expected SHA-256' in out, out
    assert not home.exists()


@pytest.mark.skipif(not os.path.isfile(os.path.join(GAME, 'Cube.exe')), reason='no Cube World folder')
def test_full_check_from_the_game_folder(tmp_path):
    before = hashes(GAME)
    r, home = full_check(tmp_path, GAME)
    out = r.stdout + r.stderr
    assert r.returncode == 0, out
    assert re.search(r'^kmap check: .* ok$', out, re.M), out
    assert hashes(GAME) == before, 'the game folder is only read'
    assert not home.exists(), 'nothing is staged into the workspace'
