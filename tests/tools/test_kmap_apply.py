"""mapping/ghidra/mappings.json and the standalone mapping/ghidra/apply.py (add-mapping-workspace task 4.6).

The export and the script's build check run anywhere. The Ghidra tests import a binary into a fresh project under
$KMAP_HOME/tmp and run the committed apply.py on it, as a modder would; they need KMAP_GHIDRA_TESTS=1 and a
workspace with Ghidra installed (`kmap ghidra install`). The modder's-own-project test also needs the verified
Cube.exe and Server.exe of `kmap init`. It imports without auto-analysis (the script creates the functions; about
30 s per build) unless KMAP_APPLY_ANALYSE=1, which analyses first as the Ghidra import dialog offers (about 10
minutes for Cube.exe and 7 for Server.exe).
"""
import importlib.util
import json
import os
import shutil

import pytest

from conftest import ROOT
from test_kmap_docs import write_fixture
from test_kmap_headers import CUBE

SERVER = 'server-0x51D2CE9B'
MAPPINGS = os.path.join(ROOT, 'mapping', 'ghidra', 'mappings.json')
SCRIPT = os.path.join(ROOT, 'mapping', 'ghidra', 'apply.py')
ghidra_tests = pytest.mark.skipif(os.environ.get('KMAP_GHIDRA_TESTS') != '1',
                                  reason='set KMAP_GHIDRA_TESTS=1 with a kmap workspace to run')


def apply_module():
    spec = importlib.util.spec_from_file_location('kyubu_apply', SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)                                  # not __main__: nothing runs
    return mod


def committed():
    with open(MAPPINGS, encoding='utf-8') as f:
        return json.load(f)


# --- mappings.json ----------------------------------------------------------------------------------------

def test_mappings_json(kmap_repo):
    write_fixture(kmap_repo)
    code, out, err = kmap_repo.run('export', '--offline')
    assert code == 0, out + err
    path = kmap_repo.root / 'mapping' / 'ghidra' / 'mappings.json'
    text = path.read_text(encoding='utf-8')
    doc = json.loads(text)
    from kmap.config import Context
    ws = Context(kmap_repo.root).ws
    assert doc['format'] == 1 and doc['ghidra'] == ws.ghidra.version
    assert list(doc['builds']) == list(ws.builds)
    cube = doc['builds'][CUBE]
    assert (cube['file'], cube['sha256'], cube['image_base']) == ('Cube.exe', ws.build(CUBE).sha256.lower(), '0x400000')
    symbols = {s['name']: s for s in cube['symbols']}
    update = symbols['cube::Creature::update']
    assert update['kind'] == 'function' and update['address'] == '0x4A2000'
    assert update['aliases'] == ['Creature::update']
    assert update['prototype'] == {'conv': 'thiscall', 'name': 'cube::Creature::update', 'ret': 'void',
                                   'params': [['cube::Creature*', 'this'], ['int32_t', 'dt_ms']]}
    assert update['comment'].startswith('kyubu: verified (kyubu)\nevidence: vtable:cube::Creature:0')
    assert symbols['cube::Creature::vftable'] == {'address': '0x6FC8CC', 'aliases': [], 'kind': 'label',
                                                  'name': 'cube::Creature::vftable',
                                                  'comment': 'kyubu: vtable of cube::Creature (verified, 2 slots)'}
    assert symbols['cube::helper']['prototype'] is None and symbols['cube::helper']['convention'] == 'thiscall'
    types = [t['name'] for t in cube['types']]
    assert types[0] == 'cube::Mode'                                            # enums first
    assert types.index('cube::Base') < types.index('cube::Thing') and types.index('cube::Vec') < types.index('cube::Thing')
    thing = next(t for t in cube['types'] if t['name'] == 'cube::Thing')
    assert thing['base'] == 'cube::Base' and thing['size'] == 0x20
    assert [(f['offset'], f['name'], f['type'], f['comment']) for f in thing['fields']] == [
        (8, 'hp', 'float', 'verified'), (0xC, 'pos', 'cube::Vec', 'candidate'), (0x18, 'label', 'std::string*', 'candidate')]
    # guesses stay in the private project
    assert 'secretGuess' not in text and '"secret"' not in text and '0x4A2100' not in text
    # one item per line; deterministic; a hand edit is caught
    rows = [line for line in text.splitlines() if line.startswith('    {')]
    assert len(rows) == sum(len(b['types']) + len(b['symbols']) for b in doc['builds'].values())
    assert all(json.loads(line.rstrip(',')) for line in rows)
    code, out, err = kmap_repo.run('export', '--offline')
    assert '0 changed' in out
    path.write_text(text.replace('"0x4A2000"', '"0x4A2001"'), encoding='utf-8')
    code, out, err = kmap_repo.check()
    assert code == 1 and 'mapping/ghidra/mappings.json' in out and 'the source of truth is mapping/' in out


