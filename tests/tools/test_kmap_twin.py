"""kmap twin (add-twin-matching): the twin's build record, its source-file parts and its survey summary.

These tests need no Ghidra and no game files; the twin's own import and survey run on the owner's PC.
"""
import hashlib
from types import SimpleNamespace

import pytest

import conftest  # noqa: F401  (puts tools/kmap on sys.path)
from kmap import twin
from kmap.cli import build_parser
from kmap.util import KmapError

ROOT = 'D:/work/ic/alpha'


def write_record(home, exe=b'exe', pdb=b'pdb', **variant):
    v = {'id': 'ic-test-o2', 'folder': 'build/o2/RelWithDebInfo',
         'cube_exe_sha256': hashlib.sha256(exe).hexdigest(), 'cube_pdb_sha256': hashlib.sha256(pdb).hexdigest()}
    v.update(variant)
    folder = home / 'twin' / v['folder']
    folder.mkdir(parents=True)
    (folder / 'Cube.exe').write_bytes(exe)
    (folder / 'Cube.pdb').write_bytes(pdb)
    lines = ['[source]', 'path = "tmp/ic"', 'folder = "alpha"', 'rebuilds = "cube-0x51EA955E"', '', '[[variant]]']
    lines += [f'{k} = "{val}"' for k, val in v.items()]
    (home / 'twin' / 'build.toml').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return v


# --- parts ------------------------------------------------------------------------------------------------

@pytest.mark.parametrize('path, part', [
    ('D:/work/ic/alpha/src/cube/Creature.cpp', 'game'),
    ('D:/work/ic/alpha/include/cube/Creature.h', 'game'),
    ('D:/work/ic/alpha/plasma/src/plasma/Widget.cpp', 'game'),
    ('/D:/work/ic/alpha/src/cube/World.cpp', 'game'),                  # Ghidra's form
    ('d:\\work\\IC\\alpha\\src\\server\\main.cpp', 'game'),            # Windows form, other case
    ('D:/work/ic/alpha/thirdparty/sqlite/sqlite3.c', 'library'),
    ('D:/work/ic/alpha/platform/FreeImageRuntime.cpp', 'library'),
    ('D:/work/twin/build/o2/_deps/zlib_source-src/inflate.c', 'library'),
    ('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.41.34120/include/vector', 'library'),
    ('D:/work/ic/alpha-other/src/cube/X.cpp', 'library'),               # a sibling folder is not the tree
    (None, 'none'),
    ('', 'none'),
])
def test_part_of(path, part):
    assert twin.part_of(path, ROOT) == part


@pytest.mark.parametrize('name, part', [
    ("cube::Item::`scalar_deleting_destructor'", 'game'),
    ('plasma::Widget::~Widget', 'game'),
    ("`cube::World::update'::`2'::`dynamic atexit destructor'", 'game'),
    ('std::_Tree<int>::_Erase', 'none'),
    ('Imf_2_2::Header::Header', 'none'),
    ('cubefoo::x', 'none'),
    (None, 'none'),
])
def test_without_a_source_file_the_namespace_decides(name, part):
    assert twin.part_of(None, ROOT, name) == part
    assert twin.part_of(f'{ROOT}/thirdparty/x.c', ROOT, name) == 'library'   # a source file always wins


# --- the build record -------------------------------------------------------------------------------------

def test_missing_record_names_the_task(tmp_path):
    with pytest.raises(KmapError, match='task 1.1'):
        twin.load_record(SimpleNamespace(home=tmp_path))


def test_variants_are_picked_by_id(tmp_path):
    write_record(tmp_path)
    rec = twin.load_record(SimpleNamespace(home=tmp_path))
    assert [v['id'] for v in twin.pick_variants(rec, None)] == ['ic-test-o2']
    with pytest.raises(KmapError, match='unknown variant'):
        twin.pick_variants(rec, ['ic-test-ltcg'])


