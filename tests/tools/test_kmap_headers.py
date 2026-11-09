"""kmap export, C headers in sdk/include/cubeworld/ (add-mapping-workspace task 4.3).

The compile tests use every 32-bit x86 compiler found: MinGW gcc (i686-w64-mingw32-gcc), clang (MSVC target) and
MSVC (cl on PATH, or found with vswhere on Windows). They are skipped when there is none.
"""
import functools
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from conftest import ROOT, function_entry

CUBE = 'cube-0x51D2CE9C'
LANGS = ('c99', 'c11', 'c++20')


# --- compilers --------------------------------------------------------------------------------------------

def _run(argv, env=None):
    r = subprocess.run(argv, capture_output=True, text=True, env=env)
    return r.returncode == 0, r.stdout + r.stderr


@functools.lru_cache(maxsize=None)
def _msvc_env():
    """The environment of an x86 MSVC developer prompt, or None."""
    if shutil.which('cl'):
        return dict(os.environ)
    if os.name != 'nt':
        return None
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / \
        'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    if not vswhere.is_file():
        return None
    vs = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                         'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                        capture_output=True, text=True).stdout.strip()
    bat = Path(vs) / 'VC' / 'Auxiliary' / 'Build' / 'vcvarsall.bat'
    if not vs or not bat.is_file():
        return None
    out = subprocess.run(f'"{bat}" x86 >nul && set', shell=True, capture_output=True, text=True).stdout
    env = dict(line.split('=', 1) for line in out.splitlines() if '=' in line)
    return env if shutil.which('cl', path=env.get('Path') or env.get('PATH')) else None


def compilers():
    """{label: compile(source, lang, include_dir, deprecated_ok=False) -> (ok, output)}; lang is c99, c11 or c++20.
    Warnings are errors, except deprecation warnings with deprecated_ok (a mod that still uses old names)."""
    found = {}
    if shutil.which('i686-w64-mingw32-gcc'):
        def gcc(src, lang, inc, deprecated_ok=False):
            exe = 'i686-w64-mingw32-g++' if lang == 'c++20' else 'i686-w64-mingw32-gcc'
            return _run([exe, f'-std={lang}', '-pedantic', '-Wall', '-Wextra', '-Werror', '-fsyntax-only', f'-I{inc}',
                         *(['-Wno-error=deprecated-declarations'] if deprecated_ok else []), str(src)])
        found['MinGW gcc'] = gcc
    if shutil.which('clang'):
        def clang(src, lang, inc, deprecated_ok=False):
            exe = 'clang++' if lang == 'c++20' else 'clang'
            return _run([exe, '--target=i686-pc-windows-msvc', '-ffreestanding', f'-std={lang}', '-pedantic', '-Wall',
                         '-Wextra', '-Werror', '-fsyntax-only', f'-I{inc}',
                         *(['-Wno-error=deprecated-declarations'] if deprecated_ok else []), str(src)])
        found['clang'] = clang
    env = _msvc_env()
    if env:
        cl = shutil.which('cl', path=env.get('Path') or env.get('PATH'))

        def msvc(src, lang, inc, deprecated_ok=False):
            mode = {'c99': ['-TC'], 'c11': ['-TC', '-std:c11'], 'c++20': ['-TP', '-std:c++20']}[lang]
            return _run([cl, '-nologo', '-W4', *([] if deprecated_ok else ['-WX']), '-Zs', *mode, f'-I{inc}', str(src)],
                        env)
        found['MSVC'] = msvc
    return found


needs_compiler = pytest.mark.skipif(not compilers(), reason='no 32-bit x86 C compiler found')


# --- generation -------------------------------------------------------------------------------------------

def field(offset, name, type_, status='candidate'):
    evidence = '"string:\\"Thing\\"", "manual:test"' if status == 'verified' else '"manual:test"'
    return (f'\n[[type.field]]\noffset = {offset}\nname = "{name}"\ntype = "{type_}"\nstatus = "{status}"\n'
            f'evidence = [{evidence}]\nsource = "kyubu"\n')