def test_committed_mappings_cover_every_exported_entry():
    from kmap import db as dbmod
    from kmap.config import Context
    ctx = Context(ROOT)
    db = dbmod.load(ctx)
    doc = committed()
    for bid in ctx.ws.builds:
        b = doc['builds'][bid]
        names = {s['name'] for s in b['symbols']}
        want = {e['name'] for e in db.functions(bid) + db.globals(bid) if dbmod.exported(e)}
        assert want and want <= names, sorted(want - names)[:5]
        assert {t['name'] for t in b['types']} == {t['name'] for t in db.types + db.enums
                                                   if dbmod.exported(t) and bid in t.get('builds', [bid])}
        assert b['sha256'] == ctx.ws.build(bid).sha256.lower()


# --- the build check, without Ghidra ------------------------------------------------------------------------

def test_build_check():
    # covers: mapping-exports/Wrong binary (the check and the message; the Ghidra run is test_wrong_binary)
    m = apply_module()
    doc = committed()
    cube = doc['builds'][CUBE]
    assert m.match_build(doc, cube['sha256'].upper()) == (CUBE, cube)
    assert m.match_build(doc, '0' * 64) == (None, None)
    assert m.match_build(doc, None) == (None, None)
    msg = m.wrong_binary(doc, '0' * 64)
    assert msg.startswith('kyubu: nothing applied: this program (SHA-256 ' + '0' * 64 + ')')
    for bid, b in doc['builds'].items():
        assert f'{b["file"]} ({bid}): SHA-256 {b["sha256"]}' in msg


def test_other_format_is_refused(tmp_path):
    m = apply_module()
    p = tmp_path / 'mappings.json'
    p.write_text(json.dumps({'format': 2, 'builds': {}}), encoding='utf-8')
    with pytest.raises(ValueError, match='format 2'):
        m.load(str(p))


# --- in Ghidra ----------------------------------------------------------------------------------------------

@pytest.fixture
def ctx():
    from kmap import ghidra
    from kmap.config import Context
    c = Context(ROOT)
    try:
        ghidra.require_ghidra(c)
    except Exception as e:
        pytest.skip(f'no Ghidra in the workspace: {e}')
    return c


@pytest.fixture
def fresh_project(ctx):
    p = ctx.home / 'tmp' / 'apply-test-project'           # on the workspace's disk, not in the repository
    shutil.rmtree(p, ignore_errors=True)
    yield p
    shutil.rmtree(p, ignore_errors=True)


def run_apply(ctx, project, binary, runs, name, addresses=(), types=(), analyse=False):
    from kmap import ghidra
    return ghidra.run_worker(ctx, 'apply_script', {
        'project': str(project), 'binary': str(binary), 'name': name, 'script': SCRIPT, 'runs': runs,
        'language': ctx.ws.analysis['language'], 'compiler': ctx.ws.analysis['compiler'],
        'options': ctx.ws.analysis.get('options', {}), 'analyse': analyse,
        'addresses': list(addresses), 'types': list(types)}, quiet=True)


