# Lua mods

Kyubu embeds **LuaJIT 2.1**. Lua mods need no compiler: a `mod.toml` and a few `.lua` files are a complete mod. Everything the native API offers is reachable from Lua (the `fx_parity` test fixture calls every native function through its Lua binding: 70 on the client side, 58 on the server side in API 1.1).

## Scripts and sides

```toml
[scripts]
common = "scripts/common.lua"   # runs on every side the mod declares
client = "scripts/client.lua"   # runs after common, client side only
server = "scripts/server.lua"   # runs after common, server side only
```

- Kyubu runs **one Lua VM per logical side**. In single-player there are two VMs, so `common.lua` runs twice, once per side, with separate state. Use channels, not shared variables, between sides (see [sides-and-networking.md](sides-and-networking.md)).
- Every mod gets its own **environment** per side. Globals a script defines are private to that mod; two mods can both define `config` without clashing.
- Scripts run during PreInit, in mod load order. Code at the top level of a script is PreInit code; subscribe to `lifecycle.register` for registrations.
- `kyubu` is the API table for the running mod (`kyubu.id`, `kyubu.side`, `kyubu.trust`). `print(...)` logs like `kyubu.info(...)`.

## The `kyubu` API

The complete reference, with parameter and return types, is the LuaLS stub file `sdk/lua/kyubu.lua`. Overview:

| Area | Functions |
|---|---|
| Logging and info | `log`, `debug`, `info`, `warn`, `error`, `phase`, `host`, `current_side`, `game_dir`, `data_dir`, `read_file`, `is_mod_loaded`, `manifest`, `mods`, `stats`, `mod_state`, `api_functions` |
| Scheduling | `run_on_main_thread(fn)`, `on_unload(fn)` |
| Events | `on(name, fn, priority?, side?)`, `off(id)`, `emit(name, value?, cancellable?)` |
| Registries | `registry.{create, add, remap, lookup, name, payload, count, entry, hash}`, `model.{add, remap, slot}`, `text.{add, get}` |
| Assets | `assets.{provider, read, served_by}` |
| Channels | `channel.{register, send, SERVER, LOCAL_CLIENT, ALL_CLIENTS}`, `pack`, `unpack` |
| Interfaces | `publish(name, version, pointer)`, `interface(name, min_version)` |
| Console and UI (client) | `command(name, help, fn)`, `ui.{begin, finish, text, button, checkbox, slider_float, input_text, same_line, separator, want_input}` |
| Options | `config.{get, set}`: this mod's own options ([config.md](config.md)) |
| Symbols and memory | `symbol(name)`, `rva_to_addr`, `addr_to_rva`, `read(addr, n)` |
| Full trust only | `write`, `patch`, `game_alloc`, `game_free`, `hook`, `hook_import`, `hook_vtable`, `hook_mid`, `unhook`, `ffi` |
| Game API (full trust only) | `game.{available, enum, creature_get, creature_player, creature_find, creature_spawn}`, creature objects ([game-api.md](game-api.md)) |

Functions that can fail return `nil, "error text"` (for example `nil, "not allowed from this side"` or `nil, "not allowed in this lifecycle phase"`) instead of raising, so `assert(kyubu.registry.create(...))` turns a failure into a script error with the reason.

### Events

Handlers receive an event table with `name`, `side`, `cancellable`, `network_thread`, `cancel()`, and the fields of the event's payload (`ev.vk`, `ev.frame`, `ev.world`...; the full list is in [events.md](events.md) and in the stubs):

```lua
kyubu.on("input.key", function(ev)
  if ev.vk == 0x78 and ev.down then   -- F9
    toggle_window()
    ev:cancel()                        -- the game never sees the key
  end
end, 10)                               -- priority 10: before handlers with priority 0
```

Custom events carry any Lua value within the same VM: `kyubu.emit("mymod:spawned", { x = 1 })` → `ev.value.x`.

### UI

`kyubu.ui` draws Dear ImGui windows. It only works inside `render.overlay` handlers on the client; elsewhere the calls do nothing and return false.

### Game API

