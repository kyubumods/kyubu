"""mapping/exported-names.lock, `kmap lock` and the old names of renamed entries in the headers and Lua files
(add-mapping-workspace task 4.7).

The compile checks use every 32-bit x86 compiler found (see test_kmap_headers); the LuaJIT check needs a 32-bit
Windows LuaJIT (KYUBU_LUAJIT). Each is skipped when missing.
"""
import os
import re
import subprocess
import sys
import tomllib

import pytest

from conftest import ROOT, function_entry
from test_kmap_headers import CUBE, LANGS, compilers, field, type_head
from test_kmap_lua import MODULE, STUBS, game_abi_luajit, luajit

SERVER = 'server-0x51D2CE9B'
LOCK = 'mapping/exported-names.lock'
FIXTURE_101 = os.path.join(ROOT, 'tests', 'tools', 'fixtures', 'symbols-1.0.1')


# --- the fixture: one function, one type with fields, one enum -------------------------------------------------

def write_mapping(repo, renamed=False, drop=()):
    """Before the rename: cube::Creature::update(cube::CreatureData*), cube::CreatureData.hp, cube::Mode::ATTACK.
    renamed: they become cube::Creature::tick, cube::CreatureState.health and cube::Mode::STRIKE, keeping the old
    names as aliases. drop: entries left out ('function', 'field', 'value'), or made a guess ('guess')."""
    fn, ty, hp, attack = (('cube::Creature::tick', 'cube::CreatureState', 'health', 'STRIKE') if renamed else
                          ('cube::Creature::update', 'cube::CreatureData', 'hp', 'ATTACK'))
    old = (lambda name: f'aliases = ["{name}"]\n') if renamed else (lambda name: '')
    status, evidence = ('guess', ['manual:test']) if 'guess' in drop else ('verified', ['manual:test', 'string:"tick"'])
    repo.write(f'mapping/{CUBE}/functions/core.toml', '' if 'function' in drop else function_entry(
        fn, '0x4A1230', status, evidence,
        f'prototype = "void __thiscall {fn}(cube::CreatureData* this, int32_t dt_ms)"\n' + old('cube::Creature::update')))
    fields = field('0x0', 'level', 'uint32_t', 'verified')
    if 'field' not in drop:
        fields += field('0x8', hp, 'float', 'verified').replace('source = "kyubu"\n', 'source = "kyubu"\n' + old('hp'))
    values = '\n[[enum.value]]\nname = "IDLE"\nvalue = 0\n'
    if 'value' not in drop:
        values += f'\n[[enum.value]]\nname = "{attack}"\nvalue = 2\n{old("ATTACK")}'
    repo.write('mapping/types/creatures.toml',
               type_head(ty, '0x10', 'verified', extra=old('cube::CreatureData')).replace(
                   '["manual:test"]', '["manual:test", "string:\\"creature\\""]') + fields +
               '\n[[enum]]\nname = "cube::Mode"\nsize = 1\nstatus = "candidate"\nevidence = ["manual:test"]\n'
               'source = "kyubu"\n' + values)
    repo.write('sdk/include/kyubu_api.h', '#define KYUBU_API_VERSION_MAJOR 1\n')


def lock_file(repo):
    return tomllib.loads((repo.root / LOCK).read_text(encoding='utf-8'))


def generated(repo, write=False):
    """The headers, Lua files and symbol maps (without signatures: no game files here) of the fixture."""
    from kmap import db as dbmod
    from kmap import export, export_headers, export_lua
    from kmap.config import Context
    ctx = Context(repo.root)
    db = dbmod.load(ctx)
    out = export_headers.generate(ctx, db)
    out.update(export_lua.generate(ctx, db))
    out.update(export.symbol_maps(ctx, db, offline=True))
    for rel, text in out.items() if write else ():
        (repo.root / rel).parent.mkdir(parents=True, exist_ok=True)
        (repo.root / rel).write_text(text, encoding='utf-8')
    return out


def resolve(symbol_map, name):
    """What resolve_symbol does with a name: the address of the map entry with that name."""
    return {s['name']: s['address'] for s in tomllib.loads(symbol_map)['symbol']}.get(name)


