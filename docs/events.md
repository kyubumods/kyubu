# Game events

Mods react to the game through events. A handler subscribes to an event name and is called with a `KyubuEvent` (native) or an event table (Lua) every time the event fires.

```cpp
// native (kyubu.hpp)
mod.on("frame.begin", [](KyubuEvent& ev) { auto* f = kyubu::Mod::data<KyubuFrameData>(ev); /* f->frame, f->dt */ });
```
```lua
-- Lua
kyubu.on("input.key", function(ev) if ev.vk == 0x70 and ev.down then ev:cancel() end end)
```

## Rules

- **Order.** Handlers with a higher priority run first (`subscribe(..., priority, ...)`, default 0). Equal priorities run in mod load order, then in subscription order.
- **Cancelling.** Events marked *cancellable* can be cancelled (`api->event_cancel(ev)` / `ev:cancel()`). Cancelling stops the event: handlers after the one that cancelled are not called, and the game does not see it (for example a cancelled key press never reaches the game). Cancelling a non-cancellable event has no effect.
- **Sides.** Every event is delivered on a logical side, `client` or `server` (see [sides-and-networking.md](sides-and-networking.md)). A subscription is for one side: by default the side of the code that subscribes; native mods can pass `KY_SIDE_BOTH`. Single-player `Cube.exe` runs both sides, a multiplayer client only the client side, `Server.exe` only the server side. "Both" in the table below means the event is sent once to each side that is active in the process.
- **Threads.** Handlers run on the game's main thread unless the table says otherwise. Network events can run on the game's socket thread; `ev.network_thread` tells you. From another thread only logging, `channel_send` and `run_on_main_thread` are safe.
- **Faults.** A handler that crashes is caught; its mod is marked *faulted* and disabled, and the remaining handlers still run. A Lua handler that raises errors more than 20 times per second is disabled.
- **Unavailable events.** Some events depend on game functions that are not verified on every build. Subscribing to such an event fails with `KY_E_UNAVAILABLE` and a log line explaining why, instead of silently never firing.
- **Payload lifetime.** `ev.data` and its strings are valid only during the call. Copy what you need.

## Catalogue

### Lifecycle

| Event | Side | Payload | When |
|---|---|---|---|
| `lifecycle.preinit` | both | - | After native mods were initialised and scripts loaded. Create registries now. |
| `lifecycle.register` | both | - | Registration phase: add registry entries, remaps, models, text. |
| `lifecycle.post_register` | both | - | Registries are frozen; numeric ids can be looked up. |
| `lifecycle.game_ready` | both | - | `Cube.exe`: first frame (start menu). `Server.exe`: the server started listening. The asset overlay summary has been logged. |
| `lifecycle.shutdown` | both | - | The process is exiting normally. Save your data here. |
| `lifecycle.world_enter` | both | `KyubuWorldData { world }` | A world was loaded. Single-player: the world name, sent to both sides after the per-world id map was applied. Multiplayer client: `remote:<address>`, client side only. |
| `lifecycle.world_exit` | both | `KyubuWorldData { world }` | The world was closed (back to the menu, disconnect, or another world). |
| `registry.remapped` | both | - | Loading a world's id map changed the numbers of some registry entries. Refresh cached numbers. |

### Frame and render (client)

| Event | Payload | Notes |
|---|---|---|
| `frame.begin` | `KyubuFrameData { frame, dt }` | Start of every main-loop iteration, before the game updates. `dt` is milliseconds since the previous frame. Queued `run_on_main_thread` tasks and channel messages are delivered just before it. |
| `frame.end` | `KyubuFrameData { frame }` | After the game's frame: update, render and `Present`. |
| `render.overlay` | `KyubuRenderData { device, width, height }` | Inside the overlay's Dear ImGui frame, just before `Present`. The only place where `ui_*` calls draw. `device` is the `IDirect3DDevice9*`. |
| `render.device_lost` | `KyubuRenderData` | Before the D3D9 device is reset (window resized, alt-tab from fullscreen). Release your `D3DPOOL_DEFAULT` resources. |
| `render.device_reset` | `KyubuRenderData` | After the reset succeeded. Re-create them. |

### Input (client, cancellable)

Input events come from the game window's message loop. Cancelling one hides it from the game. While the Kyubu overlay wants the mouse or keyboard (a window is hovered, a text field or the console is active) the overlay consumes the input before these events are sent.