def test_files_are_checked_against_the_record(tmp_path):
    v = write_record(tmp_path)
    ctx = SimpleNamespace(home=tmp_path)
    exe, pdb = twin.variant_files(ctx, v)
    assert exe.name == 'Cube.exe' and pdb.name == 'Cube.pdb'
    (exe.parent / 'Cube.pdb').write_bytes(b'rebuilt')
    with pytest.raises(KmapError, match='cube_pdb_sha256'):
        twin.variant_files(ctx, v)


def test_alpha_root_is_under_kmap_home(tmp_path):
    write_record(tmp_path)
    ctx = SimpleNamespace(home=tmp_path)
    assert twin.alpha_root(ctx, twin.load_record(ctx)) == tmp_path / 'tmp' / 'ic' / 'alpha'


# --- the summary ------------------------------------------------------------------------------------------

def fact(entry, source, pdb_name):
    return {'entry': entry, 'source_file': source, 'pdb_name': pdb_name}


def rtti(*names_with_vtables, plain=()):
    return {'classes': [{'name': n, 'vtables': [1]} for n in names_with_vtables] +
                       [{'name': n, 'vtables': []} for n in plain]}


def test_summary_counts_parts_names_and_shared_vtable_classes():
    facts = [fact(1, f'{ROOT}/src/cube/A.cpp', 'cube::A::f'), fact(2, f'{ROOT}/src/cube/A.cpp', None),
             fact(3, f'{ROOT}/thirdparty/x.c', 'x_init'), fact(4, None, None),
             fact(5, None, "cube::A::`scalar_deleting_destructor'")]
    s = twin.summarise(facts, ROOT, rtti('cube::A', 'cube::B', 'std::exception'),
                       rtti('cube::A', 'cube::C', 'std::exception', 'std::_Func_base', plain=('cube::B',)))
    assert s['functions'] == 5 and s['pdb_named'] == 3 and s['game_pdb_named'] == 2
    assert s['parts'] == {'game': 3, 'library': 1, 'none': 1} and s['game_by_name_only'] == 1
    assert s['rtti'] == {'twin_vtable_classes': 3, 'cube_vtable_classes': 4, 'same_name': 2,
                         'cube_game_vtable_classes': 2, 'game_same_name': 1,
                         'only_cube': ['cube::C', 'std::_Func_base'], 'only_twin': ['cube::B']}


def test_twin_image_ranges():
    t = twin.TwinImage(0x400000, 0x1000, 0x401000, 0x401800)
    assert t.in_image(0x400FFF) and not t.in_image(0x401000)
    assert t.in_text(0x401000) and not t.in_text(0x401800)


# --- the command line -------------------------------------------------------------------------------------

def test_cli_has_twin_init_and_survey():
    p = build_parser()
    a = p.parse_args(['twin', 'init', '--variant', 'ic-test-o2', '--rebuild'])
    assert (a.cmd, a.twin_cmd, a.variant, a.rebuild) == ('twin', 'init', ['ic-test-o2'], True)
    a = p.parse_args(['twin', 'survey'])
    assert (a.twin_cmd, a.variant) == ('survey', None)


# --- the matcher ------------------------------------------------------------------------------------------

def mf(entry, calls=(), size=100, pdb=None):
    """A function fact: calls are entries (in order); size from one range."""
    return {'entry': entry, 'name': pdb or f'FUN_{entry:08x}', 'pdb_name': pdb, 'ranges': [[entry, entry + size]],
            'calls': sorted(set(calls)), 'call_seq': [[c, []] for c in calls]}


def vt(*slots, offset=0):
    return {'address': 1, 'offset': offset, 'slots': list(slots)}


def side(facts, classes=(), strings=(), imports=None, eligible=None):
    return {'facts': facts, 'eligible': set(eligible if eligible is not None else (f['entry'] for f in facts)),
            'rtti': {'classes': [{'name': n, 'vtables': v} for n, v in classes]}, 'strings': list(strings),
            'imports': imports or {}}


def rows(result):
    return {(r['cube'], r['twin']): r for r in result['matches']}


