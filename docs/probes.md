# Probes: looking at the running game

`kyubu_probe` is a developer mod for mapping work. It shows game objects field by field, finds fields by how their values change, and traces calls to game functions. What it shows can be written down as a **probe record** in `mapping/probes/`, the strongest evidence a mapping entry can have (`probe:<id>`, see [mapping/conventions.md](../mapping/conventions.md)), and a record with a script can be **replayed** unattended to check it still holds.

It ships in the SDK, not in the player release: `dev-mods/kyubu_probe/` is the mod, ready to copy into `<Cube World>\mods\`; `dev-mods/source/kyubu_probe/` is its source. In this repository it is `mods-dev/kyubu_probe/` and builds with Kyubu (`build/dev-mods/kyubu_probe/`).

## Dev mode only

The probe does nothing unless dev mode is on (`Cube.exe --kyubu-dev`, or `loader.dev_mode = true` in `kyubu/config.toml`; see [dev-tools.md](dev-tools.md)). Without it, it logs once

```
INFO  [kyubu_probe] [client] inactive: dev mode is off (start the game with --kyubu-dev, or set loader.dev_mode = true in kyubu/config.toml); nothing is installed
```

and installs no hooks, commands, windows or interfaces. In dev mode it logs `active (dev mode): F8 opens the probe window, kyubu_probe:help lists the commands`.

## Using it

- **F8** opens the probe window (inspector, field finder, call tracer). **F7** takes a finder snapshot.
- Every tool is also a console command (the console is on the key left of 1): `kyubu_probe:help`, `kyubu_probe:inspect`, `kyubu_probe:find`, `kyubu_probe:trace`, `kyubu_probe:watch`, `kyubu_probe:expect`.
- A script in `mods/kyubu_probe/probe.cfg` runs by itself once the game is ready (see *Scripts*).

### Addresses

Every command that takes an address accepts:

| Form | Means |
|---|---|
| `0x1C4A2F0`, `29664000` | a runtime address |
| `@0x4ED0EA` | a mapping address (image base `0x400000`, as in `mapping/`); rebased to where the game is loaded, since the game runs with ASLR |
| `cube::GameController::render` | a mapped symbol (function or global), from the symbol maps |
| `ecx(<traced>)` | `this` (ECX) of the latest recorded call of a traced function, named as in its `trace add`, e.g. `[ecx(@0x427FF0) + 4]`. It reaches objects no global points at, such as Server.exe's `cube::Server` ([game-access-server](../mapping/probes/game-access-server.toml)) |
| `[x]` | the 32-bit pointer stored at `x` |
| `a + b`, `a - b` | sums, e.g. `[@0x5DB2C8] + 0x800` |

Addresses inside the game image are printed both ways: `0x001B858F (@0x4C858F)`.

### Inspector

```
kyubu_probe:inspect types                           the types in cubeworld/layout.h
kyubu_probe:inspect cube::FloatVector3 [@0x5DB2C8]+0x10
kyubu_probe:inspect set x 12.5                      writes a field of the inspected object
kyubu_probe:inspect set pos.x from <address> plus 0x8000   copies the value at an address (plus n) into the field
kyubu_probe:inspect follow next                     opens the object a pointer field points at; inspect back returns
```

The inspector reads the layouts that `kmap export` generates into `sdk/include/cubeworld/layout.h`, so it knows every mapped type, field, array and enum. Each row shows the field's offset, type, value and status (`verified`, `candidate`, `guess`). Bytes no field covers are shown as `_unk_<offset>` rows in hex. Arrays show their first four elements; char arrays show as text; enums show the value's name. A pointer to memory that is not mapped shows `(unreadable)`; an object at such an address shows one row, `unreadable: 0x00000010 is not mapped`. The inspector never faults the game: every read and write goes through Kyubu's guarded `mem_read` and `mem_write`.

#### Views

```
kyubu_probe:inspect player                          the local player's creature
kyubu_probe:inspect creatures                       every creature of the World, in id order
kyubu_probe:inspect player [@0x7691C8]              the same, from a controller address
kyubu_probe:inspect creatures [@0x7691C8] + 0x2DC   the same, from a World address
```

`inspect player` follows `cube::GameController.player` and shows that `cube::Creature` like `inspect cube::Creature <address>`, so `set`, `follow` and `back` work on it. `inspect creatures` walks `cube::World.creatures`, a `std::map` from creature id to `cube::Creature*`, and prints one line per creature: its id, its address, its RTTI type, and `(local player)` for the player's own. The window (F8) has a **Local player** and a **Creatures** button; the list it shows has an **inspect** button per creature.

Without an address, the views start from the global `cube::game_controller`. It resolves only through a loader that reads the symbol maps' `[[global]]` array (not the 1.0.1 release that replays install) and only in Cube.exe, so records give the address. The views remember the last address they were given and use it again when called without one, so after a script's `inspect player [@0x7691C8]` the window's buttons work too. In Server.exe the World is `[ecx(<a cube::Server method>) + 4]` (see [game-access-server](../mapping/probes/game-access-server.toml)). The walk reads the map while the game runs; if the game changes it meanwhile, the view says `the walk stopped` and why, and shows what it read.

### Field finder

For finding a field you can make change in the game (health, a timer, a position):

```
kyubu_probe:find new f32 cube::CreatureData <address>    search one object, as 4-byte floats
kyubu_probe:find new i32 0x2000 <address>                or any length, up to 16 MB
kyubu_probe:find snap                                     (or F7) take a snapshot
... get hit ...
kyubu_probe:find snap
kyubu_probe:find decreased                                keep the candidates that went down
kyubu_probe:find list 10                                  the best candidates: offset, value, previous, changes
```

Value types are `i8 u8 i16 u16 i32 u32 f32 f64`; a search looks at every aligned offset. The filters are `changed`, `unchanged`, `increased`, `decreased`, `equal <v>` and `delta <v>` (changed by exactly v since the last snapshot). Candidates are ranked by how often they changed. Pages that cannot be read are skipped, and float NaNs are never candidates.

Snapshots can also be taken for you: `find key F6` (a key), `find on <event> [client|server]` (every time a game event fires, see [events.md](events.md)) or `find hook <function>` (every call of a game function; at most one snapshot per 50 ms). `off` removes a trigger; `find clear` ends the search.

`list` names the field at each offset when the search is over a mapped type, so a hit on a mapped field says which one.

### Call tracer

```
kyubu_probe:trace add cube::GameController::render
kyubu_probe:trace args 2                  record the first two stack arguments of each call
kyubu_probe:trace detour 3 cube::World::update    or detour a __thiscall function that pops 3 stack arguments
kyubu_probe:trace start
kyubu_probe:trace stop                    removes every hook it installed
kyubu_probe:trace report
```

The report gives, for each function, the number of calls, the times of the first ones, and the last calls with `this` (ECX), the arguments, the return address (the caller) and the side and thread. It then lists where all the calls came from, busiest first: every call is tallied by return address, `this` and thread (up to 256 such sites per function), so a rare caller on another thread shows up beside a frequent one that fills the last calls ([world-lock-server](../mapping/probes/world-lock-server.toml)). The report prints the first 24 such sites; when more exist it adds one `caller <address>: N call(s) in all` line per return address alone, so a function called with a varying ECX still lists every caller ([level-terms](../mapping/probes/level-terms.toml)). Keys pressed while tracing are added as marks, so you can match calls to what you did. `trace stop` prints `tracing stopped: N hook(s) removed; hooks owned by kyubu_probe now: 0`; the `hooks kyubu_probe` console command lists the same.

A trace hooks the function's first instruction. Kyubu's hook engine refuses a site it cannot safely move, and the tracer says why, e.g. for the functions the loader itself hooks (`Game::frame`, `cube::GameController::update`, `cube::Database::get`, `cube::Database::open`, `cube::ModelLoader::loadAll`, `cube::Model::load`): `not traced: the function already starts with a jump: another hook (often the loader's own) is there`. Trace their callers or callees instead.

`trace detour <n> <function>` hooks the whole function instead, as the loader's own detours do: a pass-through handler for a `__thiscall` function that pops n stack arguments (0 to 4) and records all of them. A wrong n leaves the stack off after the first call and the game crashes, so a session that runs through with the detour in place is evidence for the prototype ([game-detours](../mapping/probes/game-detours.toml)). At most 4 functions are detoured at a time.

### Watches

`kyubu_probe:watch hp f32 [@0x5DB2C8]+0x100` samples a value every 10 ms until `watch clear`; `expect ... watch hp decreased` checks the samples.

## Scripts

`mods/kyubu_probe/probe.cfg` holds commands, one per line, without the `kyubu_probe:` prefix; `#` starts a comment. The script starts once the game is ready and runs one line after another. Three lines exist only in scripts:

| Line | Does |
|---|---|
| `wait <seconds>` | waits |
| `wait world` | waits until a world is entered |
| `wait <seconds> until <check>` | waits until a check of `expect` holds (any but `alloc`; tried four times a second), for at most the seconds: `wait 40 until value u32 == 948 <address>`. The log says `wait: held after N s` or `wait: not met after N s, going on`; it counts as no expectation, so follow it with the `expect` that matters |
| `log <text>` | writes the text to the log |

The log shows `script started: N line(s)` and at the end `frame rate: N frame(s) in S s, F fps` and `script finished: N expectation(s), H held, F failed`.

A fixed `wait` has to be long enough for the slowest run, so a script that waits for something to happen (a click landing, a value changing) is faster with `wait <seconds> until <check>`: the seconds become a limit, not the length.

### Expectations

`expect <entry> <check>` logs whether an observation held, as `expect <entry>: held (...)` or `expect <entry>: FAILED (...)`. The entry is the mapping entry the observation is about (`-` for none):

| Check | Holds when |
|---|---|
| `calls <function> <op> <n>` | the traced function was called op n times (`op`: `== != < <= > >=`) |
| `calls <function> within <n> of <function>` | both were traced and called, and their counts are at most n apart: for two functions that run equally often, such as once per pass of one loop. The hooks go in and come out one after the other while the game runs, so allow a call at each end (n = 2) |
| `caller <function> <address>` | each recent call of the traced function returned to the address |
| `from <function>[,<function>...] <address> [this <address>]` | at least one call of the traced function (or of any of the listed ones) while tracing returned to the address (with ECX = this, when given). Every call counts, not only the recent ones: for a caller among many, such as another thread's. A trace on an instruction inside a function reads the first stack word the same way: at a `call`, that is the last argument pushed, so `from @<call> <object>+<offset>` checks which object a call such as `EnterCriticalSection` got ([world-lock](../mapping/probes/world-lock.toml)) |
| `threads <function> <op> <n> [this <address>]` | so many threads called the traced function while tracing (counting only the calls with ECX = this, when given) |
| `arg <function> <n> <op> <value>` | each recent call's stack argument n (1 = the first), as an unsigned 32-bit number, compares so |
| `reg <function> <eax\|ebx\|edx\|esi\|edi> <op> <value>` | each recent call's register at a traced instruction (a mid hook; a detour records none), as an unsigned 32-bit number. The report lists the registers of such calls too ([ability-rank](../mapping/probes/rank-and-equip-gate.toml)) |
| `watch <name> changed\|unchanged\|increased\|decreased` | the watch's samples did so |
| `watch <name> reached <v>` | one of the watch's samples equals v (a value that came and went between two checks, such as a mode); the detail also gives the lowest and highest sample |
| `value <vt> <op> <n> <address>` | the value at the address compares so |
| `found <offset>` | the offset is among the finder's candidates |
| `hooks <op> <n>` | kyubu_probe owns so many hooks (`hooks == 0` after a stop) |
| `creatures <op> <n> [<world>]` | the World's creature map walks completely and holds op n creatures (the World as in `inspect creatures`) |
| `alloc <new> <delete> [<rounds>]` | the two addresses (the game's `operator new` and `delete` thunks) lead to the MSVCR110.dll pair that `game_alloc`/`game_free` call, and blocks cross over `rounds` times (default 64): `game_alloc` → the game's delete, the game's new → `game_free`, each block in MSVCR110.dll's heap and the heap valid afterwards. Nothing is called when the addresses lead elsewhere ([game-allocator](../mapping/probes/game-allocator.toml)) |

## Probe records

`python tools/kmap probe new <id>` writes `mapping/probes/<id>.toml` from a template, with this checkout's version, the Cube.exe build and today's date (`--build server-...` for the server). Fill it in and cite it from each entry it confirms as evidence `"probe:<id>"`. `kmap probe list` lists the records.

```toml
id = "menu-render-trace"            # the file name
build = "cube-0x51D2CE9C"           # the build it was recorded on
kyubu = "1.0.1"
date = "2026-09-27"
steps = ["Started Cube.exe with --kyubu-dev and kyubu_probe; ..."]
observations = ["render ran 1733 times in 10 s, always with the same this (ECX)", "..."]

[[confirms]]                        # one per entry: a function, global, type, Type.field or enum value
entry = "cube::GameController::render"
observed = "called about once per frame at the start menu, with one GameController as this"

[replay]                            # optional
host = "Cube.exe"                   # or "Server.exe"
setup = "menu"                      # menu | new-world | existing-world
seconds = 90                        # 10 to 3600; the game is closed then, or when the script finishes
mods = []                           # repository mod folders installed next to kyubu_probe (see Replay)
mods_on = ["Cube.exe"]              # optional: only these games get the mods (default: every game)
input = []                          # gamepilot steps: "wait 5", "key w", "hold w 10", "console <command>", "type <text>" (into an open console; `key Return` sends it), "click <x> <y>",
                                    # "move <x> <y>" (the cursor alone, a hover), "until <log text>", "shot <name>" (see Replay)
script = """
trace add cube::GameController::render
trace start
wait 10
trace stop
expect cube::GameController::render calls cube::GameController::render > 100
expect - hooks == 0
"""
```

`kmap check` requires: the id matches the file name, the date is YYYY-MM-DD, `steps` and `observations` are not empty, at least one `[[confirms]]` naming an existing entry, the build is known, a valid `[replay]` table (each of its `mods` a folder of the repository with a `mod.toml`, and `mods_on` naming only games the replay runs), a script with at least one `expect` line, and every `expect` entry is one the record confirms (or `-`). The record in this repository, [menu-render-trace.toml](../mapping/probes/menu-render-trace.toml), is a complete example.

## Replay

```
python tools/kmap probe replay <id> --game "<Cube World folder>" --kyubu <kyubu-release folder> [--probe <kyubu_probe folder>] [--out <folder>] [--keep]
```

The replay copies the game (without `Save/`) into `$KMAP_HOME/tmp/replay-<id>/`, installs the Kyubu release there with its own installer and this checkout's symbol maps, checks that the copy is the record's build, installs `kyubu_probe` (default: `build/dev-mods/kyubu_probe`) with the record's script as `probe.cfg`, and starts the host with `--kyubu-dev --kyubu-headless`. When the script finishes (or after `seconds`), it closes the game and writes `report.md` with each confirmed entry as **held**, **FAILED**, **not reached** or **no expectation**, and the game's log next to it as `latest.log`. The report goes to `$KMAP_HOME/replays/<id>-<time>/` or `--out`; the copy is deleted unless `--keep`.

A record can run other mods beside the probe, such as the mod that needs the entries it confirms: `mods` lists repository folders (`mods = ["mods-examples/hello_lua"]`, written with `/`), and the replay copies each one as it is into the sandbox's `mods/<folder name>` next to `kyubu_probe`, on Windows and under Wine alike (`wine_sandbox.sh`'s `MODS`). Every game in the replay gets them: with `client = "join"` or `server = "local"`, the second host's sandbox has the same mods, only without `kyubu_probe`. `mods_on` limits them to the games it lists: with `server = "local"` and `mods_on = ["Cube.exe"]`, the probed Cube.exe has them and the Server.exe it joins runs without them, as for a mod on a joined client whose server lacks it ([entity-spawner-join](../mapping/probes/entity-spawner-join.toml)). The sandbox also gets this checkout's `loader/lua/*.lua` in `kyubu/lua/` (the 1.0.1 release ships no `require("cubeworld")`), like its symbol maps. A mod is copied from its sources, so a Lua mod runs as it is. A native mod's sources hold no DLL: for each DLL its `mod.toml` names under `[native]`, the replay takes the built one from the `--kyubu` folder, out of `<kyubu>/<root>/<id>/` (a CMake build folder: `mods/`, `gameplay-mods/`; a release: `example-mods/`, `gameplay-mods/`), found by file name whatever the build's layout, and refuses to run when it is not built (build the mod's target first). `kmap check` and the replay itself fail when a listed folder has no `mod.toml`. The report's run line names the mods.

A replay only reports. It never changes `mapping/` or the game folder, and a failed replay changes no status: a person looks at the entries. It exits 1 when an expectation failed or was not reached.

On Windows a replay is native: the host starts with `--kyubu-dev --kyubu-headless` (see [dev-tools.md](dev-tools.md), "Headless mode"), in a window no monitor shows, and gamepilot's native backend gives input by files in the sandbox's `kyubu/inbox/` (`.input`, `.shot`), so `new-world`, `existing-world` and `input` work like on Linux. Nothing you do in other windows reaches the game, and the game never takes the foreground. `--kyubu` is a build folder (such as `toolchains/kyubu-mingw-c`) or a release, and `--game` must be a vanilla copy of the game with the record's databases. The replay waits for the loader's `phase: game_ready` before gamepilot's first click, and each sandbox gets its own empty `Save/`. A game `SetCursorPos` is a warp: it moves the virtual cursor and gives the game no mouse motion.

`python tools/kmap probe replay-all [<id>...|--all] -j N` runs many records, `N` at a time, and prints one table (verdict, time, median and lowest fps). Records that need the same port (a `Server.exe` or a `client = "join"` pair) run one after another. Each replay runs in a Windows Job Object, so a crashed or stopped run leaves no `Cube.exe` or `Server.exe`; crash reports are copied next to the report without `--keep`.

**Wine is deprecated.** The Linux/WSL path (`scripts/wine_sandbox.sh`, `scripts/run_replay.sh`, `scripts/gamepilot.py` with the `xdo` backend) still works and stays the reference for parity, but a new record is checked natively first. Under Wine (a WSL distribution on the Windows machine, as [creature-data](../mapping/probes/creature-data.toml) was recorded) two conditions hold: the game copy and `KMAP_HOME` must be on the Linux filesystem (Wine faults at start-up when the game runs from a `/mnt/` drive), and the prefix needs the DLL overrides and an audio device from [install.md](install.md).

Gamepilot's input runs beside the script, so three steps tie the two together. `until <text>` waits until the log holds the text, usually a script's `log` line (when it never comes, the rest of the input is skipped). `shot <name>` saves a screenshot as `<name>.png` next to the report. `wheel up|down <n>` turns the mouse wheel `n` ticks (the camera zoom). `modfile <mod> <from> <to>` copies a file of an installed mod (one of the record's `mods`) over another of its files while the game runs, for a dev-mode hot reload: the mod ships the edited text beside the file the game reads ([anim-hot-reload](../mapping/probes/anim-hot-reload.toml), with the mod `mods-dev/anim_clips_proto`). `inbox <mod> <from> <name>.txt` drops a file of an installed mod as a request in the game's `kyubu/inbox/`, for the capture mod's dev-mode file inbox ([anim-inbox](../mapping/probes/anim-inbox.toml)); it is written under a temporary name and renamed. `hold mouse1 <s>` (or `mouse2`, `mouse3`) holds a mouse button: the game reads the buttons once a frame, and under software rendering (about 4 fps) a plain `click` is often over before a frame sees it. `hold w+mouse2 <s>` holds several keys and buttons together (down in the order written, up in reverse), for an action that needs two, such as a dodge roll (W and the middle button). [creature-fields](../mapping/probes/creature-fields.toml) uses all three: after each edit, the script logs a line, and the input waits for it and takes a screenshot of the HUD. The game is closed only once the input is done too, so input steps after the script's last line still run; `seconds` is the limit for a run that never finishes, not its length.

A `Server.exe` record can have a client: with `client = "join"` (and `setup = "menu"`), once the server is ready the replay starts a Kyubu-loaded `Cube.exe` without the probe as a second host (native on Windows; on the deprecated Wine path in a second sandbox in the same prefix), and gamepilot makes a new character there and connects it to `127.0.0.1` (**Connect to server** on the world list). The input then goes to that client, while the script and the `until` steps follow the server's log. The client's log is kept next to the report as `client.log`. [creature-server](../mapping/probes/creature-server.toml) is recorded that way; [server-ability-refusal-0720](../mapping/probes/server-ability-refusal-0720.toml) was replayed natively (a join takes about 35 s: gamepilot waits for the loader's `connected to` line). On the Wine path the client's sandbox leaves the prefix's `wineserver` running (`NOKILL=1` in `wine_sandbox.sh`), so the server's run ends both.

Two clients (Windows only): with `client = "join"` and `watcher = true`, a third game, the **watcher**, a Kyubu-loaded `Cube.exe` without the probe, joins the same server as "Watcher" once the client has joined. The input still drives the client; three steps act on the watcher instead: `shot watcher <name>` (its screenshot, saved as `watcher-<name>.png`), `until watcher <text>` (waits for the text in its log) and `console watcher <command>`. Its log is kept as `watcher.log`. Use it to check what another player sees of the client's actions ([watch-berserker-0720](../mapping/probes/watch-berserker-0720.toml), the SkillsUnleashed bystanders).

Fast join (Windows only): with `client = "join"`, `fast_join = true` skips the joining clients' menus. Each joining `Cube.exe` gets a saved character (`characters.db` from `$KMAP_FAST_JOIN_SAVE`, else `$KMAP_HOME/tmp/replay-seed-testworld/game/Save/`: a game file, kept out of the repository) and `mods-dev/auto_enter` with its option `join = "127.0.0.1"`; the client and the watcher start together, join by themselves through the game's own `client_connect`, and the input starts once both logs say `connected to`. Both play the saved character (no "Watcher" name). [server-fire-mage-0720](../mapping/probes/server-fire-mage-0720.toml) takes 27 s against about 110 s through the menus. Not for records with `level_rework`: it refuses a character without its save marker, which the seed save has not (`server-protocol-modded-0720` keeps the menus). A record whose script waits a fixed time stays as slow as that wait: wait for the join with `trace add @0x4257BC` (`Net::acceptJoinSite`) and `wait 140 until calls @0x4257BC >= 1` (`server-berserker-0720`).

`log_expect` lists texts that must appear in a second game's log, by log name: `log_expect = { client = ["..."], watcher = ["..."] }` (`server` for `server = "local"`); `latest` names the probed host's own log, on any replay. Each text is a row of the report's second table, `found` or `MISSING`, and a missing one fails the replay. `kmap check` refuses a name the replay keeps no log for, and a watcher step without `watcher = true`.

The other way round, a `Cube.exe` record can join a server: with `server = "local"` (and `setup = "menu"`), once the probed `Cube.exe` is at its menu the replay starts a Kyubu-loaded `Server.exe` without the probe as a second host (on the Wine path in a second sandbox, again `NOKILL=1`), and gamepilot joins it from the `Cube.exe`. The script, the input and the `until` steps all belong to the `Cube.exe`; the server's log is kept as `server.log`. [world-lock-client](../mapping/probes/world-lock-client.toml) is recorded that way.

### Replay speed

Under Wine (deprecated) on the owner's PC most of a replay used to be fixed waits sized for software rendering (about 4 fps):

- Gamepilot's menu steps (`new-world`, `existing-world`, the client's join) return once the next screen has changed
  and held still for 1.5 s, at most after the old fixed time; the click that loads a world returns 5 s after the log
  says `world entered`, at most after 150 s. A record's input should start with `until <text>`, as nearly all do.
- `render_distance = <5..100>` in `[replay]` (or `KMAP_RENDER_DISTANCE` for every run) sets the sandbox's
  `renderDistance` (default 40), a percentage of the chunk mesh budget: fewer chunks drawn, faster frames.
- A WoW64 Wine (Wine 11 built with `--enable-archs=i386,x86_64`) runs the 32-bit game in a 64-bit process with 64-bit
  Mesa. It needs a 64-bit prefix (`WINEARCH=win64`) with the same DLL overrides as the 32-bit one. Draw with llvmpipe
  (`GALLIUM_DRIVER=llvmpipe`): under Xvfb in WSL, `GALLIUM_DRIVER=d3d12` (WSL's GPU) ran at 8.6 fps against llvmpipe's 64
  ([auto-enter-0720](../mapping/probes/auto-enter-0720.toml), 2026-10-27), so slowly that typed text arrived about one
  letter every 2 s and gamepilot's `new-world` never entered its world.

## Session report

A record whose script needs a world or input can still be checked on Windows by playing it by hand:

1. Put the record's `script` into `mods/kyubu_probe/probe.cfg` in a game copy with Kyubu and `kyubu_probe` installed.
2. Start the game with `--kyubu-dev` and do what the record's `setup` and `input` say (enter a world, walk, get hit). The script starts once the game is ready and waits where it says `wait world`.
3. When the log shows `script finished`, or after closing the game, run:

```
python tools/kmap probe report <id> --log "<game copy>\kyubu\logs\latest.log" [--out <folder>]
```

It writes the same `report.md` as a replay (held, FAILED, not reached or no expectation per confirmed entry) with a copy of the log, to `$KMAP_HOME/replays/<id>-<time>-session/` or `--out`, and exits 1 when an expectation failed or was not reached. It refuses a log from another build than the record's (the loader's `Host build ... verified` line) and a log in which no script ran; when a log holds several runs, only the last one counts. Like a replay, it only reports: it never changes `mapping/`, and raising an entry to `verified` with `probe:<id>` stays a person's decision after reading the table.

## From other mods

Native mods can use the finder through the interface `kyubu_probe:finder` (`include/kyubu_probe.h`):

```c
void* p; uint32_t version;
if (api->get_interface(KYUBU_PROBE_FINDER, KYUBU_PROBE_FINDER_VERSION, &p, &version) != KY_OK) return;
const KyubuProbeFinder* f = (const KyubuProbeFinder*)p;
int32_t s = f->create("[@0x5DB2C8]+0x100", 0x400, "f32", 4);
f->snapshot(s);  /* ... */  f->snapshot(s);
f->filter(s, "decreased", 0);
KyubuProbeHit h;
for (int i = 0; f->result(s, i, &h) == KY_OK; ++i) { /* h.offset, h.address, h.value, h.previous, h.changes */ }
f->destroy(s);
```

Full-trust Lua mods load the same through `lua/kyubu_probe.lua` (copy it into the mod's `scripts/`):

```lua
local probe = assert(load(kyubu.read_file("scripts/kyubu_probe.lua"), "@kyubu_probe.lua"))()(kyubu)
local s = assert(probe.search("[@0x5DB2C8]+0x100", 0x400, "f32"))
s:snapshot()  -- ...
s:snapshot()
s:filter("decreased")
for _, hit in ipairs(s:results(10)) do print(hit.offset, hit.value, hit.previous, hit.changes) end
s:destroy()
```

The interface exists only in dev mode; without it `get_interface` returns `KY_E_NOT_FOUND`.

## Limits

- Functions that already start with a jump (the loader's own hooks, or another mod's) cannot be traced.
- The tracer records calls, not returns: it shows the arguments and the caller, not the result.
- The inspector knows only what `mapping/` maps. `cube::Creature` maps its `id` (+8) and cuwo's creature data as `data` (+0x10); `inspect player` shows those fields, and the bytes after +0x1178 in hex.
- A native replay cannot play audio you hear or show a window; the scenarios that need a person (judging a picture) are played by hand and checked with `kmap probe report`.