def macro(header, name):
    """The string a CW_SYM_ macro expands to, following macro aliases."""
    m = re.search(rf'^#define {name} (\S+)', header, re.M)
    value = m.group(1)
    return value.strip('"') if value.startswith('"') else macro(header, value)


# --- the committed lock ------------------------------------------------------------------------------------------

def test_committed_lock_holds_the_1_0_1_names():
    # 1.0.1 shipped only the symbol maps; every name in them is locked as shipped in 1.0.1 and still resolves
    from kmap import db as dbmod
    from kmap import lock
    from kmap.config import Context
    ctx = Context(ROOT)
    db = dbmod.load(ctx)
    locked = lock.load(ctx, db.ws.builds)
    assert (locked.release, locked.api_major) == ('1.0.1', 1)
    for bid in (CUBE, SERVER):
        with open(os.path.join(FIXTURE_101, f'{bid}.toml'), 'rb') as f:
            shipped = {s['name'] for s in tomllib.load(f)['symbol']}
        assert locked.names[f'symbols.{bid}'] == {n: '1.0.1' for n in shipped}
    assert set(locked.names) == {f'symbols.{CUBE}', f'symbols.{SERVER}'}
    problems = []
    lock.check_lock(ctx, db, problems)
    assert problems == []
    text = (ctx.repo / LOCK).read_text(encoding='utf-8')
    assert text == lock.render(locked, db.ws.builds)            # as `kmap lock` writes it


# --- scenarios ---------------------------------------------------------------------------------------------------

def test_silent_removal_blocked(kmap_repo):
    # covers: mapping-exports/Silent removal blocked
    write_mapping(kmap_repo)
    code, out, err = kmap_repo.run('lock', '--release', '1.1.0')
    assert code == 0, out + err
    names = lock_file(kmap_repo)
    assert names['symbols'][CUBE] == {'cube::Creature::update': '1.1.0'}
    assert names['fields'] == {'cube::CreatureData.hp': '1.1.0', 'cube::CreatureData.level': '1.1.0'}
    assert kmap_repo.check()[0] == 0
    lines = (kmap_repo.root / LOCK).read_text(encoding='utf-8').splitlines()
    for drop, name, alias in (('function', 'cube::Creature::update', 'cube::Creature::update'),
                              ('field', 'cube::CreatureData.hp', 'hp'), ('value', 'cube::Mode::ATTACK', 'ATTACK')):
        write_mapping(kmap_repo, drop=(drop,))                       # deleted without an alias
        code, out, err = kmap_repo.check()
        assert code == 1, out
        fails = [line for line in out.splitlines() if line.startswith('FAIL ')]
        assert len(fails) == 1 and name in fails[0], out
        line = next(i for i, text in enumerate(lines, 1) if text.startswith(f'"{name}"'))
        assert fails[0].startswith(f'FAIL {LOCK}:{line}: locked ')
        assert 'no longer the name or an alias' in fails[0] and 'shipped in 1.1.0' in fails[0]
        assert f'keep it as an alias of the entry that replaced it (aliases = ["{alias}"])' in fails[0]
    write_mapping(kmap_repo, drop=('guess',))                        # still there, but no longer exported
    code, out, err = kmap_repo.check()
    assert code == 1 and 'cube::Creature::update' in out and 'only a guess now' in out, out
    # a release can't drop it either, until the major API version goes up
    write_mapping(kmap_repo, drop=('function',))
    code, out, err = kmap_repo.run('lock', '--release', '1.2.0')
    assert code == 1 and 'cube::Creature::update' in err and 'major API version' in err, err
    assert lock_file(kmap_repo)['release'] == '1.1.0'
    kmap_repo.write('sdk/include/kyubu_api.h', '#define KYUBU_API_VERSION_MAJOR 2\n')
    code, out, err = kmap_repo.run('lock', '--release', '2.0.0')
    assert code == 0 and '1 dropped' in out, out + err
    after = lock_file(kmap_repo)
    assert (after['release'], after['api_major']) == ('2.0.0', 2) and CUBE not in after.get('symbols', {})
    assert kmap_repo.check()[0] == 0


