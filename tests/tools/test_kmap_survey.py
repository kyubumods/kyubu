"""kmap survey, its query index and kmap import-cuwo (add-mapping-workspace tasks 3.1 to 3.6).

The synthetic tests run anywhere. The scenarios on the real builds read $KMAP_HOME/survey/ (made by
`kmap survey`) and are skipped without it; the re-run scenario runs `kmap survey` twice and also needs
KMAP_GHIDRA_TESTS=1.
"""
import hashlib
import itertools
import json
import os
import shutil
import struct
import subprocess
import tomllib
from types import SimpleNamespace

import pytest

from conftest import ROOT

CUBE = 'cube-0x51D2CE9C'
SERVER = 'server-0x51D2CE9B'


# --- helpers --------------------------------------------------------------------------------------------------

class FakeBuild:
    image_base = 0x400000

    def in_image(self, a):
        return 0x400000 <= a < 0x800000

    def in_text(self, a):
        return 0x401000 <= a < 0x500000


def fake_image(*sections):
    from kmap import survey
    img = survey.Image.__new__(survey.Image)
    img.build = FakeBuild()
    img.sections = list(sections)
    return img


UNIQUE = itertools.count()


def fact(entry, **kw):
    """One record of the facts pass; the hashes are unique unless given."""
    n = next(UNIQUE)
    f = {'entry': entry, 'name': f'FUN_{entry:08x}', 'name_source': 'DEFAULT', 'thunk_target': None, 'data_refs': [],
         'calls': [], 'imports': [], 'instructions': 10, 'call_seq': [], 'ranges': [[entry, entry + 0x10]],
         'vtable_stores': [], 'bytes_sha1': f'bytes-{n}', 'mnemonics_sha1': f'mnemonics-{n}'}
    f.update(kw)
    return f


class FakeDb:
    def __init__(self, functions=None, classes=None):
        self._functions = functions or {}
        self._classes = classes or {}
        self.types = []

    def functions(self, bid):
        return self._functions.get(bid, [])

    def symbols(self, bid):
        return self.functions(bid)

    def classes(self, bid):
        return self._classes.get(bid, [])


def mapped(name, address, status='verified', subsystem='core'):
    return {'name': name, 'address': address, 'status': status, 'subsystem': subsystem, 'convention': 'thiscall',
            '_kind': 'function'}


# --- C-source scanner (SQLite identification) --------------------------------------------------------------------

C_SOURCE = r'''/* a comment with { braces } and "quotes" */
static int helper(int x){
  return x + 1;
}
#define MY_CORRUPT SQLITE_CORRUPT_BKPT
int sqlite3_thing(sqlite3 *db, const char *z){
  if( z==0 ) return MY_CORRUPT;
#if defined(A)
  if( db ){
#else
  if( !db ){
#endif
    helper(1);
  }
  return report("unique " "message: %s", z);  // trailing { comment
}
int report(const char *fmt, ...){
  return SQLITE_MISUSE_BKPT;
}
int sqlite3_wrap(void){
  return sqlite3_thing(0, 0x10);
}
'''


def test_csource_functions():
    from kmap import csource
    funcs = {f['name']: f for f in csource.functions(C_SOURCE)}
    assert list(funcs) == ['helper', 'sqlite3_thing', 'report', 'sqlite3_wrap']
    assert (funcs['helper']['start'], funcs['helper']['end'], funcs['helper']['static']) == (2, 4, True)
    # the #if/#else alternative openings count once, so the function ends at its own closing brace
    assert (funcs['sqlite3_thing']['start'], funcs['sqlite3_thing']['end']) == (6, 16)
    assert funcs['sqlite3_thing']['strings'] == ['unique message: %s']
    assert funcs['sqlite3_thing']['calls'] == ['helper', 'report']
    assert not funcs['sqlite3_thing']['static']


