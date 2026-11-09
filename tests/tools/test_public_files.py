"""The public-file check keeps Ghidra projects, type archives, decompiler dumps and game files out of the
repository (add-mapping-workspace task 1.5)."""
import os
import shutil
import subprocess
import sys

import pytest

from conftest import ROOT

CHECK = os.path.join(ROOT, 'tools', 'check_public_files.py')
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import check_public_files as cpf  # noqa: E402


@pytest.mark.parametrize('path', ['work/kyubu.gpr', 'work/kyubu.rep/idata/00/x.prp', 'cube.gzf', 'types/cube.gdt',
                                  'decomp/GameController.c', 'mapping/decomp/a.c', 'game/Cube.exe', 'data1.db'])
def test_rejected(path):
    assert cpf.problem(path)


@pytest.mark.parametrize('path', ['mapping/types/creature.toml', 'docs/mapping/setup.md', 'tools/kmap/kmap/cli.py',
                                  'decompile.md', 'mapping/atlas/core.md'])
def test_allowed(path):
    assert cpf.problem(path) is None


@pytest.mark.skipif(not shutil.which('git'), reason='git not available')
def test_decompiler_dump_committed_by_mistake(tmp_path):
    # covers: mapping-workspace/Decompiler dump committed by mistake
    repo = tmp_path / 'repo'
    (repo / 'tools').mkdir(parents=True)
    shutil.copy2(CHECK, repo / 'tools' / 'check_public_files.py')
    shutil.copy2(os.path.join(ROOT, '.gitignore'), repo / '.gitignore')
    (repo / 'decomp').mkdir()
    (repo / 'decomp' / 'GameController.c').write_text('// dump\n')
    git = ['git', '-c', 'user.email=t@example.com', '-c', 'user.name=t']
    subprocess.run(git + ['init', '-q'], cwd=repo, check=True)
    subprocess.run(git + ['add', 'tools', '.gitignore'], cwd=repo, check=True)
    ok = subprocess.run([sys.executable, 'tools/check_public_files.py'], cwd=repo, capture_output=True, text=True)
    assert ok.returncode == 0, ok.stdout
    ignored = subprocess.run(git + ['add', 'decomp/GameController.c'], cwd=repo, capture_output=True, text=True)
    assert ignored.returncode != 0          # .gitignore keeps it out of a normal `git add`
    subprocess.run(git + ['add', '-f', 'decomp/GameController.c'], cwd=repo, check=True)
    r = subprocess.run([sys.executable, 'tools/check_public_files.py'], cwd=repo, capture_output=True, text=True)
    assert r.returncode == 1
    assert 'FAIL decomp/GameController.c: decompiler output' in r.stdout


def test_repository_is_clean():
    if not os.path.isdir(os.path.join(ROOT, '.git')) or not shutil.which('git'):
        pytest.skip('not a git checkout')
    r = subprocess.run([sys.executable, CHECK], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout
