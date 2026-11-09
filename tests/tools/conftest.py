"""Fixtures for the kmap tests: a throw-away repository with mapping/ and a private workspace outside it."""
import os
import shutil
import sys
import textwrap

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'kmap'))

# The 1.0.1 candidates that the deep dives resolved. Four in the foundation deep dive (game-atlas, "Old candidates
# resolved", task 7.5): each keeps its name and address, its status rose to verified, and Server::simLoop's convention
# was corrected (it takes the std::function's lambda in ECX). One in the creature deep dive (task 8.4): it keeps its
# address and resolves through an alias. Every other 1.0.1 symbol keeps convention and status.
RESOLVED_101 = {
    'World::openSave': ('thiscall', 'verified'),
    'Map::openSave': ('thiscall', 'verified'),
    'Server::simLoop': ('thiscall', 'verified'),
    'Server::tickSite': ('cdecl', 'verified'),
    # Task 8.4: model resolver C is zone generation (cube::World::generateZone), verified by probe:model-slots.
    'ModelResolverC': ('thiscall', 'verified'),
}


def resolved_101(symbol):
    """(convention, status) a 1.0.1 map symbol must have in today's mapping."""
    return RESOLVED_101.get(symbol['name'], (symbol['convention'], symbol['status']))


FIXTURE_BUILDS = ('cube-0x51D2CE9C', 'server-0x51D2CE9B')


def fixture_workspace():
    """The repository's workspace.toml with only the two builds the fixtures are written for (the 07-02 pair), both
    targeted: the tests' entries, symbols and inputs are those builds'. Later builds and `retired` flags are dropped."""
    text = open(os.path.join(ROOT, 'mapping', 'workspace.toml'), encoding='utf-8').read()
    head, *blocks = text.split('[[build]]')
    keep = []
    for b in blocks:
        bid = next((l.split('"')[1] for l in b.splitlines() if l.startswith('id = ')), None)
        if bid in FIXTURE_BUILDS:
            keep.append('\n'.join(l for l in b.split('\n') if not l.startswith('retired = ')))
    return head + ''.join('[[build]]' + b for b in keep)


class KmapRepo:
    def __init__(self, tmp_path, monkeypatch, capsys):
        self.root = tmp_path / 'repo'
        self.home = tmp_path / 'kmap-home'
        self.mapping = self.root / 'mapping'
        (self.root / '.git').mkdir(parents=True)
        self.mapping.mkdir()
        (self.mapping / 'workspace.toml').write_text(fixture_workspace(), encoding='utf-8')
        self.monkeypatch = monkeypatch
        self.capsys = capsys
        monkeypatch.setenv('KMAP_HOME', str(self.home))
        for var in ('KMAP_GHIDRA', 'KMAP_REPO'):
            monkeypatch.delenv(var, raising=False)

    def write(self, rel, text):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(textwrap.dedent(text).lstrip('\n'), encoding='utf-8')
        return p

    def run(self, *args):
        from kmap.cli import main
        self.capsys.readouterr()
        code = main(['--repo', str(self.root), *map(str, args)])
        out = self.capsys.readouterr()
        return code, out.out, out.err

    def check(self, *extra):
        return self.run('check', '--offline', *extra)


@pytest.fixture
def kmap_repo(tmp_path, monkeypatch, capsys):
    return KmapRepo(tmp_path, monkeypatch, capsys)


def function_entry(name, address, status='candidate', evidence=('manual:test',), extra=''):
    ev = ', '.join('"' + e.replace('"', '\\"') + '"' for e in evidence)
    return (f'[[function]]\nname = "{name}"\naddress = {address}\nconvention = "thiscall"\nsubsystem = "core"\n'
            f'status = "{status}"\nevidence = [{ev}]\nsource = "kyubu"\n{extra}\n')
