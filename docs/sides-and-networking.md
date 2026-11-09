# Sides, channels and multiplayer

Cube World splits into a **client** (rendering, input, UI, sound) and a **server** (the authoritative world simulation). `Cube.exe` contains both and runs the server part in-process for single-player; `Server.exe` is the dedicated server. Kyubu makes that split explicit so a mod written today keeps working when a modded dedicated server exists.

## Logical sides

Every piece of mod code runs on a logical side: `client` or `server`. Which sides exist depends on how the game runs:

| Host mode | Process | Client side | Server side |
|---|---|---|---|
| Start menu and single-player | `Cube.exe` | yes | yes (in-process) |
| Multiplayer client | `Cube.exe` connected to a server | yes | **no** |
| Dedicated server | `Server.exe` | no | yes |

- Scripts: `mod.toml` lists `client`, `server` and `common` scripts; `common` runs once per active side. Lua keeps **one VM per side**, so a common script really runs twice in single-player, with separate state.
- Native mods: `kyubu_mod_init(api, handle, side)` is called once per side the mod declares (`sides = [...]`, and `KyubuModQuery::sides`). Each call gets its own handle; everything registered with a handle belongs to that side.
- Events are side-tagged (see [events.md](events.md)); a server-side handler never runs in a multiplayer client.
- When single-player `Cube.exe` connects to a server, the server side stops: server-side handlers get `lifecycle.world_exit` and no more `sim.tick`. The log says `connected to <addr> (multiplayer: client side only)`.

`api->current_side()` / `kyubu.current_side()` return the side of the running callback. `api->host_info()` / `kyubu.host()` return the process and host mode.

## Side-checked API

Every API function has a side rule, kept in one table (`api.cpp`, exported as `kyubu.api_functions()`). Calling a function from the wrong side fails, even in single-player where both sides share a process:

| Rule | Functions |
|---|---|
| Client only | `register_command`, `imgui_context`, `ui_begin`, `ui_end`, `ui_text`, `ui_button`, `ui_checkbox`, `ui_slider_float`, `ui_input_text`, `ui_same_line`, `ui_separator`, `ui_want_input` |
| Server only | none in API 1.0. Authoritative world and entity functions will be server-only when they are added. |
| Either side | everything else |

A refused call returns `KY_E_SIDE` (UI calls return 0 / false and draw nothing) and logs one warning per mod and function. The test suite checks that every member of `KyubuApi` has a rule and that each client-only function refuses a server-side caller; the `fx_parity` fixture checks the same from Lua in game.

## Channels

Channels carry mod messages between the sides. A channel is named `<mod id>:<name>` and has at most one handler per side.

```lua
-- server side (scripts/server.lua)
kyubu.channel.register("hello_lua:ping", function(conn, msg)
  kyubu.channel.send("hello_lua:pong", conn, { n = msg.n, from = "server" })
end)

-- client side (scripts/client.lua)
kyubu.channel.register("hello_lua:pong", function(conn, msg) kyubu.info("pong " .. msg.n) end)
kyubu.channel.send("hello_lua:ping", kyubu.channel.SERVER, { n = 1 })
```

```c
/* native */
api->channel_register(self, "demo:echo", KY_SIDE_SERVER, on_echo, NULL);
api->channel_send(self, "demo:echo", KY_CONN_SERVER, bytes, len);
```

| Rule | Detail |
|---|---|
| Directions | Client → server (`KY_CONN_SERVER`); server → one client (its connection id) or all clients (`KY_CONN_ALL_CLIENTS`). Other directions fail with `KY_E_SIDE`. |
| Connection ids | Handlers receive the sender's connection: `KY_CONN_SERVER` (0) on the client, the client's id on the server. In single-player the local client is `KY_CONN_LOCAL_CLIENT` (1). |
| Payload | Bytes, at most 64 KiB (`KY_E_TOO_LARGE` above). The payload is copied at send time. Lua tables are serialised with `kyubu.pack` (nil, booleans, numbers, strings, nested tables). |
| Ordering | Messages on one channel arrive in the order they were sent. |
| Delivery | Never inside `send`. A message is delivered on the receiving side's next update (start of the next frame), exactly as it would be over a network. |
| Threads | `channel_send` is safe from any thread; handlers run on the main thread. |

