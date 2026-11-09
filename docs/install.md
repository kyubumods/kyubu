# Installing Kyubu

Kyubu is a mod loader for **Cube World Alpha v0.1.1** (the 2013-07-20 build: `Cube.exe` and `Server.exe`). It works by replacing one DLL the game already loads, `zlib1.dll`, with a proxy that forwards every zlib call to the original and loads mods. No other game file is changed.

## Requirements

- Cube World Alpha v0.1.1. The installer checks the SHA-256 of `Cube.exe` / `Server.exe`:
  - `Cube.exe`: `84a7a132a84d4282338e7ea45a1940d32066d64e39cbafed8f2418d5a6dc30bf`
  - `Server.exe`: `1ffbcbce251e1506a63c9ce18ab0009d5553fcbb9e6202be2bc3a7d870d34fb1`
  - The earlier 2013-07-02 build is not supported.
- Windows 7 or later (the game is 32-bit), or Linux with Wine 9+ (see the note at the end).
- Nothing else: the proxy is statically linked.

## Install

1. Extract the release zip anywhere, for example into your Cube World folder as `Cube World\kyubu-1.0.2\`.
2. Run **`install\install.cmd`**. It asks for the Cube World folder when it cannot find `Cube.exe` two levels up. You can also pass the folder: `install.cmd "C:\Games\Cube World"`.
   - Without the `.cmd` wrapper: `powershell -ExecutionPolicy Bypass -File install\kyubu_install.ps1 -Action install -GameDir "C:\Games\Cube World"`
   - With Python 3: `python install\kyubu_install.py install "C:\Games\Cube World"`
3. Start `Cube.exe` as usual. On the start menu a notice shows how many mods were loaded.

The installer needs an internet connection for one step: it downloads **ModManager** from the mod catalog. Offline,
Kyubu is installed anyway and the installer says ModManager was left out; run it again when you are online to add it.
`--no-mod-manager` / `-NoModManager` skips the download.

The installer:

| Step | Detail |
|---|---|
| Checks the game | Refuses unknown `Cube.exe`/`Server.exe` builds (`--force` / `-Force` installs anyway; Kyubu then stays inactive on that build). Refuses when `zlib1.dll` is neither the original nor a Kyubu proxy. |
| Renames `zlib1.dll` → `zlib1_vanilla.dll` | A copy of the original is also kept in `kyubu\backup\zlib1.dll`. |
| Copies the proxy as `zlib1.dll` | |
| Adds `kyubu\` | Symbol maps (`kyubu\symbols\`), `config.default.toml`, and `config.toml` if you do not have one yet. |
| Adds `mods\` | |
| Downloads ModManager | From the catalog at <https://kyubumods.github.io/catalog/>: the newest version that fits this loader. The catalog's `index.json` must carry the catalog key's signature and the zip must match the catalog's SHA-256, or nothing is installed. It goes to `mods\mod_manager\`; an upgrade replaces the folder (your ModManager settings are in `kyubu\config\mod_manager.toml` and stay). |
| Verifies and records | Re-checks all hashes and writes `kyubu\install.json`. |

Running the installer again upgrades the proxy, the symbol maps and ModManager in place; your `config.toml`, logs and mod
data are kept.

Start the game with **`Cube.exe`**, not `CubeLauncher.exe` (the old auto-updater).

## Folder layout after installing

```
Cube World\
  Cube.exe, Server.exe, data1-4.db ...   unchanged
  zlib1.dll                              Kyubu proxy
  zlib1_vanilla.dll                      the original zlib1.dll
  mods\                                  one folder or .zip per mod
  kyubu\
    config.toml                          your settings (see below)
    config\<mod id>.toml                 each mod's options, written on first start (see config.md)
    symbols\                             symbol maps for the supported builds
    logs\latest.log                      this session's log (the 5 previous ones are log-1.log .. log-5.log)
    logs\crash-*.txt / *.dmp             crash reports
    data\<mod id>\                       per-mod storage
    pending.toml                         changes queued by a mod manager, applied at the next start (see mods.md)
    staging\                             downloaded packages that wait for that start
    backup\                              packages that the last apply replaced or deleted, and last-apply.toml
    SAFE                                 create this file to force safe mode
