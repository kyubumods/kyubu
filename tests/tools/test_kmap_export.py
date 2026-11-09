"""kmap export and the staleness checks (add-mapping-workspace tasks 4.1, 4.2).

The unique-signature test needs the real Cube.exe: KYUBU_GAME_DIR (default ~/game), skipped otherwise.
"""
import os
import shutil
import tomllib
from pathlib import Path

import pytest

from conftest import ROOT, function_entry, resolved_101

CUBE = 'cube-0x51D2CE9C'
SERVER = 'server-0x51D2CE9B'
FIXTURE_101 = os.path.join(ROOT, 'tests', 'tools', 'fixtures', 'symbols-1.0.1')
GAME = os.environ.get('KYUBU_GAME_DIR', os.path.expanduser('~/game'))


@pytest.fixture
def real_copy(kmap_repo):
    """The repository's mapping/ and generated files in a throw-away checkout."""
    shutil.rmtree(kmap_repo.mapping)
    shutil.copytree(os.path.join(ROOT, 'mapping'), kmap_repo.mapping)
    shutil.copytree(os.path.join(ROOT, 'symbols'), kmap_repo.root / 'symbols')
    dev_mods = sorted(f'mods-dev/{p.parent.name}' for p in (Path(ROOT) / 'mods-dev').glob('*/mod.toml'))
    gameplay_mods = sorted(f'mods-gameplay/{p.parent.name}' for p in (Path(ROOT) / 'mods-gameplay').glob('*/mod.toml'))
    for d in ('sdk/include/cubeworld', 'docs/game', 'sdk/lua', 'loader/lua', 'mods-examples', 'tests/fixtures/catalog-src', *dev_mods,
              *gameplay_mods):
        if os.path.isdir(os.path.join(ROOT, d)):
            shutil.copytree(os.path.join(ROOT, d), kmap_repo.root / d)
    return kmap_repo


def load_map(path):
    return tomllib.load(open(path, 'rb'))['symbol']


@pytest.mark.parametrize('build', [CUBE, SERVER])
def test_migration_round_trip(build):
    # covers: mapping-exports/Migration round trip
    old = load_map(os.path.join(FIXTURE_101, f'{build}.toml'))
    new = {s['name']: s for s in load_map(os.path.join(ROOT, 'symbols', f'{build}.toml'))}
    for s in old:
        n = new.get(s['name'])
        assert n is not None, f'{s["name"]} was removed'
        assert n['address'] == s['address'], (s['name'], 'address')
        assert (n['convention'], n['status']) == resolved_101(s), s['name']   # only the 7.5 resolutions changed
        assert n['signature'].startswith(s['signature']), f'{s["name"]}: signature changed, not extended'
    for s in new.values():
        if s['notes'].startswith('deprecated alias of') if 'notes' in s else False:
            target = new[s['notes'].split('deprecated alias of ', 1)[1]]
            assert (s['address'], s['signature'], s['status']) == (target['address'], target['signature'],
                                                                   target['status'])


def test_globals_have_their_own_array():
    # The maps hold every verified and candidate global, in [[global]]: symgen and the 1.0.1 loader read only
    # [[symbol]], where a variable (no signature, outside .text) would be reported as a broken symbol.
    from kmap import db as dbmod
    from kmap.config import Context
    db = dbmod.load(Context(ROOT))
    for build in (CUBE, SERVER):
        data = tomllib.load(open(os.path.join(ROOT, 'symbols', f'{build}.toml'), 'rb'))
        want = {g['name'] for g in db.globals(build) if dbmod.exported(g)}
        assert {g['name'] for g in data.get('global', [])} == want
        assert not want & {s['name'] for s in data['symbol']}
        assert all('signature' not in g and 'convention' not in g for g in data.get('global', []))
    assert 'cube::game_controller' in {g['name'] for g in db.globals(CUBE)}


def test_committed_artifacts_are_current(real_copy):
    code, out, err = real_copy.check()
    assert code == 0, out + err


def test_hand_edit(real_copy):
    # covers: mapping-exports/Hand edit
    p = real_copy.root / 'symbols' / f'{CUBE}.toml'
    p.write_text(p.read_text().replace('address = 0x489130', 'address = 0x489140', 1))
    code, out, err = real_copy.check()
    assert code == 1
    assert f'symbols/{CUBE}.toml: differs from what `kmap export` generates; the source of truth is mapping/' in out


def test_guess_stays_private(real_copy):
    # covers: mapping-database/Guess stays private (and mapping-exports: nothing below candidate is exported)
    f = real_copy.mapping / CUBE / 'functions' / 'core.toml'
    f.write_text(f.read_text() + '\n' + function_entry('cube::Creature::secretGuess', '0x4A1230', 'guess'))
    code, out, err = real_copy.run('export', '--offline')
    assert code == 0, out + err
    for p in real_copy.root.rglob('*'):
        if p.is_file() and 'mapping' not in p.relative_to(real_copy.root).parts[:1]:
            assert 'secretGuess' not in p.read_text(errors='replace'), p
    from kmap import db as dbmod
    from kmap.config import Context
    from kmap.roundtrip import desired
    db = dbmod.load(Context(real_copy.root))
    assert any(i['name'] == 'cube::Creature::secretGuess' for i in desired(db, CUBE)['symbols'])   # push applies it


def test_new_entry_needs_a_signature_offline(real_copy):
    f = real_copy.mapping / CUBE / 'functions' / 'core.toml'
    f.write_text(f.read_text() + '\n' + function_entry('cube::World::tick', '0x4A1230', 'candidate'))
    code, out, err = real_copy.check()
    assert code == 1 and 'cube::World::tick has no signature yet' in out


# --- signatures -------------------------------------------------------------------------------------------

class FakeSigner:
    def __new__(cls, text, base=0x401000):
        from kmap.export import Signer
        s = object.__new__(Signer)
        s.text = text
        s.wild = set()
        s.tokens = lambda va, n: list(text[va - base:va - base + n])
        return s


def test_signature_is_extended_until_unique():
    # covers: mapping-exports/Unique signature (synthetic)
    prologue = bytes([0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x10, 0x20, 0x30, 0x40, 0x64, 0xA1, 0, 0, 0, 0, 0x50,
                      0x83, 0xEC, 0x10])
    text = b''.join(prologue + bytes([0x90] * 10) + bytes([i]) + bytes([0xCC] * 33) for i in range(50))
    s = FakeSigner(text)
    target = 0x401000 + 7 * 64
    sig, unique = s.signature(target)
    assert unique and len(sig.split()) == 31          # 20 shared + 10 shared + the distinguishing byte
    assert s.count([int(x, 16) for x in sig.split()]) == 1
    twins = FakeSigner(prologue * 10 + bytes([0xCC] * 64))
    sig, unique = twins.signature(0x401000)
    assert not unique and len(sig.split()) == 20


@pytest.mark.skipif(not os.path.isfile(os.path.join(GAME, 'Cube.exe')), reason='no Cube World folder')
def test_unique_signature_on_the_game():
    # covers: mapping-exports/Unique signature (real Cube.exe: GameController::update starts with the SEH prologue)
    from kmap.config import Context
    from kmap.export import Signer
    ctx = Context(ROOT)
    s = Signer(open(os.path.join(GAME, 'Cube.exe'), 'rb').read(), ctx.ws.build(CUBE))
    toks = s.tokens(0x489130, 20)
    assert s.count(toks, limit=100) > 1
    sig, unique = s.signature(0x489130)
    assert unique and 20 < len(sig.split()) <= 64
    assert s.count([None if t == '??' else int(t, 16) for t in sig.split()]) == 1
