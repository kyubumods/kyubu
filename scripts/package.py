#!/usr/bin/env python3
"""package - builds the player release and the SDK from a finished build.

    python3 scripts/package.py [--build build] [--out dist] [--strip i686-w64-mingw32-strip] [--release]

Writes dist/kyubu-<version>/ + .zip (players) and dist/kyubu-sdk-<version>/ + .zip (mod authors).
With --release (packaging a release, not a CI build), it first checks that mapping/exported-names.lock locks every
exported name for this version: run `kmap lock --release` and commit the lock before packaging a release.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUNDLED = ['mod_manager']           # mods-gameplay/: the mods the release carries; the installer puts them in mods/
EXAMPLES = ['gold_sword', 'hello_lua', 'hello_native', 'hook_sample', 'spells_lib', 'frost_spells', 'entity_spawner']
THIRD_PARTY_LICENSES = {
    'minhook': 'minhook/LICENSE.txt', 'luajit': 'luajit/COPYRIGHT', 'imgui': 'imgui/LICENSE.txt',
    'tomlplusplus': 'toml/LICENSE', 'nlohmann-json': 'json/LICENSE.MIT', 'miniz': 'miniz/LICENSE',
    'monocypher': 'monocypher/LICENCE.md',
}
KEYS_H = os.path.join(ROOT, 'mods-gameplay', 'mod_manager', 'src', 'keys.h')   # holds the test public key's bytes
GHIDRA_FILES = ('apply.py', 'mappings.json')       # mapping/ghidra/, shipped together in the SDK's ghidra/


def version():
    text = open(os.path.join(ROOT, 'CMakeLists.txt'), encoding='utf-8').read()
    return re.search(r'project\(kyubu VERSION ([0-9.]+)', text).group(1)


def lock_problems(ver):
    """Why mapping/exported-names.lock isn't ready for releasing `ver` (kmap lock), or []."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'kmap'))
    from kmap import lock
    from kmap.config import Context
    return lock.release_problems(Context(ROOT), ver)


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src, dst, ignore=None):
    shutil.copytree(src, dst, dirs_exist_ok=True, ignore=ignore)


def zip_dir(folder):
    out = folder + '.zip'
    if os.path.exists(out):
        os.remove(out)
    base = os.path.basename(folder)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(folder):
            dirs.sort()
            for f in sorted(files):
                p = os.path.join(root, f)
                z.write(p, os.path.join(base, os.path.relpath(p, folder)))
    return out