def test_match_pairs_vtable_slots_with_the_same_class_name():
    cube = side([mf(10), mf(11)], classes=[('cube::A', [vt(10, 11)])])
    tw = side([mf(110, pdb='cube::A::f'), mf(111, pdb='cube::A::g')], classes=[('cube::A', [vt(110, 111)])])
    r = rows(twin.match_functions(cube, tw))
    assert r[(10, 110)]['matcher'] == 'vtable' and r[(10, 110)]['score'] == 0.95
    assert r[(11, 111)]['twin_name'] == 'cube::A::g' and not r[(11, 111)]['weak']


def test_match_drops_a_pairing_two_matchers_dispute():
    cube = side([mf(10), mf(11)], classes=[('cube::A', [vt(10)])],
                strings=[{'text': 'hello world', 'functions': [11]}])
    tw = side([mf(110), mf(111)], classes=[('cube::A', [vt(110)])],
              strings=[{'text': 'hello world', 'functions': [110]}])
    result = twin.match_functions(cube, tw)
    assert result['matches'] == []
    assert {(c['cube'], c['twin'], c['matcher']) for c in result['conflicts']} == \
        {(10, 110, 'vtable'), (11, 110, 'string')}


def test_match_keeps_a_pair_two_matchers_agree_on_with_the_stronger_score():
    cube = side([mf(10)], classes=[('cube::A', [vt(10)])], strings=[{'text': 'hello world', 'functions': [10]}])
    tw = side([mf(110)], classes=[('cube::A', [vt(110)])], strings=[{'text': 'hello world', 'functions': [110]}])
    row = rows(twin.match_functions(cube, tw))[(10, 110)]
    assert row['matcher'] == 'vtable' and row['also'] == ['string']


def test_match_propagates_by_call_sequence():
    cube = side([mf(10, calls=[20, 21, 22]), mf(20), mf(21), mf(22)], classes=[('cube::A', [vt(10)])])
    tw = side([mf(110, calls=[120, 122]), mf(120), mf(122), mf(121)], classes=[('cube::A', [vt(110)])])
    assert len(twin.match_functions(cube, tw)['matches']) == 1     # 3 callees against 2: gaps of unequal size
    cube = side([mf(10, calls=[20, 21]), mf(20), mf(21)], classes=[('cube::A', [vt(10)])])
    tw = side([mf(110, calls=[120, 121]), mf(120), mf(121)], classes=[('cube::A', [vt(110)])])
    r = rows(twin.match_functions(cube, tw))
    assert r[(20, 120)]['matcher'] == 'callgraph' and r[(20, 120)]['score'] == 0.807
    assert r[(20, 120)]['from'] == {'cube': 10, 'twin': 110}


def test_match_aligns_around_a_matched_callee():
    cube = side([mf(10, calls=[20, 21, 22]), mf(20), mf(21), mf(22)], classes=[('cube::A', [vt(10, 21)])])
    tw = side([mf(110, calls=[120, 121, 122]), mf(120), mf(121), mf(122)], classes=[('cube::A', [vt(110, 121)])])
    r = rows(twin.match_functions(cube, tw))
    assert r[(20, 120)]['matcher'] == r[(22, 122)]['matcher'] == 'callgraph'


def test_match_keeps_a_weak_row_and_does_not_propagate_from_it():
    def chain(base):
        return [mf(base + i, calls=[base + i + 1] if i < 8 else []) for i in range(9)]
    cube = side(chain(10), classes=[('cube::A', [vt(10)])])
    tw = side(chain(110), classes=[('cube::A', [vt(110)])])
    r = rows(twin.match_functions(cube, tw))
    assert not r[(13, 113)]['weak'] and r[(13, 113)]['score'] == 0.583
    assert r[(14, 114)]['weak'] and r[(14, 114)]['score'] == 0.496
    assert (15, 115) not in r


