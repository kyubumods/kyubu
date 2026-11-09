# Mod packages

A mod is a folder or a `.zip` in the game's `mods\` folder with a `mod.toml` at its root. It can contain any mix of assets, Lua scripts and native DLLs.

```
mods\
  gold_sword\                 asset-only mod
    mod.toml
    assets\models\iron-sword1.vox
    assets\text\dict_en.xml
  hello_lua\                  script mod
    mod.toml
    scripts\common.lua  client.lua  server.lua
  hello_native\               native mod
    mod.toml
    bin\hello_native.dll
    assets\models\hello-native-crate.vox
  my_mod-1.2.0.zip            any of the above, zipped
```

A zip may have `mod.toml` at its root or inside one top-level folder. Native DLLs inside zips are extracted to `kyubu\cache\<id>-<version>\` before loading.

## `mod.toml`

```toml
id = "hello_lua"                      # required: ^[a-z][a-z0-9_]{1,31}$, unique
name = "Hello Lua"                    # required: display name
version = "1.0.0"                     # required: semantic version
kyubu = "^1.0"                        # required: loader API versions this mod works with
sides = ["client", "server"]          # required: logical sides the mod has code or content for
authors = ["you"]
description = "What the mod does."

requires_server = false               # true: the mod adds content a server must know (blocks vanilla servers)
trust = "full"                        # "full" (default) or "restricted" (Lua sandbox, see lua.md)
manage_mods = false                   # true: the mod may list, enable, install and delete mods (see "Managing mods")
gate = false                          # true: a gate mod; its id and version feed the join gate (see "Join gate")
assets = "assets"                     # asset folder inside the package (default "assets")
symbols = "symbols"                   # symbol-map folder inside the package (default "symbols")
load_after = ["other_mod"]            # ordering hints without a dependency
load_before = ["another_mod"]

[dependencies]
hello_native = "^1.0"                               # required dependency
some_lib = { version = ">=0.3, <0.5", optional = true }   # used when present, otherwise ignored

[incompatible]
old_mod = "*"                         # refuses to load together with any version of old_mod

[native]                              # DLL entry points, per side
common = "bin/hello_native.dll"       # loaded once, kyubu_mod_init called for each declared side
# client = "bin/client.dll"
# server = "bin/server.dll"