def test_rename_keeps_working(kmap_repo, tmp_path):
    # covers: mapping-exports/Rename keeps working
    write_mapping(kmap_repo)
    assert kmap_repo.run('lock', '--release', '1.1.0')[0] == 0
    before = generated(kmap_repo)
    write_mapping(kmap_repo, renamed=True)
    code, out, err = kmap_repo.check()
    assert code == 0, out                                            # every locked name still resolves
    code, out, err = kmap_repo.run('lock')
    assert code == 0 and '+ [symbols.cube-0x51D2CE9C] cube::Creature::tick' in out and ' gone' in out
    assert '0 locked name(s) gone' in out
    after = generated(kmap_repo)
    # old and new mods resolve their names to the same address
    symbols, functions = after[f'symbols/{CUBE}.toml'], after['sdk/include/cubeworld/functions.h']
    address = resolve(before[f'symbols/{CUBE}.toml'], 'cube::Creature::update')
    assert address == 0x4A1230
    assert resolve(symbols, 'cube::Creature::tick') == address                   # a new mod
    assert resolve(symbols, 'cube::Creature::update') == address                 # an old mod's binary
    assert resolve(symbols, macro(functions, 'CW_SYM_cube_Creature_update')) == address   # rebuilt with the new header
    entry = next(s for s in tomllib.loads(symbols)['symbol'] if s['name'] == 'cube::Creature::update')
    assert entry['notes'] == 'deprecated alias of cube::Creature::tick'
    # the header offers both names, with the old one marked deprecated
    types = after['sdk/include/cubeworld/types.h']
    assert ('#define CW_SYM_cube_Creature_update "cube::Creature::update" /* old name: use CW_SYM_cube_Creature_tick */'
            in functions)
    assert ('CW_DEPRECATED("use cw_cube_Creature_tick_fn") typedef cw_cube_Creature_tick_fn cw_cube_Creature_update_fn;'
            in functions)
    assert 'typedef void (CW_FASTCALL *cw_cube_Creature_tick_fn)(cube_CreatureState* self' in functions
    assert 'CW_ALIASES_BEGIN struct cube_CreatureState {' in types and '};\nCW_ALIASES_END\n' in types
    assert 'union { float health; CW_DEPRECATED("use health") float hp; };' in types
    assert 'CW_DEPRECATED("use cube_CreatureState") typedef cube_CreatureState cube_CreatureData;' in types
    assert 'cube_Mode_ATTACK CW_DEPRECATED_VALUE("use cube_Mode_STRIKE") = 2,' in types
    assert 'offsetof(cube_CreatureState, health) == 0x8' in types and 'offsetof(cube_CreatureState, hp)' not in types
    # the Lua module and stubs keep the old names too
    module, stubs = after[MODULE], after[STUBS]
    assert '{name = "health", offset = 0x8, type = "float", status = "verified", aliases = {"hp"}},' in module
    assert '["cube::CreatureData"] = "cube::CreatureState",' in module
    assert '["cube::Creature::update"] = "cw_cube_Creature_tick_fn",' in module
    assert 'IDLE = 0, STRIKE = 2, ATTACK = 2' in module
    assert 'typedef cube_CreatureState cube_CreatureData;' in module
    assert 'typedef cw_cube_Creature_tick_fn cw_cube_Creature_update_fn;' in module
    assert 'union { float health; float hp; };' in module and 'CW_' not in module[:module.index(']]')]
    assert '---@field hp number old name of health (deprecated): float at 0x8 (verified)' in stubs
    assert '---@alias cube_CreatureData cube_CreatureState' in stubs


OLD_MOD = '''/* a mod written against the 1.1.0 header, before the rename */
#include "cubeworld/cubeworld.h"
static void CW_FASTCALL on_update(cube_CreatureData* self, void* edx, int32_t dt_ms) { (void)self; (void)edx; (void)dt_ms; }
cw_cube_Creature_update_fn old_mod_hook = on_update;
const char* old_mod_name = CW_SYM_cube_Creature_update;
float old_mod_hp(cube_CreatureData* c) { return c->hp; }
int old_mod_mode = cube_Mode_ATTACK;
'''
NEW_MOD = '''/* a mod written after the rename */
#include "cubeworld/cubeworld.h"
static void CW_FASTCALL on_tick(cube_CreatureState* self, void* edx, int32_t dt_ms) { (void)self; (void)edx; (void)dt_ms; }
cw_cube_Creature_tick_fn new_mod_hook = on_tick;
const char* new_mod_name = CW_SYM_cube_Creature_tick;
float new_mod_hp(cube_CreatureState* c) { return c->health; }
int new_mod_mode = cube_Mode_STRIKE;
'''