def test_csource_bkpt_lines_and_wrappers():
    from kmap import csource
    # line 7 uses a macro defined on top of SQLITE_CORRUPT_BKPT, line 18 the macro itself; the #define is skipped
    assert csource.bkpt_lines(C_SOURCE) == [7, 18]
    funcs = csource.functions(C_SOURCE)
    assert csource.wrappers(funcs)['sqlite3_wrap'] == ('sqlite3_thing', [0, 0x10])
    assert 'sqlite3_thing' not in csource.wrappers(funcs)


def test_csource_scrub_and_unescape():
    from kmap import csource
    clean, literals = csource.scrub('a /* x\ny */ "q\\"{" // c\nb')
    assert clean.count('\n') == 2 and '{' not in clean and 'x' not in clean
    assert [(s, line) for _, s, line in literals] == [('q"{', 2)]
    assert csource.unescape(r'\x41\101\n\\') == 'AA\n\\'


# --- 3.2 RTTI ---------------------------------------------------------------------------------------------------

RD = 0x700000
VFT = 0x6F0000          # type_info's vftable


def rtti_image():
    """.rdata with Base and Derived : Base (type descriptors, hierarchy, locators, vtables)."""
    rd = bytearray(0x140)

    def put(va, *dwords):
        struct.pack_into(f'<{len(dwords)}I', rd, va - RD, *dwords)
    put(0x700000, VFT, 0)
    rd[0x08:0x08 + 11] = b'.?AVBase@@\0'
    put(0x700020, VFT, 0)
    rd[0x28:0x28 + 14] = b'.?AVDerived@@\0'
    put(0x700040, 0, 0, 1, 0x700050)                # Base: hierarchy (1 base: itself)
    put(0x700050, 0x700060)
    put(0x700060, 0x700000)
    put(0x700080, 0, 0, 2, 0x700090)                # Derived: hierarchy (itself, Base)
    put(0x700090, 0x7000A0, 0x700060)
    put(0x7000A0, 0x700020)
    put(0x7000C0, 0, 0, 0, 0x700000, 0x700040)      # complete object locators
    put(0x7000E0, 0, 0, 0, 0x700020, 0x700080)
    put(0x700100, 0x7000C0, 0x401000, 0x401010, 0)  # Base's vtable at 0x700104
    put(0x700110, 0x7000E0, 0x401020, 0x401010, 0x401030, 0)
    return fake_image(('.text', 0x401000, bytes(0x100)), ('.rdata', RD, bytes(rd)))


def test_rtti_pass():
    from kmap import survey
    facts = [fact(0x401100, vtable_stores=[[0, 0x700104]]),
             fact(0x401200, vtable_stores=[[0, 0x700114]], calls=[0x401100]),
             fact(0x401300, call_seq=[['??2@YAPAXI@Z', [0x18]], [0x401200, []]], calls=[0x401200])]
    out = survey.pass_rtti(rtti_image(), facts, {'type_descriptors': {}})
    assert (out['type_info_vftable'], out['types']) == (VFT, 2)
    base, der = out['classes']
    assert (base['name'], base['mangled'], base['type_descriptor'], base['bases']) == ('Base', '.?AVBase@@', RD, [])
    assert base['vtables'] == [{'address': 0x700104, 'offset': 0, 'slots': [0x401000, 0x401010]}]
    assert base['stores_vtable'] == [[0x401100, 0]] and base['sizes'] == {}
    assert (der['name'], der['bases']) == ('Derived', ['Base'])
    assert der['vtables'] == [{'address': 0x700114, 'offset': 0, 'slots': [0x401020, 0x401010, 0x401030]}]
    # constructor candidate: stores the vtable at [this+0]; size: the constant passed to operator new before it
    assert der['stores_vtable'] == [[0x401200, 0]] and der['sizes'] == {'24': 1}


def test_demangle_simple():
    from kmap.survey import demangle_simple
    assert demangle_simple('.?AVGameController@cube@@') == 'cube::GameController'
    assert demangle_simple('.?AU_Container_base@std@@') == 'std::_Container_base'
    assert demangle_simple('.?AV?$vector@HV?$allocator@H@std@@@std@@') is None