def type_head(name, size, status='candidate', extra=''):
    return (f'\n[[type]]\nname = "{name}"\nkind = "struct"\nsize = {size}\nstatus = "{status}"\n'
            f'evidence = ["manual:test"]\nsource = "kyubu"\n{extra}')


FIXTURE_TYPES = (
    type_head('cube::Vec', '0xC') + field('0x0', 'x', 'float') + field('0x4', 'y', 'float') + field('0x8', 'z', 'float') +
    type_head('cube::Base', '0x4') + field('0x0', 'id', 'uint32_t', 'verified') +
    type_head('cube::Thing', '0x20', extra='base = "cube::Base"\n') + field('0x8', 'hp', 'float', 'verified') +
    field('0xC', 'pos', 'cube::Vec') + field('0x18', 'label', 'std::string*') + field('0x1C', 'secret', 'uint32_t', 'guess') +
    '\n[[enum]]\nname = "cube::Mode"\nsize = 1\nstatus = "candidate"\nevidence = ["manual:test"]\nsource = "kyubu"\n'
    '\n[[enum.value]]\nname = "IDLE"\nvalue = 0\n\n[[enum.value]]\nname = "ATTACK"\nvalue = 2\n')


def generate(repo):
    from kmap import db as dbmod
    from kmap import export_headers
    from kmap.config import Context
    ctx = Context(repo.root)
    return export_headers.generate(ctx, dbmod.load(ctx))


def test_header_layout(kmap_repo):
    kmap_repo.write('mapping/types/things.toml', FIXTURE_TYPES)
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml',
                    function_entry('cube::Thing::update', '0x4A1230', 'verified', ['rtti:.?AVThing@cube@@'],
                                   'prototype = "void __thiscall cube::Thing::update(cube::Thing* this, int32_t dt)"') +
                    '\n' + function_entry('cube::Thing::~Thing', '0x4A1300', 'verified', ['rtti:.?AVThing@cube@@']) +
                    '\n' + function_entry('cube::Thing::secretGuess', '0x4A1400', 'guess') +
                    '\n' + function_entry('helper', '0x4A1500', 'candidate').replace('"thiscall"', '"cdecl"'))
    gen = generate(kmap_repo)
    assert sorted(gen) == [f'sdk/include/cubeworld/{h}.h' for h in ('base', 'cubeworld', 'functions', 'layout',
                                                                   'types')]
    assert gen == generate(kmap_repo)                                        # deterministic
    types, functions = gen['sdk/include/cubeworld/types.h'], gen['sdk/include/cubeworld/functions.h']
    struct = types[types.index('struct cube_Thing {'):]
    struct = struct[:struct.index('};')]
    lines = [l.strip() for l in struct.splitlines()[1:]]
    assert lines[0].startswith('cube_Base _base;')
    assert lines[1] == 'uint8_t _unk_4[0x4];'                               # explicit padding up to hp
    assert lines[2].startswith('float hp;') and lines[2].endswith('/* 0x8 */')
    assert lines[3].startswith('cube_Vec pos;') and 'CUBEWORLD_CANDIDATE' in lines[3]
    assert lines[4].startswith('void* label;') and '(std::string*)' in lines[4]
    assert lines[5] == 'uint8_t _unk_1c[0x4];'                              # the guess field is padding
    assert types.index('struct cube_Base {') < types.index('struct cube_Thing {')
    assert types.index('struct cube_Vec {') < types.index('struct cube_Thing {')
    assert 'CW_STATIC_ASSERT(offsetof(cube_Thing, hp) == 0x8, cube_Thing_hp, "cube::Thing.hp is at 0x8");' in types
    assert 'CW_STATIC_ASSERT(sizeof(cube_Thing) == 0x20, cube_Thing_sizeof, "cube::Thing is 0x20 bytes");' in types
    assert 'offsetof(cube_Thing, pos)' not in types                        # candidates get no offset assertion
    assert 'typedef uint8_t cube_Mode;' in types and 'cube_Mode_ATTACK = 2,' in types
    assert '#define CW_SYM_cube_Thing_update "cube::Thing::update"' in functions
    assert 'typedef void (CW_FASTCALL *cw_cube_Thing_update_fn)(cube_Thing* self, void* edx, int32_t dt);' in functions
    assert '#define CW_SYM_cube_Thing_dtor "cube::Thing::~Thing" /* Cube.exe; no prototype yet */' in functions
    assert '#define CW_SYM_helper "helper" /* Cube.exe; CUBEWORLD_CANDIDATE; no prototype yet */' in functions
    layout = gen['sdk/include/cubeworld/layout.h']
    assert ('{"hp", "float", (uint32_t)offsetof(cube_Thing, hp), (uint32_t)sizeof(((cube_Thing*)0)->hp), 1, '
            'CW_KIND_FLOAT, 1, NULL},') in layout                          # offsets come from the struct itself
    assert '(uint32_t)offsetof(cube_Thing, pos)' in layout and '"cube_Vec"},' in layout
    assert '{"label", "std::string*", ' in layout and 'CW_KIND_POINTER, 0, NULL},' in layout
    assert '{"ATTACK", 2},' in layout and '#define CW_LAYOUT_ENUM_COUNT 1' in layout
    assert '{"cube::Thing", "cube_Thing", (uint32_t)sizeof(cube_Thing), 0, 4, cw_layout_fields_cube_Thing},' in layout
    assert '{"_base", "cube::Base", (uint32_t)offsetof(cube_Thing, _base), ' in layout
    assert '{"cube::Mode", "cube_Mode", 1, 0, 2, cw_layout_values_cube_Mode},' in layout      # a candidate enum: 0
    for text in gen.values():
        assert 'secret' not in text
        for addr in ('4A1230', '4A1300', '4A1500'):                         # no addresses
            assert addr not in text.upper()