@pytest.mark.skipif(not compilers(), reason='no 32-bit x86 C compiler found')
def test_rename_keeps_working_when_compiled(kmap_repo, tmp_path):
    # covers: mapping-exports/Rename keeps working (both mods compile; the old names warn where the compiler can)
    write_mapping(kmap_repo, renamed=True)
    generated(kmap_repo, write=True)
    inc = kmap_repo.root / 'sdk' / 'include'
    src = {}
    for name, text in (('old', OLD_MOD), ('new', NEW_MOD)):
        for ext in ('c', 'cpp'):
            src[name, ext] = tmp_path / f'{name}_mod.{ext}'
            src[name, ext].write_text(text)
    for label, cc in compilers().items():
        for lang in LANGS:
            ext = 'cpp' if lang == 'c++20' else 'c'
            ok, out = cc(src['new', ext], lang, inc)
            assert ok, f'{label} {lang}, new names:\n{out}'
            ok, out = cc(src['old', ext], lang, inc, deprecated_ok=True)
            assert ok, f'{label} {lang}, old names:\n{out}'
            warned = re.findall(r"'([\w:<> ]+)'(?: is deprecated|: (?:use|was declared deprecated))", out)
            olds = {w.rpartition('::')[2] for w in warned}
            # every compiler marks the old typedefs; gcc, clang and MSVC's C++ also mark old fields and values
            assert {'cube_CreatureData', 'cw_cube_Creature_update_fn'} <= olds, f'{label} {lang}:\n{out}'
            if label != 'MSVC' or lang == 'c++20':
                assert {'hp', 'cube_Mode_ATTACK'} <= olds, f'{label} {lang}:\n{out}'


@pytest.mark.skipif(not game_abi_luajit(), reason='no 32-bit Windows LuaJIT (KYUBU_LUAJIT)')
def test_rename_keeps_working_in_luajit(kmap_repo):
    # covers: mapping-exports/Rename keeps working (a Lua mod that uses the old names)
    write_mapping(kmap_repo, renamed=True)
    generated(kmap_repo, write=True)
    out = luajit('package.path = "loader/lua/?.lua;" .. package.path\n'
                 'local cubeworld, ffi = require("cubeworld"), require("ffi")\n'
                 'local c = cubeworld.cast("cube::CreatureData", ffi.new("uint8_t[16]"))\n'
                 'c.health = 5\n'
                 'print(ffi.offsetof("cube_CreatureData", "hp"), ffi.offsetof("cube_CreatureState", "health"), c.hp,\n'
                 '      tostring(ffi.typeof(cubeworld.functions["cube::Creature::update"])) ==\n'
                 '      tostring(ffi.typeof("cw_cube_Creature_tick_fn")), #cubeworld.check())', kmap_repo.root)
    assert out.split() == ['8', '8', '5', 'true', '0']


# --- kmap lock -----------------------------------------------------------------------------------------------------