```

## Adding and removing mods

- ModManager, which the installer adds: choose **Mods** on the title menu to install mods from the catalog, update,
  configure and switch them. It updates itself from the catalog too.
- Copy a mod folder (or its `.zip`) into `mods\`. A mod folder contains a `mod.toml`.
- To disable a mod without deleting it, add to `kyubu\config.toml`:

  ```toml
  [mods.gold_sword]
  enabled = false
  ```
- The console command `mods` (press `` ` ``) lists every mod with its version, sides and state (loaded, disabled, failed, faulted, skipped) and why a mod did not load.
- Mods that failed to load are named in a notice on the start menu, with the path of the log.
- A mod with `manage_mods = true` (a mod manager) can do the same from inside the game. It does not change `mods\`
  while the game runs. It writes the changes to `kyubu\pending.toml` and restarts the game, and Kyubu applies them
  before it loads any mod. A package that a change replaces or deletes goes to `kyubu\backup\` until the next start
  that has no pending changes. Safe mode keeps `kyubu\pending.toml` and does not apply it; delete the file to drop the
  changes. See [mods.md](mods.md#managing-mods).

## Safe mode

Safe mode starts the game with Kyubu running but **no mods loaded**. Use it when a mod breaks the game. Any one of these turns it on:

- hold **Shift** while the game starts;
- start with `Cube.exe --kyubu-safe`;
- create an empty file `kyubu\SAFE` (delete it to leave safe mode).

The log says `SAFE MODE (<reason>)` and the start menu shows a notice.

## Crash recovery

When the game crashes, Kyubu writes `kyubu\logs\crash-<time>.txt` (exception, registers, stack, the mod it implicates and the last 200 log lines) and a minidump next to it. On the next start it reads the report and, if a mod was implicated, acts on `crash.on_mod_crash`:

| Value | Effect |
|---|---|
| `"ask"` (default) | A dialog asks whether to disable that mod. |
| `"disable"` | The mod is disabled in `config.toml` without asking. |
| `"ignore"` | Only the report path is logged. |

A mod whose callback faults while the game keeps running is marked **faulted** and disabled for the rest of the session; the game continues.

## Configuration: `kyubu\config.toml`

```toml
[loader]
dev_mode = false        # developer mode (also: Cube.exe --kyubu-dev), see dev-tools.md
log_level = "info"      # "debug", "info", "warn" or "error"
console_key = "grave"   # "grave" (` ~), "f1".."f12", "insert" or "home"
asset_trace = false     # log every asset read and which layer served it

[abilities]
placeholder = true     # false: slot 4 stays empty unless a mod claims it (see abilities.md)
skill_tree = true      # false: a mod ability is at rank 1 from the start, not learned in the skill tree (see abilities.md)

[crash]
on_mod_crash = "ask"    # "ask", "disable" or "ignore"

[mods.some_mod]
enabled = false         # mods not listed are enabled
```

`[abilities] placeholder` is for the fourth ability slot ([abilities.md](abilities.md)). With `true` (the default), key 4 of every class that no mod claims runs a stand-in ability that shows "not made yet". With `false`, that slot stays empty, as in vanilla, unless a mod claims it.

Errors in `config.toml` are logged with the line and column; the defaults are used for anything that cannot be read.

## Command-line switches

| Switch | Effect |
|---|---|
| `--kyubu-safe` | Safe mode for this session. |
| `--kyubu-dev` | Developer mode for this session. |
| `--kyubu-test[=<suite>]` | Test mode, used by the feature-testing harness. |

## Dedicated server

Install into the folder that holds `Server.exe` the same way. The proxy loads into `Server.exe`, identifies it, and loads the server side of mods. In this release the server symbol map has only a few verified symbols, so most server events are unavailable; the server runs as vanilla and vanilla clients can join it. See [sides-and-networking.md](sides-and-networking.md).

## Uninstall

Run **`install\uninstall.cmd`** (or `kyubu_install.ps1 -Action uninstall -GameDir ...`, or `python kyubu_install.py uninstall <game dir>`). It deletes the proxy and renames `zlib1_vanilla.dll` back to `zlib1.dll`, then verifies that `zlib1.dll` is the original file. `kyubu\` and `mods\` stay (they do nothing without the proxy); add `--purge` / `-Purge` to delete `kyubu\` too.

To uninstall by hand: delete `zlib1.dll`, then rename `zlib1_vanilla.dll` to `zlib1.dll`.

Uninstalling restores vanilla exactly: the executables, `zlib1.dll` and `data1.db`-`data4.db` have the same hashes as before installing (the installer tests check this).

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| "The code execution cannot proceed because zlib1_vanilla.dll was not found" (or a Kyubu message box saying the original zlib library is missing) | The original zlib is not next to the proxy. Re-run the installer, or copy `kyubu\backup\zlib1.dll` to `zlib1_vanilla.dll`. The game exits before running any game code. |
| Game starts, no mods, log says `passthrough` | The game build is not one Kyubu knows (hash mismatch). Kyubu stays out of the way on unknown builds. |
| A mod is listed as `failed` | Read the reason in the `mods` console command or the log: missing dependency, version mismatch, incompatibility, manifest error. |
| Anti-virus quarantines `zlib1.dll` | Proxy DLLs are sometimes flagged. Restore it and add an exclusion for the Cube World folder, or build Kyubu from source. |
| Crash on start after adding a mod | Hold Shift while starting (safe mode), then disable the mod in `config.toml`. |

## Running under Wine (Linux)

Kyubu is developed and tested under Wine 9 (32-bit prefix). Wine must load the native DLLs, so set these overrides for `Cube.exe` and `Server.exe` (per application, in `winecfg` → Applications, or in the registry under `HKCU\Software\Wine\AppDefaults\Cube.exe\DllOverrides`): `zlib1`, `zlib1_vanilla`, `msvcr110`, `msvcp110`, `vccorlib110` = `native`. Without an audio device the game's XAudio2 initialisation fails; a PulseAudio null sink is enough.