def test_match_uses_only_eligible_functions():
    cube = side([mf(10)], classes=[('cube::A', [vt(10)])], eligible=[])
    tw = side([mf(110)], classes=[('cube::A', [vt(110)])])
    assert twin.match_functions(cube, tw)['matches'] == []


def test_match_import_only_caller_and_static_third_party():
    cube = side([mf(10), mf(11)], imports={'msvcr110.dll!_atoi': [10], 'freeimage.dll!_FreeImage_LoadU@12': [11]})
    tw = side([mf(110), mf(111, calls=[500]), mf(500, pdb='FreeImage_LoadU')],
              imports={'api-ms-win-crt-convert-l1-1-0.dll!atoi': [110]}, eligible=[110, 111])
    r = rows(twin.match_functions(cube, tw))
    assert r[(10, 110)]['matcher'] == 'import' and r[(11, 111)]['matcher'] == 'import'


def test_match_constants_need_three_rare_values_held_by_one_function_per_side():
    def withconst(e, vals):
        f = mf(e)
        f['call_seq'] = [[None, list(vals)]]
        return f
    k = (0x12345678, 0x3F800001, 0xDEADBEEF)
    cube = side([withconst(10, k), withconst(11, (0x20000, 0x20001))])
    tw = side([withconst(110, k), withconst(111, (0x20000, 0x20001))])
    r = rows(twin.match_functions(cube, tw))
    assert r[(10, 110)]['matcher'] == 'constants' and r[(10, 110)]['score'] == 0.65 and (11, 111) not in r


def test_match_leaves_out_a_stub_folded_into_several_vtable_slots_and_does_not_propagate_to_it():
    # 10 sits in the slots of two classes whose twin functions differ: it names neither, and no parent pairs it later
    cube = side([mf(10), mf(11), mf(12, calls=[10]), mf(13)],
                classes=[('cube::A', [vt(10)]), ('cube::B', [vt(10, 13)]), ('cube::C', [vt(12)])])
    tw = side([mf(110), mf(111), mf(112, calls=[110]), mf(113)],
              classes=[('cube::A', [vt(110)]), ('cube::B', [vt(111, 113)]), ('cube::C', [vt(112)])])
    result = twin.match_functions(cube, tw)
    r = rows(result)
    assert not any(c == 10 for c, _ in r) and (12, 112) in r and (13, 113) in r
    assert result['conflicts'] == []


def test_match_callgraph_can_skip_a_tiny_function(monkeypatch):
    monkeypatch.setattr(twin, 'CALLGRAPH_TINY', 16)
    cube = side([mf(10, calls=[20, 21]), mf(20, size=7), mf(21)], classes=[('cube::A', [vt(10)])])
    tw = side([mf(110, calls=[120, 121]), mf(120, size=7), mf(121)], classes=[('cube::A', [vt(110)])])
    r = rows(twin.match_functions(cube, tw))
    assert (20, 120) not in r and r[(21, 121)]['matcher'] == 'callgraph'


def test_data_consts_read_the_values_a_function_points_at_in_read_only_data():
    class Img:
        sections = [('.text', 0x1000, b'\x00' * 16), ('.rdata', 0x2000, bytes.fromhex('0000803f' '35fa8e3c')), ('.data', 0x3000, b'\x01' * 8)]

        def u32(self, va):
            for _, st, d in self.sections:
                if st <= va and va + 4 <= st + len(d):
                    return int.from_bytes(d[va - st:va - st + 4], 'little')
    f = mf(10)
    f['data_refs'] = [0x2000, 0x3000]
    g = mf(11)
    g['data_refs'] = [0x2000]
    out = twin.data_consts([f, g], {10}, Img())
    assert out == {10: {0x3F800000, 0x3C8EFA35}}


def test_match_constants_also_use_the_values_in_read_only_data():
    k = {0x3C8EFA35, 0x3F800001, 0xDEADBEEF}
    cube = side([mf(10), mf(11)])
    tw = side([mf(110), mf(111)])
    cube['consts'], tw['consts'] = {10: set(k), 11: {0x20000}}, {110: set(k), 111: {0x20000}}
    r = rows(twin.match_functions(cube, tw))
    assert r[(10, 110)]['matcher'] == 'constants' and (11, 111) not in r