# --- 3.3 libraries ----------------------------------------------------------------------------------------------

def test_stl_helper_and_library_labels(tmp_path):
    # covers: binary-survey/STL helper (synthetic)
    from kmap import survey
    text = bytearray(0x1000)
    text[0x508:0x50C] = b'OggS'                       # cmp dword [ebx], 'OggS' inside 0x401500
    img = fake_image(('.text', 0x401000, bytes(text)))
    facts = [fact(0x401000, data_refs=[0x700000]),                  # uses `vector<T> too long`
             fact(0x401100, calls=[0x401000], instructions=5),      # only forwards to it
             fact(0x401200, calls=[0x401000, 0x401300]),            # game code that also calls it
             fact(0x401300),
             fact(0x401400, name='Unwind@00401400', name_source='ANALYSIS'),
             fact(0x401500, ranges=[[0x401500, 0x401520]]),
             fact(0x401600, name='_strlen', name_source='ANALYSIS'),
             fact(0x401700, name='cube::Game::run', name_source='USER_DEFINED')]
    strings = {0x700000: {'text': 'vector<T> too long'}}
    libs = survey.pass_libraries(SimpleNamespace(home=tmp_path), img, facts, strings)
    labels = {int(k): (v['library'], v['name']) for k, v in libs['functions'].items()}
    assert labels == {0x401000: ('stl', None), 0x401100: ('stl', None), 0x401400: ('msvc-eh', None),
                      0x401500: ('ogg', None), 0x401600: ('crt', '_strlen')}
    assert libs['counts'] == {'crt': 1, 'msvc-eh': 1, 'ogg': 1, 'stl': 2}
    assert 'SQLite amalgamation 3.7.15.2 not found' in libs['notes'][0]
    cov = survey.pass_coverage(None, FakeDb(), CUBE, facts, libs, {'types': 0, 'classes': []})
    # the STL helper is library code, not unnamed game code
    assert cov['functions'] == {'total': 8, 'library': 5, 'game': 3, 'named': 0, 'typed': 0}


# --- 3.4 strings and imports ------------------------------------------------------------------------------------

def strings_image():
    rd = bytearray(0x100)
    rd[0x84:0x8D] = b'data1.db\0'
    rd[0xA0:0xAB] = b'Save/%s.cub'
    rd[0xC0:0xCC] = 'Menu'.encode('utf-16-le') + b'\0\0\0\0'
    rd[0xE0:0xE3] = b'abc'                             # too short
    return fake_image(('.rdata', 0x702E00, bytes(rd)))


def test_strings_and_imports_index():
    from kmap import survey
    facts = [fact(0x4ED0B0, data_refs=[0x702E84, 0x702EC0]),
             fact(0x4C85C0, imports=['d3d9.dll!Direct3DCreate9']),
             fact(0x4C9000, calls=[0x5D0000]),
             fact(0x5D0000, thunk_target='d3d9.dll!Direct3DCreate9', imports=['d3d9.dll!Direct3DCreate9'])]
    strings, imports = survey.pass_strings(strings_image(), facts)
    by_text = {s['text']: s for s in strings['strings']}
    assert set(by_text) == {'data1.db', 'Save/%s.cub', 'Menu'}
    db = by_text['data1.db']
    assert (db['address'], db['encoding'], db['functions'], db['hints']) == (0x702E84, 'ascii', [0x4ED0B0], ['saves'])
    assert by_text['Save/%s.cub']['hints'] == ['log', 'rendering', 'saves']
    assert (by_text['Menu']['encoding'], by_text['Menu']['hints']) == ('utf16', ['ui'])
    # direct callers and callers through the thunk; the thunk itself is not a caller
    assert imports == {'imports': {'d3d9.dll!Direct3DCreate9': [0x4C85C0, 0x4C9000]}}