### Loopback transport

In single-player both sides live in one process. Kyubu's loopback transport queues messages per side and delivers them at the start of the receiver's next update, so code that works in single-player does not depend on synchronous delivery that a real network would not give it.

### Joined games

When `Cube.exe` joins a Kyubu `Server.exe` and a join gate is in force (both have the same gate mods: `gate = true` in `mod.toml`),
a channel message travels as one Kyubu packet on the game's own connection, with the same rules: ordered per channel, at most
64 KiB, delivered at the receiver's next frame or tick. The server sees each joined player as connection id `creature id + 1`
(players are creatures 1 to 10, so 2 to 11); a reply to that id goes to that player only, `KY_CONN_ALL_CLIENTS` to every joined
player. A connection is known from its first send pass after the join (a few ms); before that a send to it returns `KY_E_NOT_FOUND`,
so retry at the next tick. Without a gate mod there is no transport and `channel_send` to the other end returns `KY_E_NO_REMOTE`:
the other end may be vanilla. The server side of a joined `Cube.exe` has no remote end either (`KY_E_NO_REMOTE`).

## Session manifest

When a world is entered, Kyubu logs a **session manifest**, a JSON summary of what this session's content depends on. `api->session_manifest()` and `kyubu.manifest()` return it:

```json
{
  "build": "cube-0x51D2CE9C",
  "host": "client",
  "loader_api": "1.0",
  "protocol": 1,
  "mods": [
    { "id": "hello_native", "version": "1.0.0", "sides": ["client", "server"], "requires_server": false }
  ],
  "registries": {
    "kyubu:models": "d1ee19c797ad7ffa9c828dc56d3502ddac8f995cd1f0c9c868ded69d70e798e6",
    "kyubu:text": "42f45a06c6bcf85485daf368946345e639c178ac0c65a3f3b2de0a5aa2b70409",
    "kyubu:icons": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "kyubu:sounds": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
  },
  "animations": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
}
```

Registry hashes are SHA-256 over the frozen entries as sorted `id=number` lines, so two processes with the same hash number every entry the same way. Register the same entries on both sides (registering an identical entry twice is allowed), so that a dedicated server, which only runs server-side code, ends up with the same registries as a client. A future Kyubu server compares manifests during its handshake ([networking-future.md](networking-future.md)).

`animations` is the hash of the animation clips that the enabled mods ship (`assets/animations/*.anim.toml`): SHA-256 over one `id`, `SHA-256 of the file's text` pair of lines per clip, in id order. Clips have string ids only: they have no numbers and are not in `registries` or in a save. The value above is the hash of no clips.

## Playing on vanilla servers

Kyubu never sends anything but the game's own bytes to a server, so a Kyubu client can join any vanilla server. What mods can do there depends on the mod:

| Mod declares | On a vanilla server |
|---|---|
| nothing (cosmetic: textures, models remapped on the client, UI, client scripts) | Works. Channels to the server fail with `KY_E_NO_REMOTE`, because the server has no Kyubu. |
| `requires_server = true` in `mod.toml` (content the server must know about) | Connecting is **blocked**: a message names the mods that need a Kyubu server, and the connection attempt fails. Disable those mods to join. |

A `Server.exe` with Kyubu and no gate mod runs as a vanilla server on the wire: vanilla clients and Kyubu clients with cosmetic mods can join it. With a gate mod it takes only clients with the same gate mods, and channels work both ways ("Joined games" above). The meta port of [networking-future.md](networking-future.md), for mixed servers, is not built.