def test_match_callers_pairs_the_only_unmatched_shared_caller():
    cube = side([mf(20), mf(21), mf(30, calls=[20, 21])], classes=[('cube::A', [vt(20, 21)])])
    tw = side([mf(120), mf(121), mf(130, calls=[120, 121])], classes=[('cube::A', [vt(120, 121)])])
    r = rows(twin.match_functions(cube, tw))
    assert r[(30, 130)]['matcher'] == 'callers' and r[(30, 130)]['score'] == 0.55


def test_names_tsv_and_pairs_file(tmp_path):
    cube = side([mf(0x401000)])
    tw = side([mf(110, pdb='cube::Creature::tick')])
    result = twin.match_functions(cube, tw, pairs=[(0x401000, 110)])
    assert twin.names_tsv(result, {0x401000: 'creature_tick'}) == \
        '0x401000\tcube::Creature::tick\tpairs\t1.000\tcreature_tick\n'
    assert twin.names_tsv(result, {}).endswith('\t-\n')
    p = tmp_path / 'pairs.toml'
    p.write_text('[[pair]]\ncube = 0x401000\ntwin = 110\n', encoding='utf-8')
    assert twin.read_pairs(p) == [(0x401000, 110)] and twin.read_pairs(tmp_path / 'none.toml') == []


def test_cli_has_twin_match():
    a = build_parser().parse_args(['twin', 'match', '--variant', 'x'])
    assert (a.twin_cmd, a.variant) == ('match', ['x'])


# --- several variants -------------------------------------------------------------------------------------------

def vrow(c, t, name, score, matcher='callgraph', **extra):
    return dict({'cube': c, 'twin': t, 'twin_name': name, 'matcher': matcher, 'score': score,
                 'weak': score < twin.WEAK_BELOW}, **extra)


def test_name_keys_leave_out_overloads_and_ineligible_functions():
    facts = [mf(1, pdb='cube::A::f'), mf(2, pdb='cube::A::g'), mf(3, pdb='cube::A::g'), mf(4, pdb='cube::B::h'), mf(5)]
    assert twin.name_keys(facts, {1, 2, 3, 5}) == {1: 'cube::A::f'}


def test_combine_counts_the_variants_that_agree_on_a_name():
    keys = {'a': {110: 'cube::A::f'}, 'b': {210: 'cube::A::f'}}
    ev = {'a': [vrow(10, 110, 'cube::A::f', 0.7)], 'b': [vrow(10, 210, 'cube::A::f', 0.95, 'vtable')]}
    [r] = twin.combine_variants(ev, keys)['matches']
    assert (r['key'], r['matcher'], r['score'], r['variants']) == ('cube::A::f', 'vtable', 0.95, ['a', 'b'])
    assert r['twin'] == {'a': 110, 'b': 210} and 'rivals' not in r


def test_combine_takes_the_best_evidence_and_lists_the_rival():
    keys = {'a': {110: 'cube::A::f'}, 'b': {211: 'cube::A::g'}}
    ev = {'a': [vrow(10, 110, 'cube::A::f', 0.8)], 'b': [vrow(10, 211, 'cube::A::g', 0.6)]}
    [r] = twin.combine_variants(ev, keys)['matches']
    assert r['key'] == 'cube::A::f' and r['rivals'] == ['cube::A::g']