def test_survey_query(kmap_repo):
    kmap_repo.write(f'mapping/{CUBE}/functions/rendering.toml',
                    '[[function]]\nname = "ModelLoader::loadAll"\naddress = 0x4ED0B0\nconvention = "thiscall"\n'
                    'subsystem = "rendering"\nstatus = "verified"\nevidence = ["string:\\"data1.db\\"", '
                    '"manual:test"]\nsource = "kyubu"\n')
    from kmap import survey
    facts = [fact(0x4ED0B0, data_refs=[0x702E84]), fact(0x4C85C0, imports=['d3d9.dll!Direct3DCreate9'])]
    strings, imports = survey.pass_strings(strings_image(), facts)
    d = kmap_repo.home / 'survey' / CUBE
    d.mkdir(parents=True)
    for name, obj in (('functions.json', facts), ('strings.json', strings), ('imports.json', imports)):
        (d / name).write_text(json.dumps(obj), encoding='utf-8')
    code, out, err = kmap_repo.run('survey', 'query', '--string', 'data1.db')
    assert code == 0, out + err
    assert '0x702E84' in out and 'referenced by 0x4ED0B0 ModelLoader::loadAll' in out
    code, out, err = kmap_repo.run('survey', 'query', '--import', 'D3D9.DLL!Direct3DCreate9')
    assert code == 0 and 'called by 0x4C85C0 FUN_004c85c0' in out
    assert kmap_repo.run('survey', 'query', '--string', 'no such text')[0] == 1


# --- 3.5 correlation ---------------------------------------------------------------------------------------------

def test_correlation_matchers_and_proposals():
    # covers: binary-survey/Name proposed across builds (synthetic)
    from kmap import survey
    cube = [fact(0x401000, bytes_sha1='same', call_seq=[[0x403000, []]]),
            fact(0x449D90, bytes_sha1='c1', mnemonics_sha1='same-ins'),
            fact(0x402000, data_refs=[0x700000], instructions=20),
            fact(0x403000, instructions=30),
            fact(0x404000, bytes_sha1='twice'), fact(0x404100, bytes_sha1='twice'),
            fact(0x405000, bytes_sha1='tiny', instructions=3)]
    server = [fact(0x401000, bytes_sha1='same', call_seq=[[0x403100, []]]),
              fact(0x412A10, bytes_sha1='s1', mnemonics_sha1='same-ins'),
              fact(0x402000, data_refs=[0x600000], instructions=18),
              fact(0x403100, instructions=29),
              fact(0x404000, bytes_sha1='twice'),
              fact(0x405000, bytes_sha1='tiny', instructions=3)]
    corr = survey.pass_correlation(None, cube, server, [{'address': 0x700000, 'text': 'a unique text'}],
                                   [{'address': 0x600000, 'text': 'a unique text'}])
    got = {(m['cube'], m['server']): m['matcher'] for m in corr['matches']}
    assert got == {(0x401000, 0x401000): 'exact-bytes', (0x449D90, 0x412A10): 'exact-instructions',
                   (0x402000, 0x402000): 'unique-string', (0x403000, 0x403100): 'call-graph'}
    assert corr['counts'] == {'call-graph': 1, 'exact-bytes': 1, 'exact-instructions': 1, 'unique-string': 1}
    assert (corr['server_functions'], corr['cube_functions']) == (6, 7)
    assert all(0 < m['score'] <= 1 for m in corr['matches'])

    db = FakeDb({CUBE: [mapped('cube::Database::get', 0x449D90), mapped('cube::Guessed', 0x401000, 'guess')],
                 SERVER: [mapped('cube::Other', 0x402000)]})
    props = survey.correlation_proposals(None, db, corr)
    assert [(p['build'], p['address'], p['name'], p['status'], p['evidence']) for p in props] == [
        (CUBE, 0x402000, 'cube::Other', 'candidate', f'correlated:{SERVER}:cube::Other'),
        (SERVER, 0x412A10, 'cube::Database::get', 'candidate', f'correlated:{CUBE}:cube::Database::get')]


