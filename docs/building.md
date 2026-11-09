# Building Kyubu from source

Kyubu is a CMake project in C++20 that must be built for **32-bit x86 Windows**. The reference build, used for everything in this release, cross-compiles with MinGW-w64 on Linux and tests under Wine.

## Linux (reference): MinGW-w64 + Wine

Packages (Debian/Ubuntu names):

```
sudo apt install cmake ninja-build mingw-w64 g++-multilib python3 python3-pip \
                 wine32 xvfb xdotool imagemagick pulseaudio        # the last line is only for in-game tests
pip install pefile pillow pytest
```

Configure and build:

```
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

| Output | What |
|---|---|
| `build/loader/zlib1.dll` | the proxy (unstripped; `i686-w64-mingw32-strip` makes it ~4 MB) |
| `build/mods/<id>/` | staged example mods, ready to copy into `mods/` |
| `build/fixture-mods/<id>/` | staged test fixtures |
| `build/tests/kyubu_tests.exe` | unit and hook-engine tests |

LuaJIT is built by `cmake/luajit.cmake` as an external project (`make HOST_CC="gcc -m32" CROSS=i686-w64-mingw32- TARGET_SYS=Windows`); the host compiler needs 32-bit support (`g++-multilib`).

### Wine prefix for testing

```
WINEARCH=win32 WINEPREFIX=~/.wine32 wineboot -i
```

Then add per-application overrides so Wine loads the game's own DLLs and the proxy: for `Cube.exe` and `Server.exe`, set `zlib1`, `zlib1_vanilla`, `msvcr110`, `msvcp110` and `vccorlib110` to `native` (`winecfg` → Applications, or `HKCU\Software\Wine\AppDefaults\Cube.exe\DllOverrides`). Do not use a global `WINEDLLOVERRIDES`: it breaks Wine's own use of those DLLs. The game's XAudio2 needs an audio device; a PulseAudio null sink is enough (`scripts/wine_sandbox.sh` starts one, and Xvfb on `:99`).

## Windows with MinGW-w64

Install CMake, Ninja and an **i686** MinGW-w64 toolchain (e.g. MSYS2 `mingw-w64-i686-toolchain`), then:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Windows with Visual Studio (not verified)

The CMake files have an MSVC branch (static CRT, `/SAFESEH:NO`, LuaJIT via `msvcbuild.bat static`), and the loader avoids compiler-specific code (game `__thiscall` functions are hooked with the portable `__fastcall` form). **This release has not been built with MSVC**; expect to fix small issues. Configure with `cmake -B build -A Win32` from a Developer Command Prompt.

## Tests

See `tests/README.md` in the source repository for everything that can be run and what each check expects. In short:

```
cd build/tests && WINEPREFIX=~/.wine32 wine kyubu_tests.exe     # unit + hook tests
python3 -m pytest tests/tools                                   # installer, cwmod, Lua stubs
tests/sdk/check_headers.sh                                      # SDK headers with several compilers
python3 tools/check_docs.py                                     # docs consistency
MODS="build/mods/hello_lua" scripts/wine_sandbox.sh ~/game build /tmp/sb 40 Cube.exe --kyubu-dev
```

## Packaging

```
python3 scripts/package.py --build build --out dist
```

writes

- `dist/kyubu-<version>/` and `.zip`: the player release (stripped proxy, symbol maps, the `cubeworld` Lua module in `kyubu/lua/`, default config, installers, example mods, docs, licences);
- `dist/kyubu-sdk-<version>/` and `.zip`: headers (including the generated `cubeworld/` game headers), Lua stubs (`kyubu.lua` and `cubeworld.lua`), the Ghidra pair `ghidra/apply.py` and `mappings.json`, templates, CMake toolchain, `cwmod`, `symgen.py`, docs (with the game reference in `docs/game/`) and example sources.

`tests/tools/test_package.py` checks what the two zips contain.

For a release, first lock the exported names (see "Name stability" in `mapping/conventions.md`): run `python tools/kmap lock --release`, commit `mapping/exported-names.lock`, then package with `--release`, which refuses when a name is not locked for this version.

## Repository layout

| Path | Contents |
|---|---|
| `loader/` | the proxy: `core/` (boot, host detection, log, crash, guard), `hook/` (symbols, hook engine), `mods/`, `api/`, `events/`, `assets/`, `registry/`, `lua/`, `sides/`, `dev/` (console, UI) |
| `sdk/` | public headers, Lua stubs, templates, toolchain file |
| `symbols/` | symbol maps (`spec/` holds the inputs for `tools/symgen.py`) |
| `mods-examples/` | example mods |
| `tests/` | unit tests, hook tests, fixtures, tool tests, header checks |
| `tools/` | `cwmod` (asset pipeline), `symgen.py`, `check_docs.py` |
| `scripts/` | installers, packaging, Wine sandbox and game-driving helpers |
| `third_party/` | MinHook, LuaJIT 2.1, Dear ImGui 1.91.5, toml++, nlohmann/json, miniz, Monocypher, doctest (licences in each folder) |