`kyubu.game` is the [game API](game-api.md), the checked way into the game's state, for `full` trust only. Its functions return `nil, "reason"` when they fail, and pass creatures as objects: one per creature per VM, so `==` and table keys work; `c:valid()` says whether it still exists, `c:get()` reads it (`{ id, type, hostile, level, hp, pos = {x =, y =, z =}, yaw }`, positions in world units, 65,536 per block) and `tostring(c)` names it. Some functions belong to one side: `creature_player` to the client side, `creature_spawn` to the server side, which a client script reaches with a channel message; `creature_find(id)` gives either side a creature by its game id, so the server side can name the player (whose id the client sends) as an `owner`. `creature_spawn{ ..., behavior = "monster" }` gives the creature the game's default monster behaviour (it comes to the player and attacks); the default `"still"` stands still, `"friendly"` looks at the player and wanders, `"companion"` (with `owner = creature`) follows its owner and fights what it fights, `"guard"` (with `path = { {x, y, z}, ... }`) fights and walks its waypoints, and `"default"` is what the game would give that hostility (the monster's, nothing for a target).

```lua
-- server side: a green slime two blocks north of a position the client sent
kyubu.channel.register("my_mod:spawn", function(_, msg)
  local slime, why = kyubu.game.creature_spawn{ type = 37, hostile = 1, level = 5,
                                                pos = {x = msg.x, y = msg.y + 2 * 65536, z = msg.z} }
  if not slime then kyubu.warn("no slime: " .. why) return end
  kyubu.info("spawned " .. tostring(slime))
end)
```

## Editor support

`sdk/lua/kyubu.lua` is a LuaLS (sumneko Lua Language Server) meta file. The Lua template ships it as `types/kyubu.lua` with a `.luarc.json` that points at it, so VS Code (with the Lua extension), Neovim and other LuaLS editors show completion, parameter hints and type errors. The example mods and the template check without diagnostics (`lua-language-server --check <mod folder>`); the test `tests/tools/test_lua_stubs.py` runs that check.

## Errors

- A Lua error is logged with a stack traceback and the script and line, under the mod's name and side.
- A handler (event, command, channel, provider, hook) that raises **more than 20 errors within one second** is disabled, with a log line saying so. The rest of the mod keeps running.
- An error in a hook handler is logged and the call falls through to the next hook or the original function, so the game gets a valid result.
- An error while a script loads (a syntax error, or an error at the top level) fails the mod: it is listed as `failed` with the error as the reason.

## Trust levels

`trust` in `mod.toml` decides what a Lua mod may do:

| | `full` (default) | `restricted` |
|---|---|---|
| Standard libraries | all (`io`, `os`, `debug`, `package`, `require`, `load`...) | `assert`, `error`, `ipairs`, `next`, `pairs`, `pcall`, `select`, `tonumber`, `tostring`, `type`, `unpack`, `xpcall`, `rawget`, `rawset`, `rawequal`, `setmetatable`, `getmetatable`, `string`, `table`, `math`, `bit`, `coroutine` |
| `kyubu.ffi` (LuaJIT FFI) | yes | no (`require("ffi")` raises "FFI is unavailable at trust level 'restricted'") |
| Hooks, patches, `kyubu.write`, `game_alloc` | yes | no (raises an error naming the trust level) |
| `kyubu.game` (the [game API](game-api.md)) | yes | no (`kyubu.game` is `nil`) |
| Everything else in `kyubu` | yes | yes |

Restricted mods can still read memory with `kyubu.read` and use events, registries, assets, channels, UI and commands. Prefer `restricted` when a mod does not need more; players can see the trust level in the log.

Full-trust Lua is as powerful as native code. Only install mods you trust.

## Hooks from Lua (full trust)

Lua hooks use LuaJIT FFI callbacks. Give the C type of the target, including its calling convention:

```lua
local reads = 0
kyubu.hook("Database::get", "bool(__thiscall*)(void*, void*, void**, uint32_t*)",
  function(next, self, key, out, len)
    reads = reads + 1
    return next(self, key, out, len)
  end)

kyubu.hook_mid("ModelLoader::resizeSite", function(ctx)   -- ctx is a KyubuCpuContext*
  kyubu.info(("model table sized, esp=0x%X"):format(ctx.esp))
end)
```

- `next` is the next hook or the original function. Call it with the same arguments unless you mean to replace the function.
- Each VM has a lock. The game may call a hooked function from any thread; the hook stub takes the VM lock before entering Lua, so Lua state is never used by two threads at once. Keep hook handlers short: a slow handler on a hot function slows the game.
- A handler is valid until `kyubu.unhook(id)` or the mod unloads. Kyubu keeps the FFI callback alive for you.

## Game types (full trust)

`require("cubeworld")` returns the game's types for the LuaJIT FFI. They are generated from `mapping/` and are the same structs as the C headers in `sdk/include/cubeworld/`. The module ships in `kyubu/lua/`; restricted mods cannot `require` it.

```lua
local cubeworld = require("cubeworld")
local creature = cubeworld.cast("cube_CreatureData", address)   -- a cube_CreatureData* at a game address
kyubu.info(("hp %.0f, level %d"):format(creature.hp, creature.level))
```

- `cubeworld.cast(name, address)` takes the C name (`cube_CreatureData`) or the mapped name (`cube::CreatureData`), and an integer, lightuserdata or cdata address. Fields read and write game memory directly.
- `cubeworld.types`, `cubeworld.enums` and `cubeworld.functions` describe the mapping:
  - each type's C name, size, status and fields (offset, type, `verified` or `candidate`);
  - each enum's values;
  - the function-pointer C type of each function that has a prototype. As in the C headers, `__thiscall` functions are typed `__fastcall` with an unused second parameter (EDX).
- Candidate fields are not confirmed yet. `cubeworld.check()` lists every verified field and known size where LuaJIT's layout differs from the mapping; it returns an empty table when they agree.
- Renamed names keep working, as in the C headers. A renamed field keeps its old name at the same offset (listed in its `aliases`). A renamed type keeps its old C name, and `cubeworld.cast` takes its old mapped name (`cubeworld.aliases` maps old names to new ones). A renamed function keeps its old name in `cubeworld.functions`, and a renamed enum value its old key. The stubs mark the old names deprecated.
- For editor completion, add `sdk/lua/cubeworld.lua` to your `workspace.library` next to `kyubu.lua`. The Lua template in the SDK zip already has both in `types/`. Hovering a field shows its C type, offset and status.

## Hot reload (dev mode)

With dev mode on (`--kyubu-dev` or `loader.dev_mode = true`), Kyubu watches every script of every Lua mod. When a file changes it:

1. calls the mod's `kyubu.on_unload` functions (newest first);
2. removes everything the mod's scripts registered on that side: event handlers, hooks, patches, providers, channel handlers, commands, interfaces;
3. creates a fresh environment and runs the scripts again.

Registry entries are kept (registries are frozen after the Register phase), so a reload must not depend on registering new ids. The console command `reload <mod>` does the same on demand. `lua <client|server> <code>` runs a line of Lua in a side's VM, e.g. `lua server return 1 + 1` prints `2`.

## Example

The Lua template (`sdk/templates/lua-mod`) opens a window with a button that asks the server side for an answer over a channel.

`scripts/client.lua`:

<!-- sample: sdk/templates/lua-mod/scripts/client.lua -->
```lua
-- Client side: UI, input, console commands.
local open = kyubu.config.get("show_window") ~= false   -- an option from config.toml
local clicks = 0

kyubu.on("render.overlay", function()
  if not open then return end
  local visible, still_open = kyubu.ui.begin("My Lua Mod", true)
  if visible then
    kyubu.ui.text(kyubu.text.get("my_lua_mod:hello") or "")
    if kyubu.ui.button("ask the server") then
      kyubu.channel.send("my_lua_mod:ask", kyubu.channel.SERVER, { clicks = clicks })
    end
    kyubu.ui.text("answers: " .. clicks)
  end
  kyubu.ui.finish()
  open = still_open
end)

kyubu.channel.register("my_lua_mod:answer", function(_, msg)
  clicks = msg.clicks
end)

kyubu.command("my_lua_mod:show", "show the window again", function(_, print)
  open = true
  print("window shown")
end)
```

`scripts/server.lua`:

<!-- sample: sdk/templates/lua-mod/scripts/server.lua -->
```lua
-- Server side: game rules and answers to clients.
kyubu.channel.register("my_lua_mod:ask", function(connection, msg)
  kyubu.channel.send("my_lua_mod:answer", connection, { clicks = msg.clicks + 1 })
end)
```

More: `mods-examples/hello_lua` (overlay window, F9 toggle, text registry, ping/pong over channels, a console command) and `mods-examples/entity_spawner` (the game API: a window of every entity type; a click asks the server side over a channel to spawn one in front of the player).