# --- 3.6 cuwo import ---------------------------------------------------------------------------------------------

ENTITY_PYX = '''cdef class ItemUpgrade:
    cpdef read(self, ByteReader reader):
        self.x = reader.read_int8()
        reader.skip(3)
        self.level = reader.read_int32()

    cpdef write(self, ByteWriter writer):
        pass

cdef class ItemData:
    cpdef read(self, ByteReader reader):
        self.type = reader.read_uint8()
        self.sub_type = reader.read_uint8()
        reader.skip(2)
        for i in range(2):
            (<ItemUpgrade>self.upgrades[i]).read(reader)
        self.upgrade_count = reader.read_uint32()

cdef class AppearanceData:
    cpdef read(self, ByteReader reader):
        self.hair_color = reader.read_uint8()
        self.scale = reader.read_vec3()

cdef class EntityData:
    def __cinit__(self):
        self.appearance = AppearanceData.__new__(AppearanceData)

    cpdef read(self, ByteReader reader):
        self.pos = reader.read_qvec3()
        self.hostile_type = reader.read_uint8()
        reader.skip(3)
        self.appearance.read(reader)
        self.name = reader.read_ascii(16)
        for i in range(2):
            (<ItemData>self.equipment[i]).read(reader)

cdef class ChunkItemData:
    pass
'''
CONSTANTS_PY = 'FRIENDLY_PLAYER_TYPE = 0\nHOSTILE_TYPE = 1\nWARRIOR_CLASS = 1\nBERSERKER = 0\nBIG = 1 << 4\n'
STRINGS_PY = '''ENTITY_NAMES = {
    0: 'Elf Male',
    1: 'Elf Female',
}
ABILITY_NAMES = {
    21: 'Teleport To City',
}
SKILL_NAMES = {
    0: 'Pet Master',
}
ITEM_NAMES = {
    (1, 0): 'Cookie',
    (3, 1): 'Longsword',
}
'''