[scripts]                             # Lua entry points, per side
common = "scripts/common.lua"         # runs on every declared side (twice in single-player)
client = "scripts/client.lua"         # runs after common, client side only
server = "scripts/server.lua"
```

Errors are reported with the file and key, for example `mods/bad/mod.toml: key 'version': '1.0' is not a semantic version (e.g. 1.0.0)`. A mod with manifest errors is listed as `failed` and not loaded.

### Versions and requirements

Versions follow [semantic versioning](https://semver.org) (`1.2.3`, `1.0.0-beta.1`). Requirements use Cargo's syntax:

| Requirement | Matches |
|---|---|
| `^1.2` or `1.2` | `>=1.2.0, <2.0.0` (`^0.3` means `>=0.3.0, <0.4.0`) |
| `~1.2` | `>=1.2.0, <1.3.0` |
| `>=1.0, <1.5` | comma = and |
| `=1.4.2` | exactly |
| `*` | any version |

## Resolution and load order

On start, Kyubu scans `mods\`, reads every manifest and decides what to load. The rules, in order:

1. **Disabled** mods (`[mods.<id>] enabled = false` in `kyubu\config.toml`) and mods implicated in the previous crash (see [install.md](install.md)) are skipped.
2. **Duplicate ids**: when two packages declare the same id, neither loads; both are reported.
3. **Loader API**: the `kyubu` requirement must match the loader's API version.
4. **Incompatibilities**: a mod that lists another installed mod under `[incompatible]` with a matching version fails (the other mod still loads).
5. **Dependencies**: a missing required dependency, or one whose version does not match, fails the mod. Failures **cascade** to every mod that depends on a failed mod. Optional dependencies only affect ordering.
6. **Sides**: a mod with no side active in this process (for example a client-only mod in `Server.exe`) is `skipped`, which is not an error.
7. **Order**: dependencies load before their dependents, `load_after`/`load_before` are honoured, and everything else is ordered by id. The result is the same on every launch. A cycle fails every mod in it and names it (`a → b → a`).

The log shows the result (`Load order: ...`), and the console command `mods` shows each mod's state and reason.

## Lifecycle

Kyubu starts mods in phases. Each phase is an event (`lifecycle.<phase>`), sent to each active side:

| Phase | What happens | Allowed |
|---|---|---|
| Boot | Symbol maps, hooks, resolution. No mod code runs. | - |
| PreInit | Native entry points (`kyubu_mod_init`) run in load order, then Lua scripts. | Subscribing, hooking, creating registries, asset providers, channels, commands, interfaces. |
| Register | `lifecycle.register` | Adding registry entries, remaps, models, text. |
| PostRegister | Registries are frozen and numbered. | Looking up numbers. |
| GameReady | The start menu is up (or the server listens). | Everything except registration. |
| Shutdown | `lifecycle.shutdown`, then `kyubu_mod_shutdown` and Lua `on_unload` handlers. | Saving data. |

Calling a function in the wrong phase fails with `KY_E_PHASE`.

## States

| State | Meaning |
|---|---|
| `loaded` | Running. |
| `disabled` | Turned off in `config.toml` (or after a crash). |
| `failed` | Not loaded: manifest error, dependency problem, incompatibility, duplicate, or its entry point failed. The reason is logged. |
| `faulted` | Crashed during this session; its hooks, handlers and commands were removed and the game continued. |
| `skipped` | No side of the mod is active in this process. |
| `new` | Only after `mods_rescan`: a package added to `mods\` since the start. It loads at the next start. |
| `missing` | Only after `mods_rescan`: a package that was there at the start and is gone. |

## Per-mod storage

- `kyubu\data\<id>\` (`mod_data_dir` / `kyubu.data_dir()`): writable, persists across sessions.
- `kyubu\config\<id>.toml`: the player's values of the options the mod declares in `config.toml` ([config.md](config.md)).
- Package files are read with `read_package_file` / `kyubu.read_file(path)`, which works for folders and zips.

## Managing mods

A mod can list the packages in `mods\`, change their options, queue installs, enables, disables and deletes, and
restart the game. This is the API that a mod manager uses (API 1.2). It works only for a mod that has **full trust**
and `manage_mods = true` in its `mod.toml`. Any other caller gets `KY_E_DENIED`, and the loader logs the refusal once
for each mod and function. A Lua mod at `trust = "restricted"` cannot have it.

No package changes while the game runs: a loaded DLL cannot be replaced and Lua states cannot be unloaded. So a
change to the set of mods is **queued** in `kyubu\pending.toml`, and the loader applies the queue at the next start,
before it scans `mods\`. An option change is the exception: it applies at once.

| Native member | Does |
|---|---|
| `mods_count`, `mods_get` | Describe each package found in `mods\` at boot, whatever its state: id, name, version, state, trust, the reason it did not load, the dependencies and the dependents. |
| `mods_package_hash` | SHA-256 of a package. A zip hashes its whole file. A folder hashes a listing of its files: one line `"<relative path> <size> <sha-256>\n"` for each file, in path order, with `/` in the paths. |
| `mods_option_count`, `mods_option_get` | The options that a package declares in its `config.toml`, with the value in use. This works for a package that is not loaded. |
| `mods_option_check`, `mods_option_set` | Check a value, or set it now. For a loaded mod this is the mod's own `config_set` (the mod gets `config.changed` on the next frame). For a package that is not loaded, only the options file is written. |
| `mods_rescan` | Reads `mods\` again. A package added since the start has the state `new` (or `failed`, with the error). A package that is gone has the state `missing`. Nothing is loaded or unloaded. |
| `mods_queue`, `mods_queue_get`, `mods_queue_count`, `mods_queue_clear` | The queue. A change is an install, enable, disable, delete or config op (`KyubuModOp`). The loader checks it when it is queued, and rewrites `kyubu\pending.toml` in full. A config op is checked against the declaration of a queued install of the same id when there is one, else the installed package's. At most 64 ops. |
| `game_restart` | Starts the same `Cube.exe` with the same command line and ends this process through the game's normal exit. |

`game_restart` works on the client only (`KY_E_SIDE` on `Server.exe`), and only at the start menu: before GameReady, in
a joined game or with a world loaded it returns `KY_E_PHASE` and starts nothing. The new process waits up to 10 s for
the old one to end before it opens the log. When a restart fails, the queue stays in `kyubu\pending.toml`: the player
can restart the game by hand with the same result.

`kyubu.hpp` wraps every member (`mod.mods_get(i, info)`, `mod.mods_queue(op)`, `mod.game_restart()`). Each wrapper
returns `KY_E_UNAVAILABLE` when the loader is older than 1.2. Check `has_mod_management()` first.

### What the loader does at the next start

If `kyubu\pending.toml` exists, the loader applies it in this order, whatever order the ops were queued in:

1. **Installs.** The staged zip moves from `kyubu\staging\` into `mods\` as `<id>-<version>.zip`. A package of the same
   id goes to `kyubu\backup\` first, and its `kyubu\cache\<id>-*` folders are removed.
2. **Enables and disables** (the `[mods.<id>]` tables of `kyubu\config.toml`).
3. **Deletes.** The package goes to `kyubu\backup\`. With `keep_settings = 0` the loader also removes
   `kyubu\config\<id>.toml` and `kyubu\data\<id>\`.
4. **Options.** The value is written to the options file, which the loader creates with the defaults when it does not
   exist. This is the one case where the loader writes the options file of a disabled mod.

An op that fails is logged and the others still run. The loader then deletes `pending.toml`, so a crash during the
apply cannot repeat. Safe mode keeps the file and does not apply it. When anything failed, the start menu shows
`ModManager: 1 of 5 changes failed, see the log`.

Before the first op, the loader writes `kyubu\backup\last-apply.toml`: the previous `[mods]` tables, the previous text
of each options file that the queue touches, and the packages that moved. The next start that has no `pending.toml`
removes the packages in `kyubu\backup\` and this record, so there is one window to undo a change.

`pending.toml` is plain TOML, so a player with a broken install can read it or delete it:

```toml
op = [
  { kind = "install", id = "emtweaks", version = "1.3.0", from = "kyubu/staging/emtweaks-1.3.0.zip" },
  { kind = "enable",  id = "emtweaks" },
  { kind = "disable", id = "gold_sword" },
  { kind = "delete",  id = "blaster", keep_settings = true },
  { kind = "config",  id = "emtweaks", key = "ring_alpha", value = "0.6" },
]
```

## Making a mod

- Asset-only: [assets.md](assets.md).
- Lua: [lua.md](lua.md). Start from `sdk/templates/lua-mod`.
- Native (C/C++): [native-api.md](native-api.md). Start from `sdk/templates/native-mod`.
- Content ids (new models, text, custom registries): [registries.md](registries.md).

## Join gate

A mod with `gate = true` is a gate mod. The loader takes the gate mods that loaded on this host, sorts them by id and version, and derives one 32-bit protocol value from them (the same for every load order). `Cube.exe` sends that value in place of the vanilla 3, and `Server.exe` accepts a client only when it sent the same value, so a client with other gate mods, another version of one, or none is refused by the game's own check. A vanilla client and a vanilla server refuse each other too. Without a gate mod nothing is patched. If the game's version site does not match on this build, the loader logs `the gate is NOT in force`. `level_rework` uses this gate (`gate = true`); a mod must not patch the version sites itself.