def test_lock_on_release(kmap_repo):
    write_mapping(kmap_repo)
    code, out, err = kmap_repo.run('lock')                                # before the first release: a listing
    assert code == 0 and '+ [symbols.cube-0x51D2CE9C] cube::Creature::update' in out and '(no lock yet)' in out
    assert '+ [types] cube::CreatureData' in out and '+ [values] cube::Mode::ATTACK' in out
    assert not (kmap_repo.root / LOCK).exists()
    kmap_repo.write('CMakeLists.txt', 'project(kyubu VERSION 1.1.0 LANGUAGES C CXX)\n')
    code, out, err = kmap_repo.run('lock', '--release')                     # the project version
    assert code == 0 and 'for 1.1.0: 7 name(s) added, 0 dropped' in out, out + err
    first = (kmap_repo.root / LOCK).read_text(encoding='utf-8')
    assert first.startswith('# Exported names that shipped in a release')
    assert 'release = "1.1.0"\napi_major = 1\n' in first
    assert '[symbols.cube-0x51D2CE9C]\n"cube::Creature::update" = "1.1.0"\n' in first
    assert lock_file(kmap_repo)['types'] == {'cube::CreatureData': '1.1.0', 'cube::Mode': '1.1.0'}
    assert lock_file(kmap_repo)['values'] == {'cube::Mode::ATTACK': '1.1.0', 'cube::Mode::IDLE': '1.1.0'}
    code, out, err = kmap_repo.run('lock', '--release', '1.1.0')            # again: nothing new
    assert code == 0 and 'unchanged' in out
    code, out, err = kmap_repo.run('lock', '--release', '1.0.9')
    assert code == 1 and 'newer than 1.0.9' in err
    # the next release: the rename adds the new names (fields also under the new type name), and the old ones keep
    # their first release
    write_mapping(kmap_repo, renamed=True)
    code, out, err = kmap_repo.run('lock', '--release', '1.2.0')
    assert code == 0 and '6 name(s) added' in out, out + err
    names = lock_file(kmap_repo)
    assert names['symbols'][CUBE] == {'cube::Creature::update': '1.1.0', 'cube::Creature::tick': '1.2.0'}
    assert names['types'] == {'cube::CreatureData': '1.1.0', 'cube::CreatureState': '1.2.0', 'cube::Mode': '1.1.0'}
    assert names['fields'] == {'cube::CreatureData.hp': '1.1.0', 'cube::CreatureData.level': '1.1.0',
                               'cube::CreatureState.health': '1.2.0', 'cube::CreatureState.hp': '1.2.0',
                               'cube::CreatureState.level': '1.2.0'}
    assert names['values'] == {'cube::Mode::ATTACK': '1.1.0', 'cube::Mode::IDLE': '1.1.0', 'cube::Mode::STRIKE': '1.2.0'}
    assert kmap_repo.check()[0] == 0


def test_release_problems(kmap_repo):
    from kmap import lock
    from kmap.config import Context
    write_mapping(kmap_repo)
    ctx = Context(kmap_repo.root)
    assert lock.release_problems(ctx, '1.1.0')[0] == f'{LOCK} was last updated for no release, not 1.1.0'
    kmap_repo.run('lock', '--release', '1.1.0')
    assert lock.release_problems(ctx, '1.1.0') == []
    write_mapping(kmap_repo, renamed=True)
    problems = lock.release_problems(ctx, '1.2.0')
    assert problems == [f'{LOCK} was last updated for 1.1.0, not 1.2.0',
                        '6 exported name(s) are not locked yet (`kmap lock` lists them)',
                        f'run `kmap lock --release 1.2.0` and commit {LOCK}']


def test_package_release_refuses_unlocked_names(tmp_path):
    # scripts/package.py --release refuses before building anything: this branch exports names 1.0.1 didn't ship
    out = tmp_path / 'dist'
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'package.py'), '--release',
                        '--build', str(tmp_path / 'no-build'), '--out', str(out)], capture_output=True, text=True,
                       env={**os.environ, 'KMAP_HOME': str(tmp_path / 'home')})
    assert r.returncode == 1, r.stdout + r.stderr
    assert 'exported name(s) are not locked yet' in r.stderr and 'run `kmap lock --release' in r.stderr
    assert not out.exists()


def test_bad_lock_is_reported(kmap_repo):
    write_mapping(kmap_repo)
    for text, message in (('release = "1.1.0"\napi_major = 1\n[symbols.cube-0xDEADBEEF]\nx = "1.1.0"\n',
                           'cube-0xDEADBEEF is not a build'),
                          ('release = "1.1.0"\napi_major = 1\n[types]\n"cube::Mode" = "soon"\n', 'must map each name'),
                          ('release = "next"\napi_major = 1\n', '`release` must be'),
                          ('api_major = 1\n[other]\n', '`release` must be')):
        kmap_repo.write(LOCK, text)
        code, out, err = kmap_repo.check()
        assert code == 1 and f'FAIL {LOCK}: ' in out and message in out, out


def test_field_names_are_unique_in_their_type(kmap_repo):
    write_mapping(kmap_repo)
    path = kmap_repo.root / 'mapping' / 'types' / 'creatures.toml'
    path.write_text(path.read_text(encoding='utf-8').replace('name = "level"', 'name = "level"\naliases = ["hp"]'),
                    encoding='utf-8')
    code, out, err = kmap_repo.check()
    assert code == 1 and "field cube::CreatureData.hp: name 'hp' is already used by field cube::CreatureData.level" in out