def sha256(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def test_key_problems(folder, keys_h=KEYS_H):
    """DLLs under folder that contain ModManager's test public key (add-mod-manager D16 release guard)."""
    text = open(keys_h, encoding='utf-8').read()
    keys = []
    for block in re.findall(r'#ifdef\s+KYUBU_MM_TEST_KEY(.*?)#endif', text, flags=re.S):
        for body in re.findall(r'Key\s*\{([^}]*)\}', block):
            nums = re.findall(r'0[xX]([0-9a-fA-F]{1,2})', body)
            if len(nums) == 32:
                keys.append(bytes(int(n, 16) for n in nums))
    bad = []
    for r, _, fs in os.walk(folder):
        for f in fs:
            if f.lower().endswith('.dll'):
                p = os.path.join(r, f)
                data = open(p, 'rb').read()
                if any(k in data for k in keys):
                    bad.append(os.path.relpath(p, folder))
    return [f'{b} contains the ModManager test public key: rebuild with -DKYUBU_MM_TEST_KEY=OFF' for b in bad]


def player_release(build, out, ver, strip):
    rel = os.path.join(out, f'kyubu-{ver}')
    shutil.rmtree(rel, ignore_errors=True)
    proxy = os.path.join(build, 'loader', 'zlib1.dll')
    if not os.path.isfile(proxy):
        sys.exit(f'{proxy} not found: build first')
    os.makedirs(rel)
    if strip and shutil.which(strip):
        subprocess.run([strip, '-o', os.path.join(rel, 'zlib1.dll'), proxy], check=True)
    else:
        copy(proxy, os.path.join(rel, 'zlib1.dll'))
    for f in os.listdir(os.path.join(ROOT, 'symbols')):
        if f.endswith('.toml'):
            copy(os.path.join(ROOT, 'symbols', f), os.path.join(rel, 'kyubu', 'symbols', f))
    for f in os.listdir(os.path.join(ROOT, 'loader', 'lua')):     # require("cubeworld") in full-trust mods
        if f.endswith('.lua'):
            copy(os.path.join(ROOT, 'loader', 'lua', f), os.path.join(rel, 'kyubu', 'lua', f))
    copy(os.path.join(ROOT, 'dist-files', 'config.default.toml'), os.path.join(rel, 'kyubu', 'config.default.toml'))
    for f in ('kyubu_install.py', 'kyubu_install.ps1', 'install.cmd', 'uninstall.cmd'):
        copy(os.path.join(ROOT, 'scripts', 'install', f), os.path.join(rel, 'install', f))
    for m in EXAMPLES:
        src = os.path.join(build, 'mods', m)
        if not os.path.isdir(src):
            sys.exit(f'{src} not found: build the example mods first')
        copy_tree(src, os.path.join(rel, 'example-mods', m))
    for m in BUNDLED:
        src = os.path.join(build, 'gameplay-mods', m)
        if not os.path.isdir(src):
            sys.exit(f'{src} not found: build {m} first')
        copy_tree(src, os.path.join(rel, 'mods', m))
    copy_tree(os.path.join(ROOT, 'docs'), os.path.join(rel, 'docs'))
    copy(os.path.join(ROOT, 'LICENSE'), os.path.join(rel, 'LICENSE.txt'))
    for name, p in THIRD_PARTY_LICENSES.items():
        copy(os.path.join(ROOT, 'third_party', p), os.path.join(rel, 'licenses', name + '.txt'))
    with open(os.path.join(rel, 'README.txt'), 'w', newline='\r\n') as f:
        f.write(f'''Kyubu {ver} - mod loader for Cube World Alpha v0.1.1

Install:    run install\\install.cmd (asks for your Cube World folder)
            or: python install\\kyubu_install.py install "<Cube World folder>"
Uninstall:  run install\\uninstall.cmd
Mods:       ModManager is installed with Kyubu: choose Mods on the title menu to browse the catalog
            and install mods. Mods from elsewhere go in <Cube World>\\mods\\ ; examples are in example-mods\\
Safe mode:  hold Shift while the game starts
Docs:       docs\\install.md (players), docs\\README.md (mod authors)
Licence:    MIT, in LICENSE.txt; third-party licences are in licenses\\

zlib1.dll sha256: {sha256(os.path.join(rel, 'zlib1.dll'))}
''')
    return rel


def dev_mod(build, s, name, strip):
    """A developer mod (mods-dev/): its sources always; the installable mod folder too when the build has it."""
    src = os.path.join(ROOT, 'mods-dev', name)
    copy_tree(src, os.path.join(s, 'dev-mods', 'source', name))
    if os.path.isfile(os.path.join(src, 'include', f'{name}.h')):
        copy(os.path.join(src, 'include', f'{name}.h'), os.path.join(s, 'include', f'{name}.h'))
    built = os.path.join(build, 'dev-mods', name) if build else ''
    dlls = [os.path.join(r, f) for r, _, fs in os.walk(os.path.join(built, 'bin')) for f in fs if f == f'{name}.dll']
    if not dlls:
        print(f'package: {name} is not in {built or "the build"}; the SDK has its sources only', file=sys.stderr)
        return
    mod = os.path.join(s, 'dev-mods', name)
    for f in ('mod.toml', 'probe.cfg'):
        if os.path.isfile(os.path.join(src, f)):
            copy(os.path.join(src, f), os.path.join(mod, f))
    copy_tree(os.path.join(src, 'lua'), os.path.join(mod, 'lua'))
    dll = os.path.join(mod, 'bin', f'{name}.dll')              # MSVC builds put it in bin/<config>/
    os.makedirs(os.path.dirname(dll), exist_ok=True)
    if strip and shutil.which(strip):
        subprocess.run([strip, '-o', dll, dlls[0]], check=True)
    else:
        copy(dlls[0], dll)


def sdk(out, ver, build=None, strip=''):
    s = os.path.join(out, f'kyubu-sdk-{ver}')
    shutil.rmtree(s, ignore_errors=True)
    copy_tree(os.path.join(ROOT, 'sdk', 'include'), os.path.join(s, 'include'))
    copy_tree(os.path.join(ROOT, 'sdk', 'lua'), os.path.join(s, 'lua'))
    copy_tree(os.path.join(ROOT, 'sdk', 'cmake'), os.path.join(s, 'cmake'))
    copy_tree(os.path.join(ROOT, 'sdk', 'templates'), os.path.join(s, 'templates'))
    # the Lua template carries its own copy of the stubs (the kyubu API, and the game types of require("cubeworld"))
    for f in ('kyubu.lua', 'cubeworld.lua'):
        copy(os.path.join(ROOT, 'sdk', 'lua', f), os.path.join(s, 'templates', 'lua-mod', 'types', f))
    # a named Ghidra project of the modder's own game copy; apply.py reads mappings.json from its own folder
    for f in GHIDRA_FILES:
        copy(os.path.join(ROOT, 'mapping', 'ghidra', f), os.path.join(s, 'ghidra', f))
    no_cache = shutil.ignore_patterns('__pycache__', '*.pyc')
    copy_tree(os.path.join(ROOT, 'tools', 'cwmod'), os.path.join(s, 'tools', 'cwmod'), ignore=no_cache)
    copy(os.path.join(ROOT, 'tools', 'symgen.py'), os.path.join(s, 'tools', 'symgen.py'))
    copy_tree(os.path.join(ROOT, 'mods-examples'), os.path.join(s, 'examples'),
              ignore=shutil.ignore_patterns('CMakeLists.txt'))
    copy_tree(os.path.join(ROOT, 'docs'), os.path.join(s, 'docs'))
    dev_mod(build, s, 'kyubu_probe', strip)
    copy(os.path.join(ROOT, 'LICENSE'), os.path.join(s, 'LICENSE'))
    with open(os.path.join(s, 'README.md'), 'w') as f:
        f.write(f'''# Kyubu SDK {ver}

| Folder | Contents |
|---|---|
| `include/` | `kyubu_api.h` (C ABI 1.0) and `kyubu.hpp` (C++20 helpers); `cubeworld/`: the game's types, function-pointer types and symbol names (`#include <cubeworld/cubeworld.h>`), and `layout.h`, the mapped field layouts as tables |
| `lua/` | LuaLS type stubs for the `kyubu` Lua API (`kyubu.lua`) and for the game types of `require("cubeworld")` (`cubeworld.lua`) |
| `ghidra/` | `apply.py` and `mappings.json`: Kyubu's names, prototypes and types for your own Ghidra project of the game (usage at the top of `apply.py`) |
| `templates/native-mod/` | CMake template for a native mod (MSVC or MinGW, 32-bit) |
| `templates/lua-mod/` | Lua mod template with editor setup |
| `cmake/` | MinGW-w64 i686 toolchain file for cross-compiling on Linux |
| `tools/` | `cwmod` (extract and convert game assets) and `symgen.py` (symbol maps) |
| `examples/` | sources of the example mods |
| `dev-mods/` | `kyubu_probe/`: the probe mod for mapping work (inspector, field finder, call tracer; active only in dev mode, see `docs/probes.md`), ready to copy into the game's `mods/`; `source/`: its sources. `include/kyubu_probe.h` is its finder interface for other native mods |
| `docs/` | documentation; start with `docs/README.md`; the game reference is `docs/game/README.md` |

Build the native template: `cmake -S templates/native-mod -B build -A Win32` (Visual Studio) or
`cmake -S templates/native-mod -B build -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/mingw-i686.cmake` (Linux cross; the toolchain path must be absolute), then
`cmake --build build` and copy `build/my_mod/` into the game's `mods/` folder.
''')
    return s


def test_kit(build, out, ver, strip):
    """Test binaries and staged fixture mods for checking a build on Windows (tests/WINDOWS-TEST-PLAN.md)."""
    k = os.path.join(out, f'kyubu-test-kit-{ver}')
    shutil.rmtree(k, ignore_errors=True)
    os.makedirs(os.path.join(k, 'bin'))
    tests_exe = os.path.join(build, 'tests', 'kyubu_tests.exe')
    if not os.path.isfile(tests_exe):
        sys.exit(f'{tests_exe} not found: build the tests first')
    if strip and shutil.which(strip):
        subprocess.run([strip, '-o', os.path.join(k, 'bin', 'kyubu_tests.exe'), tests_exe], check=True)
    else:
        copy(tests_exe, os.path.join(k, 'bin', 'kyubu_tests.exe'))
    cc = 'i686-w64-mingw32-gcc'
    if shutil.which(cc):
        subprocess.run([cc, '-O2', '-s', '-o', os.path.join(k, 'bin', 'zlib_parity.exe'),
                        os.path.join(ROOT, 'tests', 'proxy', 'zlib_parity.c')], check=True)
    copy_tree(os.path.join(build, 'fixture-mods'), os.path.join(k, 'fixture-mods'))
    copy(os.path.join(ROOT, 'tests', 'README.md'), os.path.join(k, 'TESTS-README.md'))
    with open(os.path.join(k, 'README.md'), 'w') as f:
        f.write(f"""# Kyubu test kit {ver}

Test binaries and fixture mods for checking Kyubu on Windows (see tests/WINDOWS-TEST-PLAN.md in the source).

- bin/kyubu_tests.exe   unit and hook-engine tests (doctest). Prints each case as it starts; KYUBU_TEST_TRACE=0 turns that off.
- bin/zlib_parity.exe   compares the proxy's zlib exports with the original zlib1.dll
- fixture-mods/         small mods that each exercise one behaviour (expected results in TESTS-README.md)
""")
    return k


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', default=os.path.join(ROOT, 'build'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'dist'))
    ap.add_argument('--strip', default='i686-w64-mingw32-strip')
    ap.add_argument('--release', action='store_true', help='a release: every exported name must be locked for it')
    a = ap.parse_args()
    ver = version()
    if a.release:
        problems = lock_problems(ver)
        for p in problems:
            print(f'package: {p}', file=sys.stderr)
        if problems:
            sys.exit(1)
    os.makedirs(a.out, exist_ok=True)
    rel = player_release(a.build, a.out, ver, a.strip)
    s = sdk(a.out, ver, a.build, a.strip)
    k = test_kit(a.build, a.out, ver, a.strip)
    problems = [p for folder in (rel, s, k) for p in test_key_problems(folder)]
    for p in problems:
        print(f'package: {p}', file=sys.stderr)
    if problems:
        sys.exit(1)
    for folder in (rel, s, k):
        z = zip_dir(folder)
        print(f'{z}  {os.path.getsize(z) / 1e6:.1f} MB  sha256 {sha256(z)}')


if __name__ == '__main__':
    main()