def test_thiscall_to_an_rtti_class_is_opaque(kmap_repo):
    kmap_repo.write(f'mapping/{CUBE}/classes.toml', '''
    [[class]]
    name = "cube::World"
    rtti = ".?AVWorld@cube@@"
    status = "verified"
    evidence = ["rtti:.?AVWorld@cube@@"]
    source = "rtti"
    ''')
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml', function_entry(
        'cube::World::tick', '0x4A1230', 'verified', ['rtti:.?AVWorld@cube@@'],
        'prototype = "bool __thiscall cube::World::tick(cube::World* this, std::string key, int32_t n)"') + '\n' +
        function_entry('cube::World::load', '0x4A1300', 'verified', ['rtti:.?AVWorld@cube@@'],
                       'prototype = "cube::World* __thiscall cube::World::load(cube::World* this)"'))
    gen = generate(kmap_repo)
    assert 'typedef struct cube_World cube_World;' in gen['sdk/include/cubeworld/types.h']
    functions = gen['sdk/include/cubeworld/functions.h']
    assert 'typedef cube_World* (CW_FASTCALL *cw_cube_World_load_fn)(cube_World* self, void* edx);' in functions
    assert 'takes std::string by value' in functions and 'cw_cube_World_tick_fn' not in functions


# --- compiling --------------------------------------------------------------------------------------------

def names_field(out, header):
    """The compiler output names cube::Thing.b: in its message, or (MSVC in C mode) at a line it cites."""
    if 'cube::Thing.b is at 0x6' in out or 'cw_static_assert_cube_Thing_b' in out:
        return True
    lines = header.splitlines()
    return any('cube::Thing.b is at 0x6' in lines[int(m.group(1)) - 1] for m in re.finditer(r'types\.h\((\d+)\)', out))


@needs_compiler
def test_real_headers_compile():
    # covers: mapping-exports/Header check (every compiler found here, warnings as errors)
    inc, sdk = os.path.join(ROOT, 'sdk', 'include'), os.path.join(ROOT, 'tests', 'sdk')
    for label, cc in compilers().items():
        for lang in LANGS:
            src = os.path.join(sdk, 'cubeworld_check.cpp' if lang == 'c++20' else 'cubeworld_check.c')
            ok, out = cc(src, lang, inc)
            assert ok, f'{label} {lang}:\n{out}'