| Event | Payload | Lua fields |
|---|---|---|
| `input.key` | `KyubuKeyData { vk, down, repeat }` | `vk` (Windows virtual-key code), `down`, `is_repeat` |
| `input.char` | `KyubuCharData { codepoint }` | `codepoint`, `char` (UTF-8) |
| `input.mouse_button` | `KyubuMouseButtonData { button, down, x, y }` | `button` (0 left, 1 right, 2 middle), `down`, `x`, `y` |
| `input.mouse_move` | `KyubuMouseMoveData { x, y, dx, dy }` | `x`, `y`, `dx`, `dy` |
| `input.mouse_wheel` | `KyubuMouseWheelData { delta }` | `delta` (120 per notch) |

### Assets

| Event | Side | Payload | Notes |
|---|---|---|---|
| `asset.db_open` | host | `KyubuDbOpenData { path }` | The game opened an SQLite database: `data1-4.db`, and in single-player the world saves under `Save\`. |
| `asset.request` | host | `KyubuAssetRequestData { db, key }` | The game reads one key, e.g. `data1.db` / `iron-sword1.cub`. Subscribe during PreInit to see every read: the game starts with `data4.db/dict_en.xml`, then loads the model table (about 2,560 `data1.db` keys) and icons. Reads from the save databases (`characters.db`, `worlds.db`) are reported too. |
| `asset.models_loaded` | both (client process) | - | The model table, including mod slots, has been loaded. |

"host" means the client side in `Cube.exe` and the server side in `Server.exe`.

### Network (raw)

These events show the game's own TCP traffic (port 12345). They are for inspection and debugging; mod-to-mod messages use channels instead.

| Event | Side | Payload | Notes |
|---|---|---|---|
| `net.connect` | client (Cube.exe) / server (Server.exe) | `KyubuNetConnData { socket, address, port }` | The client connected to a server / the server accepted a client. |
| `net.disconnect` | same | `KyubuNetConnData` | The connection closed. |
| `net.recv_raw` | host | `KyubuNetData { socket, data, len }` | Bytes received, before the game parses them. Often on the socket thread. |
| `net.send_raw` | host | `KyubuNetData { socket, data, len, mutable_data, mutable_len }` | Bytes about to be sent. Handlers may rewrite the buffer in place (not grow it) or drop the send (`*mutable_len = 0`; Lua: `ev:set_data(bytes)`). |

### Simulation

| Event | Side | Payload | Notes |
|---|---|---|---|
| `sim.tick` | server | `KyubuFrameData { frame, dt }` | One game-state update, after the game's update ran. Single-player `Cube.exe`: every `GameController::update` while a world is loaded (not in the start menu). Multiplayer client: never (the server simulates). `Server.exe`: once per server tick, after each `cube::World::update` (main's loop runs it exactly once per pass; about 50 ticks a second, `dt` 20-21 ms), from the first World step on. Unavailable on a build where the game API's `world` is unavailable. |
| `sim.world_step` | both | `KyubuWorldStepData { world, dt, step_out, effects }` | One World step, inside `cube::World::update`, after the hits and buffs the clients sent and before the zone items and the creatures (`cube::World::worldStepSite`). Every side whose World it is: single-player `Cube.exe` both, a joined client its client side, `Server.exe` its server side. `Cube.exe` steps the World in 20 ms pieces, so a frame can hold several. `creature_hit` is allowed from here to the end of the step. Unavailable where the game API's `world_step` is. |

### Options

| Event | Side | Payload | Notes |
|---|---|---|---|
| `config.changed` | both | `KyubuConfigChangedData { mod, key }` | An option of mod `mod` changed after its init: set through the API or the console, or read again by `config reload`. Main thread, next frame; sent to every side the mod runs on. Not sent when the value stays the same. [config.md](config.md) |

### Custom events

Mods can define their own events named `<mod id>:<name>` and send them with `api->emit(...)` / `kyubu.emit(name, value, cancellable)`. Only the owning mod can emit them; any mod can subscribe. Native payloads are passed through as a pointer and size; in Lua the emitted value is `ev.value`, and `emit` returns whether a handler cancelled the event.

## Checking this list

`tools/check_docs.py` extracts every event name the loader emits or marks unavailable and fails if one is missing from this page.
