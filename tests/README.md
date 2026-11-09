# Kyubu tests

Until the `add-feature-testing` harness exists, Kyubu is checked at four levels. Everything below runs on Linux with the MinGW-w64 cross-compiler and Wine; see `docs/building.md` for the setup.

## 1. Unit and hook-engine tests (doctest)

`build/tests/kyubu_tests.exe`, built with the loader and run under Wine:

```
cd build/tests
WINEPREFIX=~/.wine32 wine kyubu_tests.exe                              # 47 test cases (one needs KYUBU_VOX_DIR)
KYUBU_STRESS_ITERATIONS=10000 wine kyubu_tests.exe -tc="*stress*"      # the 10,000-cycle hook stress test (default 500; ~1 h on real Windows)
KYUBU_VOX_DIR='Z:\path\to\pairs' wine kyubu_tests.exe -tc="*real models*"
```

`KYUBU_VOX_DIR` must hold `<name>.vox` and `<name>.cub` pairs as written by `tools/cwmod/cwmod.py extract` (`work/vanilla/vox/*.vox` and `work/vanilla/models/*.cub` in one folder, symlinks are fine). The test converts every `.vox` with the loader's C++ code and compares it byte-for-byte with the `.cub` (2,857 models).

Test names start with the capability they cover (`hook-framework: ...`, `mod-packages: ...`), and `// covers: <capability>/<scenario>` comments name the OpenSpec scenarios.

## 2. Tool tests (pytest)

```
python3 -m pytest tests/tools
```

| File | Checks | Needs |
|---|---|---|
| `test_cwmod.py` | cipher, `.cub`/`.vox` round trips, dictionary merge of `cwmod-tools` | - |
| `test_install.py` | install → upgrade → uninstall → purge restores every game file byte-for-byte; refusal of unknown executables and foreign `zlib1.dll`; both the Python and the PowerShell installer | a vanilla game folder (`KYUBU_GAME_DIR`, default `~/game`); `pwsh` for the PowerShell half |
| `test_lua_stubs.py` | example Lua mods and the template check without diagnostics in LuaLS with the stubs, and not without them; every game-API function has its `kyubu.game` binding and stub | `lua-language-server` (`KYUBU_LUALS`) for the LuaLS runs |
| `test_game_api_entries.py` | the game API's entry list against `primitives.md`, the mapping and `kyubu_api.h` | - |
| `test_game_api_checklist.py` | the adding-a-function checklist of `docs/game-api.md`, walked for a test-only function: the entries, side-rules, LuaLS and docs checks pass, and each fails when its step is skipped | `lua-language-server` for the LuaLS runs |

## 3. Static checks

```
tests/sdk/check_headers.sh     # kyubu_api.h as C99/C11 (MinGW gcc, clang in MSVC mode) and kyubu.hpp as C++20 (MinGW g++, clang);
                               # the generated cubeworld/*.h as C99/C11/C++20, also with MSVC when cl is on PATH
python3 tools/check_docs.py    # event catalogue complete, doc samples identical to the files, links valid,
                               # every game-API function and primitive in docs/game-api.md
python3 tools/kmap check --offline          # mapping/ rules, the name lock, every generated file up to date (CI's `mapping` job)
python3 tools/kmap check --game <game dir>  # the full check: also rebuilds the symbol-map signatures from the game (read-only)
python3 tools/symgen.py check <Cube.exe> symbols/cube-0x51D2CE9C.toml
# zlib parity: the proxy's exports behave like the original (build tests/proxy/zlib_parity.c with MinGW, put it next to
# the proxy as zlib1.dll and the original as zlib1_vanilla.dll, run with WINEDLLOVERRIDES="zlib1,zlib1_vanilla=n")
```

## 4. In-game checks

`scripts/wine_sandbox.sh <game dir> build <sandbox dir> <seconds> [Cube.exe|Server.exe] [args]` makes a throw-away copy of the game (databases copied, everything else linked), installs the freshly built proxy and symbol maps, copies the mods listed in `MODS`, runs the host under Xvfb until it exits or `<seconds>` have passed, and leaves `kyubu/logs/latest.log` in the sandbox. `scripts/wine_bg.sh` does the same in the background, and `scripts/gamepilot.py` drives the menus with xdotool (`fit`, `new-world`, `existing-world`, `console <commands...>`, `shot <png>`).

```
MODS="build/fixture-mods/fx_fault_frame" scripts/wine_sandbox.sh ~/game build /tmp/sb 40 Cube.exe
grep FAULTED /tmp/sb/kyubu/logs/latest.log
```

The fixtures in `tests/fixtures/mods/` (staged to `build/fixture-mods/`) and what the log must show:

| Fixture | Scenario | Expected |
|---|---|---|
| `fx_fault_frame` | native handler faults on the first frame | `FAULTED and disabled: access violation in frame.begin at ...`; the game keeps running |
| `fx_crash` | raw hook crashes the game after 60 frames | `kyubu/logs/crash-*.txt` names `fx_crash`; next start with `on_mod_crash = "disable"` logs `Mod 'fx_crash' disabled after the previous crash` |
| `fx_script_error` | Lua handler raises 25 errors per frame | `handler ... disabled after more than 20 errors within 1 second` |
| `fx_restricted` | restricted Lua calls FFI and hooks | `require('ffi')` and `kyubu.hook` both fail with "... at trust level 'restricted'"; `io`/`os` are nil |
| `fx_missing_dep` | depends on a mod that does not exist | `failed`, start-menu notice names it and the log path |
| `fx_requires_server` | `requires_server = true` | joining a vanilla `Server.exe` is blocked with a message |
| `fx_patch_a` + `fx_patch_b` | overlapping byte patches | the second patch fails with `patch ... conflicts with fx_patch_a` |
| `fx_dup_a` + `fx_dup_b` | same mod id twice | both `failed: duplicate mod id` |
| `fx_hot` | edit the script while running with `--kyubu-dev` | `unloading version 1`, `hot reload: reloaded fx_hot`, `hot version 2` |
| `fx_features` | providers, remaps, custom registry, interfaces, Lua FFI hook on `Database::get`, mid hook | `provider generated proc-tree7.cub`, `slot 899 ... (remap)`, `Database::get calls seen by the Lua hook: 26xx`, `mid hook at 0x4ED0EA` |
| `fx_parity` | every native API function through Lua; side rules; text registry | `API parity: 71/74 ... client`, `59/61 ... server` (the gap is `creature_find`, `creature_teleport` and, on the client, `creature_give_item`: no Lua check yet), `side rules: 5/5`, `text: current=Hello en=Hello de=Hallo dictionaries=ok` (`current=Hallo` with `language 1` in `options.cfg`) |
| `fx_slow` | 10 ms frame handler | diagnostics panel (Ctrl+F3, dev mode) shows about 10-12 ms for `fx_slow` |
| `fx_input` | cancels Enter; consumes clicks over its window | `cancelled Enter #1`; clicking the window over the menu's BACK button does not leave the character screen |

Other in-game checks: the example mods (`hello_native`, `hello_lua`, `gold_sword`, `hook_sample`, `spells_lib` + `frost_spells`), safe mode (`--kyubu-safe`, `kyubu/SAFE`), passthrough on a byte-modified `Cube.exe`, `Server.exe` boot, and multiplayer against a local vanilla and Kyubu `Server.exe`. The results of the last full run are recorded in `openspec/changes/add-mod-loader/tasks.md`.