@pytest.fixture
def cuwo_checkout(tmp_path):
    """A made-up checkout in cuwo's layout, committed to a throw-away git repository."""
    if shutil.which('git') is None:
        pytest.skip('git not found')
    d = tmp_path / 'cuwo'
    (d / 'cuwo').mkdir(parents=True)
    for name, text in (('entity.pyx', ENTITY_PYX), ('constants.py', CONSTANTS_PY), ('strings.py', STRINGS_PY)):
        (d / 'cuwo' / name).write_text(text, encoding='utf-8')
    git = ['git', '-C', str(d), '-c', 'user.name=test', '-c', 'user.email=test@example.invalid']
    subprocess.run(git + ['init', '-q'], check=True)
    subprocess.run(git + ['add', '.'], check=True)
    subprocess.run(git + ['commit', '-q', '-m', 'fixture'], check=True)
    head = subprocess.run(git + ['rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    return d, head


def test_cuwo_proposal(cuwo_checkout):
    from kmap import cuwo
    types, enums = cuwo.proposal(cuwo_checkout[0])
    by = {t['name']: t for t in types}
    assert [t['name'] for t in types] == ['cube::ItemUpgrade', 'cube::ItemData', 'cube::AppearanceData',
                                          'cube::CreatureData']
    assert [(f['offset'], f['name'], f['type']) for f in by['cube::ItemData']['fields']] == [
        (0, 'type', 'uint8_t'), (1, 'sub_type', 'uint8_t'), (4, 'upgrades', 'cube::ItemUpgrade[2]'),
        (20, 'upgrade_count', 'uint32_t')]
    cd = by['cube::CreatureData']
    assert (cd['aliases'], cd['size']) == (['EntityData'], 24 + 1 + 3 + 13 + 16 + 2 * 24)
    assert [(f['offset'], f['name'], f['type']) for f in cd['fields']] == [
        (0, 'pos', 'cube::LongVector3'), (24, 'hostile_type', 'uint8_t'), (28, 'appearance', 'cube::AppearanceData'),
        (41, 'name', 'char[16]'), (57, 'equipment', 'cube::ItemData[2]')]
    en = {e['name']: e for e in enums}
    assert en['cube::HostileType']['values'] == [('FRIENDLY_PLAYER', 0), ('HOSTILE', 1)]
    assert en['cube::AbilityId']['values'] == [('TELEPORT_TO_CITY', 21)]
    assert en['cube::ItemId']['values'] == [('COOKIE', 256), ('LONGSWORD', 3 * 256 + 1)]


def test_creature_data_imported(kmap_repo, cuwo_checkout):
    # covers: binary-survey/Creature data imported (on a made-up checkout, with --commit for its own HEAD)
    d, head = cuwo_checkout
    code, out, err = kmap_repo.run('import-cuwo', d, '--commit', head, '--write')
    assert code == 0, out + err
    data = tomllib.loads((kmap_repo.mapping / 'types' / 'cuwo-entity.toml').read_text(encoding='utf-8'))
    cd = next(t for t in data['type'] if t['name'] == 'cube::CreatureData')
    assert [f['name'] for f in cd['field']] == ['pos', 'hostile_type', 'appearance', 'name', 'equipment']
    for e in [cd, *cd['field']]:
        assert e['status'] == 'candidate' and e['evidence'] == ['cuwo:cuwo/entity.pyx']
    code, out, err = kmap_repo.check()
    assert code == 0, out + err


def test_wrong_commit(kmap_repo, cuwo_checkout):
    # covers: binary-survey/Wrong commit
    d, head = cuwo_checkout
    code, out, err = kmap_repo.run('import-cuwo', d)
    assert code != 0
    msg = out + err
    assert head in msg and '240bb61ec42abb10a73d750ce72f243e04e55509' in msg
    assert not (kmap_repo.mapping / 'types' / 'cuwo-entity.toml').exists()


# --- the scenarios on the real builds ------------------------------------------------------------------------------

@pytest.fixture(scope='module')
def real():
    from kmap.config import Context
    from kmap import db as dbmod
    try:
        ctx = Context(ROOT)
    except Exception as e:             # no usable KMAP_HOME
        pytest.skip(str(e))
    if not all((ctx.survey_dir / b / 'libraries.json').is_file() for b in (CUBE, SERVER)):
        pytest.skip('no survey in $KMAP_HOME/survey (run `kmap survey`)')

    cache = {}

    def load(bid, name):
        key = (bid, name)
        if key not in cache:
            p = ctx.survey_dir / name if bid is None else ctx.survey_dir / bid / name
            cache[key] = json.loads(p.read_text(encoding='utf-8'))
        return cache[key]
    return SimpleNamespace(ctx=ctx, db=dbmod.load(ctx), load=load)


def test_real_cube_classes(real):
    # covers: binary-survey/Cube.exe classes
    rtti = real.load(CUBE, 'rtti.json')
    # every type descriptor, each one agreeing with Ghidra's RTTI analyzer, lambda types included
    assert rtti['types'] == 193
    assert sum(1 for c in rtti['classes'] if 'lambda' in c['mangled']) == 25
    gc = next(c for c in rtti['classes'] if c['name'] == 'cube::GameController')
    vt = gc['vtables'][0]
    assert (vt['address'], vt['slots'][10], vt['slots'][11]) == (0x6FEDC8, 0x489130, 0x4AC240)
    for name, slot in (('cube::GameController::update', 10), ('cube::GameController::render', 11)):
        e = real.db.find_function(CUBE, name)
        assert (e['address'], e['status']) == (vt['slots'][slot], 'verified')


def test_real_server_classes(real):
    # covers: binary-survey/Server.exe classes
    rtti = real.load(SERVER, 'rtti.json')
    names = {c['name'] for c in rtti['classes']}
    assert {'cube::Creature', 'cube::World', 'cube::Server'} <= names
    assert rtti['types'] == 62
    assert sum(1 for c in rtti['classes'] if 'lambda' in c['mangled']) == 10


@pytest.mark.parametrize('bid,address', [(CUBE, 0x51F490), (SERVER, 0x4633C0)])
def test_real_sqlite_found_independently(real, bid, address):
    # covers: binary-survey/SQLite found independently
    from kmap import survey
    from kmap.game import load_input
    assert real.load(bid, 'libraries.json')['functions'][str(address)] == {
        'by': 'sqlite-source', 'library': 'sqlite3', 'name': 'sqlite3_close'}
    # again, from facts with every name that came from mapping/ (pushed into Ghidra) removed
    if survey.sqlite_source(real.ctx) is None:
        pytest.skip('SQLite amalgamation not in $KMAP_HOME/sqlite')
    try:
        img = survey.Image(load_input(real.ctx, real.ctx.ws.build(bid)), real.ctx.ws.build(bid))
    except Exception as e:
        pytest.skip(f'no game input: {e}')
    facts = real.load(bid, 'functions.json')
    blind = [dict(f, name=f'FUN_{f["entry"]:08x}', name_source='DEFAULT') if f['name_source'] != 'ANALYSIS' else f
             for f in facts]
    strings = {s['address']: s for s in real.load(bid, 'strings.json')['strings']}
    libs = survey.pass_libraries(real.ctx, img, blind, strings)
    assert libs['functions'][str(address)]['name'] == 'sqlite3_close'


@pytest.mark.parametrize('bid', [CUBE, SERVER])
def test_real_stl_helper(real, bid):
    # covers: binary-survey/STL helper
    libs = real.load(bid, 'libraries.json')['functions']
    users = {f for s in real.load(bid, 'strings.json')['strings'] if s['text'] == 'vector<T> too long'
             for f in s['functions']}
    assert users
    for f in users:
        assert libs[str(f)]['library'] == 'stl', hex(f)
    cov = real.load(bid, 'coverage.json')['functions']
    assert cov['game'] == cov['total'] - len(libs)


def test_real_database_file_name(real, capsys):
    # covers: binary-survey/Database file name
    from kmap.cli import main
    assert main(['--repo', ROOT, 'survey', 'query', '--string', 'data1.db']) == 0
    out = capsys.readouterr().out
    assert '0x702E84' in out
    assert 'referenced by 0x4ED0B0 ModelLoader::loadAll' in out
    assert real.db.find_function(CUBE, 'ModelLoader::loadAll')['status'] == 'verified'


def test_real_direct3d_entry(real, capsys):
    # covers: binary-survey/Direct3D entry
    from kmap.cli import main
    key = 'd3d9.dll!Direct3DCreate9'
    assert main(['--repo', ROOT, 'survey', 'query', '--import', key]) == 0
    listed = {int(l.split()[2], 16) for l in capsys.readouterr().out.splitlines() if 'called by' in l}
    facts = real.load(CUBE, 'functions.json')
    thunks = {f['entry'] for f in facts if f['thunk_target'] == key}
    every = {f['entry'] for f in facts if f['entry'] not in thunks
             and (key in f['imports'] or thunks & set(f['calls']))}
    assert listed == every == {0x4C85C0}


def test_real_known_pair(real):
    # covers: binary-survey/Known pair
    # the survey pairs the targeted builds (07-20 since 2026-10-26)
    corr = real.load(None, 'correlation.json')
    pair = next(m for m in corr['matches'] if m['cube'] == 0x449810)
    assert pair['server'] == 0x413070
    for bid, addr in (('cube-0x51EA955E', 0x449810), ('server-0x51EA94D5', 0x413070)):
        e = real.db.find_function(bid, 'cube::Database::get')
        assert (e['address'], e['status']) == (addr, 'verified')


def test_real_name_proposed_across_builds(real):
    # covers: binary-survey/Name proposed across builds
    from kmap import survey
    corr = real.load(None, 'correlation.json')
    props = {(p['build'], p['address']): p for p in survey.correlation_proposals(real.ctx, real.db, corr)}
    at = {bid: {e['address']: e for e in real.db.functions(bid)} for bid in (CUBE, SERVER)}
    for m in corr['matches']:
        e = at[CUBE].get(m['cube'])
        if m['matcher'].startswith('exact-') and e and e['status'] == 'verified' and m['server'] not in at[SERVER] \
                and e.get('kind', 'function') == 'function':
            p = props[(SERVER, m['server'])]
            assert (p['name'], p['status'], p['evidence']) == (e['name'], 'candidate', f'correlated:{CUBE}:{e["name"]}')
    for p in props.values():
        assert p['address'] not in at[p['build']] and p['status'] == 'candidate'
    assert corr['counts']['exact-bytes'] + corr['counts']['exact-instructions'] > 0


def test_real_cuwo_import_matches_the_committed_types(real):
    # covers: binary-survey/Creature data imported (on the pinned checkout)
    from kmap import cuwo
    checkout = cuwo.find_checkout(real.ctx)
    if checkout is None:
        pytest.skip('no cuwo checkout (KMAP_CUWO or $KMAP_HOME/cuwo)')
    cuwo.check_commit(real.ctx, checkout)
    types, enums = cuwo.proposal(checkout)
    committed = os.path.join(ROOT, 'mapping', 'types')
    # the committed file is the import plus what mapping work added since (task 8.2 raised creature fields)
    kept = cuwo.mapped(os.path.join(committed, 'cuwo-entity.toml'))
    assert cuwo.types_toml(types, kept) == open(os.path.join(committed, 'cuwo-entity.toml'), encoding='utf-8').read()
    assert kept and 'status = "verified"' not in cuwo.types_toml(types)       # the import itself proposes candidates
    # less the enums a hand-written types file defines (9.2: cube::AbilityId in combat.toml, named by the game)
    elsewhere = cuwo.enums_elsewhere(committed)
    assert 'cube::AbilityId' in elsewhere and 'cube::AbilityId' in {e['name'] for e in enums}
    assert cuwo.enums_toml([e for e in enums if e['name'] not in elsewhere]) == \
        open(os.path.join(committed, 'cuwo-constants.toml'), encoding='utf-8').read()
    cd = next(t for t in real.db.types if t['name'] == 'cube::CreatureData')
    assert len(cd['field']) == 60
    for f in cd['field']:           # cuwo's candidates, unless a probe has confirmed them since
        probes = [e for e in f['evidence'] if e.startswith('probe:')]
        assert 'cuwo:cuwo/entity.pyx' in f['evidence'] and (f['status'] == 'candidate' or probes), f['name']


# --- 3.1 re-run ------------------------------------------------------------------------------------------------------

def survey_hashes(ctx):
    return {p.relative_to(ctx.survey_dir).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(ctx.survey_dir.rglob('*')) if p.is_file()}


@pytest.mark.skipif(os.environ.get('KMAP_GHIDRA_TESTS') != '1', reason='set KMAP_GHIDRA_TESTS=1 to run kmap survey')
def test_rerun(real, capsys):
    # covers: binary-survey/Re-run
    from kmap.cli import main
    if not (real.ctx.project_dir / 'kyubu.gpr').is_file():
        pytest.skip('no Ghidra project in the workspace')
    assert main(['--repo', ROOT, 'survey']) == 0
    first = survey_hashes(real.ctx)
    assert main(['--repo', ROOT, 'survey']) == 0
    assert survey_hashes(real.ctx) == first

    def sorted_keys(pairs):
        assert [k for k, _ in pairs] == sorted(k for k, _ in pairs)
        return dict(pairs)
    for name in first:                 # sorted JSON (Ghidra's facts are compact, the passes' indented)
        json.loads((real.ctx.survey_dir / name).read_text(encoding='utf-8'), object_pairs_hook=sorted_keys)