@ghidra_tests
def test_wrong_binary(ctx, fresh_project, tmp_path):
    # covers: mapping-exports/Wrong binary
    binary = tmp_path / 'NotCube.exe'
    binary.write_bytes(bytes(range(256)) * 64)
    doc = committed()
    r = run_apply(ctx, fresh_project, binary, [[MAPPINGS]], 'NotCube.exe', types=['cube::CreatureData'])
    run = r['runs'][0]
    assert 'kyubu: nothing applied' in run['stderr'], run
    assert f'this program (SHA-256 {r["sha256"].lower()})' in run['stderr']
    for bid, b in doc['builds'].items():                                    # names the expected hashes
        assert f'{b["file"]} ({bid}): SHA-256 {b["sha256"]}' in run['stderr']
    assert not run['modified'] and 'kyubu: applied' not in run['stdout']   # applies nothing
    assert run['shown']['types'] == {'cube::CreatureData': None}


@ghidra_tests
@pytest.mark.parametrize('bid', [CUBE, SERVER])
def test_a_modders_own_project(ctx, fresh_project, bid):
    # covers: mapping-exports/A modder's own project (and applying twice changes nothing)
    build = ctx.ws.build(bid)
    binary = ctx.input_path(build)
    if not binary.is_file():
        pytest.skip(f'{binary} not found: run `kmap init --game <folder>`')
    b = committed()['builds'][bid]
    functions = [s for s in b['symbols'] if s['kind'] == 'function']
    others = [s for s in b['symbols'] if s['kind'] != 'function']
    r = run_apply(ctx, fresh_project, binary, [[MAPPINGS], [MAPPINGS]], build.file,
                  addresses=[int(s['address'], 16) for s in b['symbols']], types=[t['name'] for t in b['types']],
                  analyse=os.environ.get('KMAP_APPLY_ANALYSE') == '1')
    assert r['sha256'] == build.sha256.lower()
    first, second = r['runs']
    assert first['stderr'] == '', first['stderr']
    report = [line for line in first['stdout'].splitlines() if line.startswith('kyubu: applied ')]
    assert report and report[0].startswith(
        f'kyubu: applied {len(functions)} functions ({sum(1 for s in functions if s["prototype"])} with prototypes), '
        f'{len(others)} labels, 0 globals and {len(b["types"])} types to {build.file} ({bid}): '), first['stdout']
    assert ', 0 change(s)' not in report[0]
    shown = first['shown']
    for s in b['symbols']:                                                    # every exported name
        got = shown['symbols'][s['address']]
        if s['kind'] == 'function':
            assert got['function'] == s['name'], (s['name'], got)
        assert s['name'] in got['names'] and set(s['aliases']) <= set(got['names']), (s['name'], got)
        assert got['plate'] == s['comment'], (s['name'], got)
    for s in functions:
        if s['prototype']:
            got = shown['symbols'][s['address']]['prototype']
            assert s['prototype']['conv'] in got, (s['name'], got)
            if s['prototype']['conv'] == 'thiscall' and '::' in s['name']:
                assert '(void * this' not in got, (s['name'], got)            # a method: `this` is its class
    for t in b['types']:                                                      # every exported type
        got = shown['types'][t['name']]
        assert got and got['kind'] == t['kind'], (t['name'], got)
        if t['kind'] == 'enum':
            assert sorted(got['values']) == sorted(t['values'])
        else:
            assert got['size'] == t['size'], (t['name'], got)
            assert {(f['offset'], f['name']) for f in t['fields']} <= {(c[0], c[1]) for c in got['fields']}
    # the second run changes nothing
    assert second['stderr'] == '', second['stderr']
    assert ': 0 change(s), already applied, 0 warning(s)' in second['stdout'], second['stdout']
    assert not second['modified']
    assert second['shown'] == shown
