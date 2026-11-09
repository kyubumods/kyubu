"""What the release zips contain (scripts/package.py; add-mapping-workspace task 4.9).

Packages this checkout with a stand-in build folder (a fake proxy DLL and example mods), so it runs without a
compiler, then reads the zips back.
"""
import os
import sys
import zipfile
from pathlib import Path

import pytest

from conftest import ROOT

sys.path.insert(0, os.path.join(ROOT, 'scripts'))
import package  # noqa: E402

VER = package.version()


def tree(rel):
    """{path relative to rel: bytes} for every file under ROOT/rel."""
    base = Path(ROOT) / rel
    return {p.relative_to(base).as_posix(): p.read_bytes() for p in sorted(base.rglob('*')) if p.is_file()}


def read_zip(path):
    with zipfile.ZipFile(path) as z:
        top = {n.split('/', 1)[0] for n in z.namelist()}
        assert top == {Path(path).stem}, f'{path}: everything is under one folder named like the zip'
        return {n.split('/', 1)[1]: z.read(n) for n in z.namelist() if not n.endswith('/')}


@pytest.fixture(scope='module')
def zips(tmp_path_factory):
    tmp = tmp_path_factory.mktemp('package')
    build = tmp / 'build'
    (build / 'loader').mkdir(parents=True)
    (build / 'loader' / 'zlib1.dll').write_bytes(b'MZ stand-in proxy')
    for m in package.EXAMPLES:
        (build / 'mods' / m).mkdir(parents=True)
        (build / 'mods' / m / 'mod.toml').write_text(f'id = "{m}"\n')
    for m in package.BUNDLED:
        (build / 'gameplay-mods' / m).mkdir(parents=True)
        (build / 'gameplay-mods' / m / 'mod.toml').write_text(f'id = "{m}"\n')
    (build / 'dev-mods' / 'kyubu_probe' / 'bin' / 'Release').mkdir(parents=True)       # where MSVC puts it
    (build / 'dev-mods' / 'kyubu_probe' / 'bin' / 'Release' / 'kyubu_probe.dll').write_bytes(b'MZ stand-in probe')
    out = tmp / 'dist'
    out.mkdir()
    player = package.zip_dir(package.player_release(str(build), str(out), VER, strip=''))
    sdk = package.zip_dir(package.sdk(str(out), VER, str(build)))
    return read_zip(player), read_zip(sdk)


def test_sdk_has_the_game_headers_and_lua_stubs(zips):
    _, sdk = zips
    headers = tree('sdk/include/cubeworld')
    assert {'cubeworld.h', 'base.h', 'types.h', 'functions.h'} <= set(headers)
    for name, data in headers.items():
        assert sdk[f'include/cubeworld/{name}'] == data
    assert sdk['include/kyubu_api.h'] == (Path(ROOT) / 'sdk/include/kyubu_api.h').read_bytes()
    stubs = (Path(ROOT) / 'sdk/lua/cubeworld.lua').read_bytes()
    assert sdk['lua/cubeworld.lua'] == stubs
    assert sdk['templates/lua-mod/types/cubeworld.lua'] == stubs      # completion in the template out of the box
    assert sdk['lua/kyubu.lua'] == sdk['templates/lua-mod/types/kyubu.lua']


def test_sdk_has_the_ghidra_pair(zips):
    _, sdk = zips
    ghidra = {n: d for n, d in sdk.items() if n.startswith('ghidra/')}
    assert set(ghidra) == {'ghidra/apply.py', 'ghidra/mappings.json'}, 'apply.py finds mappings.json in its folder'
    for f in package.GHIDRA_FILES:
        assert ghidra[f'ghidra/{f}'] == (Path(ROOT) / 'mapping/ghidra' / f).read_bytes()
    readme = sdk['README.md'].decode()
    assert '`ghidra/`' in readme and 'apply.py' in readme and 'cubeworld' in readme


def test_player_release_carries_mod_manager_only(zips):
    # the player release carries ModManager in mods/, where the installer takes it from; the gameplay mods come
    # from the catalog
    player, _ = zips
    assert package.BUNDLED == ['mod_manager']
    for m in package.BUNDLED:
        assert (Path(ROOT) / 'mods-gameplay' / m / 'mod.toml').is_file()
        assert f'mods/{m}/mod.toml' in player
        assert f'example-mods/{m}/mod.toml' not in player
    assert not any(n.startswith('gameplay-mods/') for n in player)
    assert 'ModManager is installed with Kyubu' in player['README.txt'].decode()


def test_sdk_and_player_have_the_game_reference(zips):
    for z in zips:
        game_docs = {n[len('docs/game/'):]: d for n, d in z.items() if n.startswith('docs/game/')}
        assert game_docs == tree('docs/game')


def test_player_release_has_the_ffi_module(zips):
    player, _ = zips
    for name, data in tree('loader/lua').items():
        assert player[f'kyubu/lua/{name}'] == data
    assert 'kyubu/lua/cubeworld.lua' in player
    for name, data in tree('symbols').items():
        if name.endswith('.toml'):
            assert player[f'kyubu/symbols/{name}'] == data