def test_combine_more_variants_break_a_score_tie():
    keys = {'a': {110: 'f'}, 'b': {210: 'f', 211: 'g'}, 'c': {311: 'g'}}
    ev = {'a': [vrow(10, 110, 'f', 0.7)], 'b': [vrow(11, 211, 'g', 0.7)], 'c': [vrow(10, 311, 'g', 0.7)]}
    # 10-f has one vote, 10-g one, 11-g one: all tie, so 10 is disputed and g goes to 11
    res = twin.combine_variants(ev, keys)
    assert {(c['cube'], c['key']) for c in res['conflicts']} == {(10, 'f'), (10, 'g')}
    assert [(r['cube'], r['key']) for r in res['matches']] == [(11, 'g')]
    ev['b'].append(vrow(10, 210, 'f', 0.7))
    res = twin.combine_variants(ev, keys)
    assert [(r['cube'], r['key'], r['variants']) for r in res['matches']] == [(10, 'f', ['a', 'b']), (11, 'g', ['b'])]


def test_combine_is_one_to_one_on_names_and_skips_seed_rows():
    keys = {'a': {110: 'f'}, 'b': {210: 'f'}}
    ev = {'a': [vrow(10, 110, 'f', 0.9)], 'b': [vrow(12, 210, 'f', 0.7), vrow(13, 210, 'f', 0.99, seed=True)]}
    [r] = twin.combine_variants(ev, keys)['matches']
    assert (r['cube'], r['rivals']) == (10, ['0xC'])


def test_combine_keeps_an_overload_within_its_variant():
    keys = {'a': {}, 'b': {}}
    ev = {'a': [vrow(10, 110, 'f', 0.7)], 'b': [vrow(10, 210, 'f', 0.7)]}
    res = twin.combine_variants(ev, keys)
    assert res['matches'] == [] and len(res['conflicts']) == 2      # two keys, same rank: a dispute


def test_matcher_seeds_are_anchors_that_propagate():
    cube = side([mf(10, calls=[20, 21]), mf(20), mf(21)])
    tw = side([mf(110, calls=[120, 121]), mf(120), mf(121)])
    r = rows(twin.match_functions(cube, tw, seeds=[(10, 110, 'string', 0.75)]))
    assert r[(10, 110)]['seed'] and r[(10, 110)]['matcher'] == 'string'
    assert r[(20, 120)]['score'] == round(0.75 * twin.CALLGRAPH_FACTOR, 3) and 'seed' not in r[(20, 120)]


def test_match_variants_carries_a_pair_from_one_variant_to_another():
    # variant a finds the parent by its vtable; variant b has no RTTI, so only a seed lets its callees pair up
    cube = side([mf(10, calls=[20, 21]), mf(20, size=40), mf(21, size=60)], classes=[('cube::A', [vt(10)])])
    a = side([mf(110, pdb='cube::A::f'), mf(120, pdb='cube::A::x', size=40)], classes=[('cube::A', [vt(110)])])
    b = side([mf(210, calls=[220, 221], pdb='cube::A::f'), mf(220, pdb='cube::A::x', size=40),
              mf(221, pdb='cube::A::y', size=60)])
    combined, first = twin.match_variants(cube, {'a': a, 'b': b})
    assert len(first['b']['matches']) == 0
    got = {(r['cube'], r['key']): r for r in combined['matches']}
    assert got[(10, 'cube::A::f')]['variants'] == ['a']
    assert got[(20, 'cube::A::x')]['variants'] == ['b'] and got[(21, 'cube::A::y')]['matcher'] == 'callgraph'
    assert combined['counts']['rounds'] == [1, 3, 3]


def test_combined_tsv_and_short_ids():
    short = twin.short_ids(['ic-x-o2', 'ic-x-ob0'])
    assert (short('ic-x-o2'), short('ic-x-ob0')) == ('o2', 'ob0')
    res = {'matches': [{'cube': 0x401000, 'twin_name': 'cube::A::f', 'matcher': 'vtable', 'score': 0.95,
                        'variants': ['ic-x-o2', 'ic-x-ob0']}]}
    assert twin.combined_tsv(res, {}, short) == '0x401000\tcube::A::f\tvtable\t0.950\t-\to2,ob0\n'


def test_cli_twin_match_combine():
    assert build_parser().parse_args(['twin', 'match', '--combine']).combine
    assert not build_parser().parse_args(['twin', 'match']).combine