def test_layout_enum_verified(kmap_repo):
    # CwLayoutEnum.verified follows the enum's status: the loader's boot check refuses a candidate enum
    kmap_repo.write('mapping/types/things.toml', FIXTURE_TYPES +
                    '\n[[enum]]\nname = "cube::Kind"\nsize = 4\nstatus = "verified"\n'
                    'evidence = ["string:\\"Thing\\"", "manual:test"]\nsource = "kyubu"\n'
                    '\n[[enum.value]]\nname = "A"\nvalue = 0\n')
    layout = generate(kmap_repo)['sdk/include/cubeworld/layout.h']
    assert '{"cube::Kind", "cube_Kind", 4, 1, 1, cw_layout_values_cube_Kind},' in layout
    assert '{"cube::Mode", "cube_Mode", 1, 0, 2, cw_layout_values_cube_Mode},' in layout
    assert '    int verified;          /* 1 verified, 0 candidate: its values are not vouched for */' in layout


@needs_compiler
def test_layout_compiles(kmap_repo, tmp_path):
    # layout.h is constant data taken from the structs: every compiler accepts it, in C and C++
    kmap_repo.write('mapping/types/things.toml', FIXTURE_TYPES)
    code, out, err = kmap_repo.run('export', '--offline')
    assert code == 0, out + err
    inc = kmap_repo.root / 'sdk' / 'include'
    src = {}
    for ext in ('c', 'cpp'):
        src[ext] = tmp_path / f'layout.{ext}'
        src[ext].write_text('#include "cubeworld/layout.h"\n'
                            'const CwLayoutType* layout_check = &cw_layout_types[CW_LAYOUT_TYPE_COUNT - 1];\n'
                            'const CwLayoutEnum* layout_enums = cw_layout_enums;\n')
    for label, cc in compilers().items():
        for lang in LANGS:
            ok, out = cc(src['cpp' if lang == 'c++20' else 'c'], lang, inc)
            assert ok, f'{label} {lang}:\n{out}'


@needs_compiler
def test_wrong_field_type_caught_at_compile_time(kmap_repo, tmp_path):
    # covers: mapping-exports/Wrong field type caught at compile time
    def write(a_type):
        kmap_repo.write('mapping/types/things.toml', type_head('cube::Thing', '0x8') +
                        field('0x4', 'a', a_type) + field('0x6', 'b', 'uint16_t', 'verified'))
    write('uint32_t')                                                   # 2 bytes more than its space (0x4-0x5)
    code, out, err = kmap_repo.check()
    assert code == 1 and 'field cube::Thing.b overlaps field cube::Thing.a' in out
    code, out, err = kmap_repo.run('export', '--force', '--offline')
    assert code == 0, out + err
    inc = kmap_repo.root / 'sdk' / 'include'
    header = (inc / 'cubeworld' / 'types.h').read_text(encoding='utf-8')
    src = {}
    for ext in ('c', 'cpp'):
        src[ext] = tmp_path / f'wrong.{ext}'
        src[ext].write_text('#include "cubeworld/types.h"\n')
    for label, cc in compilers().items():
        for lang in LANGS:
            ok, out = cc(src['cpp' if lang == 'c++20' else 'c'], lang, inc)
            assert not ok, f'{label} {lang} compiled a wrong layout'
            assert names_field(out, header), f'{label} {lang}:\n{out}'
    write('uint16_t')                                                   # the right type compiles
    code, out, err = kmap_repo.run('export', '--offline')
    assert code == 0, out + err
    for label, cc in compilers().items():
        for lang in LANGS:
            ok, out = cc(src['cpp' if lang == 'c++20' else 'c'], lang, inc)
            assert ok, f'{label} {lang}:\n{out}'