def test_nothing_private_or_source_only_ships(zips):
    for z in zips:
        for n in z:
            assert not n.startswith('mapping/'), n               # the database stays in the repository
            assert not n.endswith(('.gpr', '.gzf', '.gdt')) and '.rep/' not in n and '/decomp/' not in n, n
            assert '__pycache__' not in n and not n.endswith('.pyc'), n
        assert not any(n.endswith('exported-names.lock') or n.endswith('workspace.toml') for n in z)


def test_sdk_has_the_probe_mod_and_the_player_release_does_not(zips):
    # add-mapping-workspace task 5.1: kyubu_probe is packaged in the SDK
    player, sdk = zips
    src = tree('mods-dev/kyubu_probe')
    for name, data in src.items():
        assert sdk[f'dev-mods/source/kyubu_probe/{name}'] == data
    mod = Path(ROOT) / 'mods-dev/kyubu_probe'
    assert sdk['dev-mods/kyubu_probe/mod.toml'] == (mod / 'mod.toml').read_bytes()
    assert sdk['dev-mods/kyubu_probe/bin/kyubu_probe.dll'] == b'MZ stand-in probe'    # where mod.toml says
    assert 'common = "bin/kyubu_probe.dll"' in (mod / 'mod.toml').read_text()
    assert sdk['dev-mods/kyubu_probe/lua/kyubu_probe.lua'] == (mod / 'lua/kyubu_probe.lua').read_bytes()
    assert not any(n.startswith('dev-mods/kyubu_probe/src/') for n in sdk)
    assert sdk['include/kyubu_probe.h'] == (mod / 'include/kyubu_probe.h').read_bytes()
    assert sdk['include/cubeworld/layout.h'] == (Path(ROOT) / 'sdk/include/cubeworld/layout.h').read_bytes()
    assert '`dev-mods/`' in sdk['README.md'].decode() and 'docs/probes.md' in sdk['README.md'].decode()
    assert not any('kyubu_probe' in n for n in player)


def test_sdk_without_a_built_probe_has_its_sources(tmp_path):
    s = Path(package.sdk(str(tmp_path), VER, str(tmp_path / 'no-build')))
    assert (s / 'dev-mods/source/kyubu_probe/mod.toml').is_file()
    assert not (s / 'dev-mods/kyubu_probe').exists()


def test_sdk_templates_declare_an_option(zips):
    # add-mod-config 4.1: both templates ship a config.toml, and the native template stages it next to bin/
    _, sdk = zips
    for t in ('lua-mod', 'native-mod'):
        shipped = sdk[f'templates/{t}/config.toml']
        assert shipped == (Path(ROOT) / 'sdk' / 'templates' / t / 'config.toml').read_bytes()
        assert b'[options.' in shipped
    assert b'config.toml ${STAGE}/config.toml' in sdk['templates/native-mod/CMakeLists.txt']


def test_release_guard_finds_the_test_key_in_a_dll(tmp_path):
    keys = package.test_key_problems(str(tmp_path))
    assert keys == []                                   # nothing there
    key = bytes([0xe3, 0xf0, 0x80, 0x43, 0x38, 0xa7, 0xc5, 0x40, 0x88, 0xb5, 0x50, 0xc7, 0x5f, 0xb0, 0x30, 0x3a,
                 0x07, 0x77, 0xe1, 0x94, 0xcc, 0xc4, 0x3a, 0xee, 0x20, 0xa5, 0x56, 0xeb, 0x7b, 0x53, 0x5a, 0x0f])
    (tmp_path / 'mods' / 'mod_manager' / 'bin').mkdir(parents=True)
    (tmp_path / 'mods' / 'mod_manager' / 'bin' / 'mod_manager.dll').write_bytes(b'MZ' + key + b'rest')
    (tmp_path / 'mods' / 'other.dll').write_bytes(b'MZ clean')
    (tmp_path / 'note.txt').write_bytes(key)            # only DLLs are scanned
    bad = package.test_key_problems(str(tmp_path))
    assert len(bad) == 1 and 'mod_manager.dll' in bad[0] and 'KYUBU_MM_TEST_KEY=OFF' in bad[0]


def test_release_guard_reads_the_key_from_keys_h(tmp_path):
    h = tmp_path / 'keys.h'
    h.write_text('#ifdef KYUBU_MM_TEST_KEY\n k.push_back(Key{' + ', '.join(['0x01'] * 32) + '});\n#endif\n'
                 ' k.push_back(Key{' + ', '.join(['0x02'] * 32) + '});\n')
    (tmp_path / 'a.dll').write_bytes(b'\x01' * 32)
    (tmp_path / 'b.dll').write_bytes(b'\x02' * 32)      # the real key is allowed
    assert [p.split(' ')[0] for p in package.test_key_problems(str(tmp_path), str(h))] == ['a.dll']
