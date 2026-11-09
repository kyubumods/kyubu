# Game API

The game API is Kyubu's checked way into the game's state. Each of its functions finds the World of the calling side,
runs on the game thread under the World's lock, passes creatures as handles that cannot reach freed memory, refuses
to change a creature the calling side does not own, and uses only `verified` mapping entries.

API 1.1 ships the **base layer**, [`game_available`](#game_available) and [`game_enum`](#game_enum), the first
functions that read the game, [`creature_get`](#creature_get) and [`creature_player`](#creature_player), and the first
that changes it, [`creature_spawn`](#creature_spawn). More functions come one at a time, when a mod needs them
([Adding a function](#adding-a-function)).
Until then, full-trust mods reach the game with raw access: `require("cubeworld")` from Lua ([lua.md](lua.md)) or
the `cubeworld/` headers and hooks from native code ([hooks.md](hooks.md)), at their own risk.

From Lua the game API is `kyubu.game`, for `full` trust only: at trust level `restricted`, `kyubu.game` is `nil`.
From C it is the members after the `/* ---- game API ---- */` marker in `KyubuApi`; check `api->size` before using
them, because a 1.0 loader does not have them. From C++, `kyubu::Mod` wraps each of them under the member's name, and
the wrapper checks `api->size` itself: on a 1.0 loader it returns `KY_E_UNAVAILABLE` (`game_available` returns
`false` and `game_enum` an empty list, with the reason).

## Primitives and availability

A primitive is a group of game-API functions and the mapping entries they use. The list of primitives, entries and
functions is [`loader/src/game/entries.inc`](../loader/src/game/entries.inc); the entries are described in
[`mapping/atlas/primitives.md`](../mapping/atlas/primitives.md), under the section each primitive names.

At boot the loader checks every entry against the running build. A primitive is available when each of its entries
for that build is `verified`: functions, sites and globals in the build's symbol map, fields and types in the
loader's layouts. Every primitive also needs `world`, and a function that takes or returns a creature also needs
`handles` (without its destructor detour a handle could outlive its creature). When a primitive is unavailable, each
of its functions returns
`KY_E_UNAVAILABLE` without touching game memory, and the log names the primitive and the blocking entry once:

```
game API: handles is unavailable on this build: cube::Creature::~Creature: candidate, not verified (cube-0x51D2CE9C)
```

| Primitive | Section of primitives.md | Cube.exe | Server.exe | What it is | Functions |
|---|---|---|---|---|---|
| `world` | Foundation | yes | yes | The World of each side, the game thread, the World's lock, the safe points around each World step | none yet |
| `handles` | Foundation | yes | yes | Creature handles (the destructor detour invalidates them) and ownership | none yet |
| `entity_state` | Entity state | yes | yes | A creature's verified state: id, type, hostility, level, hp, position, facing | `creature_get`, `creature_player`, `creature_find`, `creature_teleport` |
| `items` | Items | yes | not yet (its call is mapped on `Cube.exe` only) | Putting an item into a creature's bags with the game's own insert, built from the item fields the probes verified | `creature_give_item` |
| `spawn` | Spawn | yes | not yet (its calls are mapped on `Cube.exe` only) | Building a creature with the game's own recipe: its id rule, allocator, constructor, max hp, look from type and creature-map insert | `creature_spawn` |
| `behavior` | Behavior | yes | not yet (its calls are mapped on `Cube.exe` only) | The creature's behaviour pointer and the constructors of the zone default's sequence, combat, look-at-player and random-walk behaviours; checked only when a spawn asks for a behaviour | `creature_spawn` with a behaviour other than `KY_SPAWN_STILL` |
| `behavior_companion` | Behavior | yes | not yet (its calls are mapped on `Cube.exe` only) | The companion behaviour's constructor (with `behavior`); checked only for a companion | `creature_spawn` with `KY_SPAWN_COMPANION` |
| `behavior_path` | Behavior | yes | not yet (its calls are mapped on `Cube.exe` only) | The walk-path behaviour's constructor and the waypoint append (with `behavior`); checked only for a guard | `creature_spawn` with `KY_SPAWN_GUARD` |
| `ability_state` | Abilities | yes | yes | A creature's mode, mode time, mp, hp and cooldown map (the fourth ability slot's state) | `ability_get`, `ability_cooldown`, `ability_cooldown_set`, `ability_end` |
| `ability_stun` | Abilities | yes | yes | The stun timer; outside the slot's set: with it a stun ends an ability with `stunned`, without it with `interrupted` | none yet |
| `ability_step` | Abilities | yes | yes (verified 2026-10-27) | The per-creature dispatch site where a mod's ability runs its per-frame code | none yet |
| `ability_bar` | Abilities | yes | not yet (its site and call are mapped on `Cube.exe` only) | The skill bar's build site, its vector and the append, with the class and specialization that pick its slots | none yet |
| `ability_stats` | Abilities | yes | yes (verified 2026-10-27) | The cooldown, cost and mode-duration functions a mod's ability answers for | none yet |
| `ability_icons` | Abilities | yes | not yet (its map is mapped on `Cube.exe` only) | The icon map the HUD reads for each ability id | none yet |
| `ability_use` | Abilities | yes | not yet (its calls are mapped on `Cube.exe` only) | The game's `modeInterruptible` and `canUseAbility`, which `ability_start` asks before it writes a mod ability's number; outside the slot's set | `ability_start` |
| `ability_ranks` | Abilities | yes | not yet (its detours are mapped on `Cube.exe` only) | The played slot (seen by `load_character` and `save_character`) and the player's name, the key of a rank table; outside the slot's set: without it every rank reads 1 | `ability_rank_get`, `ability_rank_set` |
| `ability_iconfile` | Abilities | yes | not yet (its call is mapped on `Cube.exe` only) | The game's icon loader, which Kyubu calls once for an ability's own `icon_file` (a `data3.db` key); outside the slot's set: without it every ability shows its vanilla `icon` | none (it changes what `ability_define`'s `icon_file` does) |
| `ability_skilltree` | Abilities | yes | not yet (its site and fields are mapped on `Cube.exe` only) | The skill panel's tenth button (site that hides it, the button vector, the icon map) and `skills`; outside the slot's set, and used only with `[abilities] skill_tree = true`: with it the tree teaches the fourth ability and its rank is `skills[9]` | none (it changes `ability_rank_get` and `ability_rank_set`) |
| `animation_pose` | Drawing | yes | not yet (its detour and site are mapped on `Cube.exe` only) | The pose provider's entries: the function hook on `draw_creature` (the borrowed look, installed at boot when this primitive is available; otherwise one log line says the provider is off), the join site after the pose switch (the override: a mid hook that writes a clip's tracks over the pose vectors for one draw) and the game controller global (which creature is the local player). `ability_animation_bind` asks for it on the client side only; a server side records the binding and draws nothing | `ability_animation_bind` |
| `ui` | Interface | yes | no (Server.exe has no plasma engine) | The UI's scene tree and the render device: the engine (`cube::GameController.engine`), its root node, a node's name, parent, children and display colour, and the device and client size. Read only; the functions take no World (the title screen has none that steps), so they work from the first frame, on the main thread | `ui_node_find`, `ui_node_get`, `ui_node_child`, `render_get` |
| `particles` | Particles | yes | not yet (its push is mapped on `Cube.exe` only) | One of the game's own particle effects, pushed on the World step's output with the game's list push after the step ran: the output's `particles` list, the push and the `cube::ParticleData` record | `particle_spawn` |
| `creature_flags` | Creature flags | yes | not yet (its sites are mapped on `Cube.exe` only) | `KY_CREATURE_UNSTOPPABLE` and `KY_CREATURE_INVULNERABLE`: mid hooks on `applyHit`'s push, velocity-reset, slow and stun sites and on the two damage sites (`hitListSite`, `hitDamageSite`) | `creature_set_flags` |
| `creature_body` | Creature bodies | yes | not yet (its sites are mapped on `Cube.exe` only) | Model pieces on a rig, posed by clips and joint offsets, drawn in place of a creature's own parts: the hide mask at `creature_draw_site`, the pieces at `draw_creature_end_site`, the death burst at `creature_death_burst_site` | `body_define`, `body_name`, `creature_body_attach`, `creature_body_play`, `creature_body_offset`, `creature_body_weight`, `creature_body_read`, `creature_body_point` |
| `lights` | Lights | yes | no (Server.exe draws nothing) | Point lights of mods in the frame's light vector: the site before the render sorts the vector and hands it to the terrain and the models, the vector's push and the `cube::Light` record | `light_spawn`, `light_set`, `light_remove` |
| `terrain` | Terrain | yes | yes | The block at a point: the walk from the World's region slots through the region's zone slots and the zone's block columns, the same on both builds | `world_block` |
| `world_step` | World step | yes | yes | The `sim.world_step` event at a site of each World step, and hits through the game's `hitCreature` with the mod's stun and direction | `creature_hit` |
| `creature_root` | Creature root | yes | yes | `KY_CREATURE_ROOTED`: mid hooks at the AI tick (the behaviour is not run, the mode is held at none) and at the move step (no step but a fall, no knockback) | `creature_set_flags` (the `rooted` bit) |
| `creature_state` | Creature state | yes | yes | A creature's position, velocity, knockback, mode and mode time, stun, slow, charge and aim, read and written by their mapped offsets | `creature_state_get`, `creature_state_set` |
| `net_transport` | Network transport | yes | yes | `kyubu.channel` between `Server.exe` and a joined `Cube.exe`: one Kyubu packet on the game connection, read at each receive loop and written by each send thread between game packets, only with a join gate in force ([sides-and-networking.md](sides-and-networking.md), "Joined games") | `channel_send` (to the other end of a joined game) |
| `player_shots` | Player shots | no (Server.exe only) | yes | The projectile packets joined clients send, copied at the receive thread before the game queues them for the relay, and a projectile pushed into the server World's list, which its projectile step flies | `shot_take`, `projectile_push` |
| `server_hits` | Server hits | no (Server.exe only) | yes | A fix of the game, no function: Server.exe pushed each of `hitCreature`'s hits on a player twice, so a joined player lost twice the damage; the loader drops the second push | none |

Only builds with a symbol map reach the game API: on an unknown build the loader runs in passthrough and loads no
mods. A mod asks at run time with [`game_available`](#game_available); the `game` console command
([dev-tools.md](dev-tools.md)) shows each primitive with the entry that blocks it.

## Result codes

Game-API functions use the loader's result codes ([native-api.md](native-api.md)). These are the ones the guard
returns before a function does anything:

| Code | Value | When |
|---|---|---|
| `KY_E_INVALID_ARG` | -1 | Not a mod handle, a null pointer, or a creature handle that is 0 or not a handle. |
| `KY_E_PHASE` | -2 | No world is loaded: before `lifecycle.world_enter`, after `lifecycle.world_exit`, or on `Server.exe` before its first World step. |
| `KY_E_SIDE` | -3 | The calling side has no World on this host (a joined client's server side, `Server.exe`'s client side), a handle issued on the other side, or a change to a creature the calling side does not own. |
| `KY_E_UNAVAILABLE` | -5 | The function's primitive is unavailable on this build; the log and `game_available` name the entry. |
| `KY_E_UNSAFE` | -7 | Not on the game thread (for example in a `net.recv_raw` handler on the socket thread). Use `run_on_main_thread`. |
| `KY_E_FAULTED` | -16 | The calling mod was disabled after a fault. |
| `KY_E_GONE` | -19 | New in 1.1: the creature a handle named was destroyed, or the world was left. |
| `KY_E_REFUSED` | -20 | New in 1.1: the game state refuses the request: `ability_start` on a creature whose ability is on cooldown, without mp, or busy. The log and Lua's second result give the reason. |

Each refusal is written to the log once per mod, function and reason, for example
`creature_example refused: not on the game thread (use run_on_main_thread)`. In Lua a game function that fails
returns `nil, "reason"` (like the rest of the Lua API; `available` returns `false, "reason"`).

## Sides, owners and threads

### The World of each side

Each logical side ([sides-and-networking.md](sides-and-networking.md)) uses the World it would have with a real
server:

| Host | Client side | Server side |
|---|---|---|
| Single-player `Cube.exe` | the game controller's World | the same World |
| Joined `Cube.exe` | the game controller's World | none (`KY_E_SIDE`) |
| `Server.exe` | none (`KY_E_SIDE`) | the World its main loop steps (`cube::World::update`'s `this`) |

The side of a call is the side of the mod handle that makes it: a mod's client-side code passes its client handle.

### Owners

A function that changes a creature, or acts for it, works only from the side that owns the creature, in
single-player too, so a mod tested in single-player does what it would do against a real server:

- On a client, its local player (`cube::World.local_player`) is owned by the client side.
- Any other creature is owned by the server side, except players, which are owned by their own clients.
- `Server.exe` cannot tell a player's creature from a monster yet, so there the server side owns every creature.

A refused change returns `KY_E_SIDE`, and the log says which side owns the creature. Functions that only read work
on both sides. A client-side script that wants to change a monster sends a channel message to its server side.

### Creature handles

A creature crosses the API as a `KyubuCreature`, an opaque `uint64_t` (0 = none) that the loader issues. The same
creature keeps the same handle on a side while it lives, so handles compare equal and work as map keys. When the game
destroys the creature, or the world is left, its handles become invalid: using one returns `KY_E_GONE` instead of
reading freed memory. A handle is valid only on the side that issued it (`KY_E_SIDE` on the other); game ids, not
handles, cross sides.

In Lua a creature is an object, one per handle per VM, so `==` and table keys work. `c:valid()` says whether the
creature still exists, without raising, on any thread; `c:get()` is [`creature_get`](#creature_get); `tostring(c)`
gives `creature 0x<handle> (id <game id>)`, or `(gone)`. Once the creature is gone, its methods return `nil` and
`"the creature is gone"`, like any failing game function. (Properties such as `c.hp`, which will raise on a gone
creature, come with the first function that writes them.) The VM is shared by the side's mods: a method calls the
game API as the mod that got the object last.

### Game thread, the lock and safe points

- **Game thread only.** Game functions run on the thread that steps the World (`Cube.exe`'s main thread,
  `Server.exe`'s main loop). From any other thread they return `KY_E_UNSAFE`; `run_on_main_thread` is the way back.
- **The lock.** Other threads take the World's lock (`cube::World.lock`) in every mode: the zone loader, and in
  multiplayer each connection's receive and send threads. Each game-API call holds the lock for its whole run, so
  keep the work short. Inside a World step the game thread already holds it.
- **Safe points.** The loader detours `cube::World::update` and runs two queues around each step: before the step,
  and after it returns. A change to the creature map (spawn, removal) is made after the step, because the step walks
  the map and mod code can run inside it.
- **`sim.tick` on `Server.exe`** comes from the same detour: once per tick, after the step and its after-step work
  ([events.md](events.md)). On `Cube.exe` it comes from the game controller's update, as before.

## Functions

### `game_available`

Whether a primitive is available on this build, and why not.

- **C:** `KyubuResult game_available(const char* primitive, char* why, uint32_t cap)`: `KY_OK`, or an error with the
  reason in `why` (may be NULL; cut to `cap`).
- **C++:** `bool kyubu::Mod::game_available(const char* primitive, std::string* why = nullptr)`.
- **Lua:** `kyubu.game.available(primitive)` returns `true`, or `false` and the reason.
- **Primitive:** none: it works on every build, before a world is loaded, on any thread.
- **Side:** both.
- **Errors:** `KY_E_UNAVAILABLE` (the reason names the blocking entry and the build, or says that `world` is
  unavailable), `KY_E_NOT_FOUND` (no such primitive; the reason says so), `KY_E_INVALID_ARG` (`primitive` is NULL).
- **Since:** 1.1.

```lua
local game = kyubu.game                  -- nil at trust level "restricted"
local ok, why = game.available("handles")
if not ok then kyubu.warn("no creature handles: " .. why) end
```

```cpp
std::string why;
if (!mod.game_available("handles", &why)) mod.warn("no creature handles: %s", why.c_str());
```

### `game_enum`

The values of a mapped enum and their names, for enums whose mapping status is `verified` in the loader's layouts
(`cubeworld/layout.h`). The list is the running loader's, not the SDK a mod was built with.

- **C:** `KyubuResult game_enum(const char* name, uint32_t index, int64_t* value, char* buf, uint32_t cap)`: the
  `index`-th value (from 0, in value order) in `value` (may be NULL) and its name in `buf`. Walk the indexes from 0
  until `KY_E_NOT_FOUND`.
- **C++:** `std::vector<kyubu::Mod::EnumValue> kyubu::Mod::game_enum(const char* name, std::string* why = nullptr)`:
  the `{value, name}` pairs in value order; an empty list, with the reason in `why`, when the call fails.
- **Lua:** `kyubu.game.enum(name)` returns `{ {value = 0, name = "ELF_MALE"}, ... }` in value order, or `nil` and the
  reason.
- **Primitive:** none: it works on every build, before a world is loaded, on any thread.
- **Side:** both.
- **Errors:** `KY_E_UNAVAILABLE` (the enum is `candidate`; the reason in `buf` says so), `KY_E_NOT_FOUND` (past the
  last value, or no such enum; for an unknown enum the reason is in `buf`), `KY_E_BUFFER` (the name does not fit),
  `KY_E_INVALID_ARG` (`name` is NULL).
- **Since:** 1.1.

```lua
local types, why = kyubu.game.enum("cube::EntityType")
if not types then kyubu.warn("no entity types: " .. why) return end
for _, t in ipairs(types) do kyubu.info(t.value .. " " .. t.name) end
```

### `creature_get`

A creature's verified state, read through its handle.

- **C:** `KyubuResult creature_get(KyubuModHandle self, KyubuCreature c, KyubuCreatureInfo* out)`. Set `out->size`
  to `sizeof(KyubuCreatureInfo)` first: the loader fills what fits, and later versions append fields.

  | Field | Type | Unit |
  |---|---|---|
  | `entity_type` | `uint32_t` | a `cube::EntityType` value ([`game_enum`](#game_enum) names it) |
  | `id` | `int64_t` | the game's id; creatures made at run time have ids below zero |
  | `pos[3]` | `int64_t` | world units: 65,536 per block; x east, y north, z up |
  | `body_yaw` | `float` | facing, degrees counter-clockwise from north (+y); the facing vector is (-sin, cos) |
  | `hp` | `float` | health points |
  | `level` | `int32_t` | level |
  | `hostile_type` | `uint8_t` | a `cube::HostileType` value: 0 player, 1 hostile, 2 friendly, 3 named friendly, 5 pet, 6 target |
  | `flags` | `uint32_t` | 1.3: the `KyubuCreatureFlags` bits `creature_set_flags` set; 0 for none (Lua: a table `{unstoppable =, invulnerable =}` of booleans) |

- **C++:** `KyubuResult kyubu::Mod::creature_get(KyubuCreature c, KyubuCreatureInfo& out)`: sets `out.size` itself.
- **Lua:** `kyubu.game.creature_get(c)`, or `c:get()`, returns
  `{ id, type, hostile, level, hp, pos = {x =, y =, z =}, yaw, flags }`, or `nil` and the reason. Positions stay below
  2^40 units, so they fit a Lua number exactly.
- **Primitive:** `entity_state` (and `handles`).
- **Side:** both, for any creature the side has a handle of.
- **Errors:** `KY_E_GONE` (the creature no longer exists; in Lua `"the creature is gone"`), `KY_E_SIDE` (a handle
  of the other side), `KY_E_INVALID_ARG` (a handle that is 0 or not a handle, `out` NULL, or `out->size` below 8);
  and the guard's ([Result codes](#result-codes)).
- **Since:** 1.1.

### `creature_player`

The client side's own creature, its player (`cube::World.local_player`).

- **C:** `KyubuResult creature_player(KyubuModHandle self, KyubuCreature* out)`: the player's handle in `out` (0
  when the call fails).
- **C++:** `KyubuResult kyubu::Mod::creature_player(KyubuCreature& out)`.
- **Lua:** `kyubu.game.creature_player()` returns the player's creature object, or `nil` and the reason. Asked
  twice, it returns the same object.
- **Primitive:** `entity_state` (and `handles`).
- **Side:** client only (`KY_E_SIDE` from the server side: it has no "local" player).
- **Errors:** `KY_E_PHASE` (no world is loaded: at the start menu, or before the World's first step),
  `KY_E_INVALID_ARG` (`out` is NULL); and the guard's ([Result codes](#result-codes)).
- **Since:** 1.1.

```lua
local player, why = kyubu.game.creature_player()   -- client side
if not player then kyubu.warn("no player: " .. why) return end
local p = player:get()
kyubu.info(("%s at %d, %d, %d facing %.0f degrees"):format(tostring(player), p.pos.x, p.pos.y, p.pos.z, p.yaw))
```

### `creature_teleport`

Moves a creature to a position and stops it (its velocity is set to zero). The creature's own step carries on from
the new place.

- **C:** `KyubuResult creature_teleport(KyubuModHandle self, KyubuCreature c, const int64_t* pos)`: `pos` is three
  values, x east, y north, z up, in units (65,536 to a block).
- **C++:** `KyubuResult kyubu::Mod::creature_teleport(KyubuCreature c, const int64_t pos[3])`.
- **Lua:** `kyubu.game.creature_teleport(c, x, y, z)`, or `c:teleport(x, y, z)`, returns `true`, or `nil` and the
  reason. The coordinates must be whole numbers.
- **Primitive:** `entity_state` (and `handles`).
- **Side:** both, for a creature the calling side owns (on the client side, the local player: in single-player and on a client joined to a server, which follows the move because movement is client-authoritative). Nothing checks the
  ground: the target zone may not be loaded, and the creature falls until it is (the `teleport` developer mod hovers
  the player until the zone loads).
- **Errors:** `KY_E_INVALID_ARG` (`pos` NULL, or a coordinate beyond 2^47 units, about 2.1 thousand million blocks),
  `KY_E_GONE`, `KY_E_SIDE` (a creature the other side owns); and the guard's ([Result codes](#result-codes)).
- **Since:** 1.1.

```lua
local player = kyubu.game.creature_player()
local p = player:get().pos
player:teleport(p.x + 100 * 65536, p.y, p.z + 10 * 65536)   -- 100 blocks east, 10 up
```

### `creature_set_yaw`

Turns a creature: sets its body yaw, in degrees counter-clockwise from north. The position is `creature_teleport`'s.

- **C:** `KyubuResult creature_set_yaw(KyubuModHandle self, KyubuCreature c, float yaw)`.
- **C++:** `KyubuResult kyubu::Mod::creature_set_yaw(KyubuCreature c, float yaw)`.
- **Lua:** `kyubu.game.creature_set_yaw(c, yaw)`, or `c:set_yaw(yaw)`, returns `true`, or `nil` and the reason.
- **Primitive:** `entity_state`.
- **Side:** both, for a creature the calling side owns.
- **Errors:** `KY_E_INVALID_ARG` (`yaw` not finite), `KY_E_GONE`, `KY_E_SIDE`; and the guard's ([Result codes](#result-codes)).
- **Since:** 1.4.

### `creature_set_flags`

Sets a creature's flags, all at once (0 clears them). A flagged creature ignores what a hit does besides taking damage, or
ignores damage:

- `KY_CREATURE_UNSTOPPABLE` (1): a hit gives it no push, no velocity reset, no stun and no slow, and a stun above 0 is ended at
  the next hit. Threat and damage are untouched, so a taunt still works. Mods that apply a status of their own (snare, freeze,
  launch, pull) must read `flags` in `creature_get` and do nothing to an unstoppable creature.
- `KY_CREATURE_INVULNERABLE` (2): a hit's damage is cut to 0 (no hp loss, no damage number); the hit still counts as a hit (sound,
  the attacker's combo). A negative damage (a heal) is not cut.
- `KY_CREATURE_ROOTED` (4, add-skills-unleashed-multiplayer 10.3): the creature does not walk, is not knocked back and does not
  act. At the World's AI tick its behaviour is not run and its mode is held at none (the behaviour stays in place); at its move step
  the step is cut to a fall and its walk push, x and y velocity and knockback are zeroed. It may still turn. A player skips the AI
  tick, so a rooted player is only held in place.

The flags are the loader's and end with the creature or the world. The sleeping-wake stun write of a hit on an ability-0x54 sleeper is
not covered.

- **C:** `KyubuResult creature_set_flags(KyubuModHandle self, KyubuCreature c, uint32_t flags)`.
- **C++:** `KyubuResult kyubu::Mod::creature_set_flags(KyubuCreature c, uint32_t flags)`.
- **Lua:** `kyubu.game.creature_set_flags(c, {unstoppable = true, invulnerable = true, rooted = true})`, or `c:set_flags({...})`, returns `true`, or
  `nil` and the reason; a key left out is false, so `{}` clears. `creature_get` returns `flags` as the same table.
- **Primitive:** `creature_flags` for the first two bits, `creature_root` for `KY_CREATURE_ROOTED` (and `entity_state`, `handles`).
- **Side:** both, for a creature the calling side owns. `KY_CREATURE_ROOTED` works on both exes; the other two on `Cube.exe` only
  (their sites are not mapped for `Server.exe`: `KY_E_UNAVAILABLE` there).
- **Errors:** `KY_E_INVALID_ARG` (a bit that is not defined; nothing changes), `KY_E_GONE`, `KY_E_SIDE` (a creature the other
  side owns); and the guard's ([Result codes](#result-codes)).
- **Since:** 1.3.

```lua
local boss = kyubu.game.creature_find(id)
boss:set_flags({unstoppable = true, invulnerable = true})
```

### Creature bodies

A body replaces a creature's own parts with model pieces on a rig, posed by the Animator's own code (`tools/animator/rig.cpp` and the clip
evaluator), so a clip at a time gives the Animator's matrices. The game keeps the creature (hp, hits, threat, network, death); only its look
changes. A rig is a `.rig.toml` of the Animator; each rig node with a `model` is a piece, and each piece needs a model slot of the mod
(`Mod::model_slot`). Clips are `.anim.toml` texts. The clock of a body runs with the creature's draw (real time, at most 100 ms a frame).
Nothing is drawn until a clip plays. When the creature's hp reaches 0 the client bursts every piece into the game's lit-cube death particles at
its last matrix, once, and stops drawing the body. `Cube.exe` only for now. The entries below are since 1.4 and need the primitive
`creature_body` (the ones with a creature also `handles`).

### `body_define`

Defines a body from a rig text, one model slot per rig node that has a model, and clips; returns its id.

- **C:** `KyubuResult body_define(KyubuModHandle self, const KyubuBodyDesc* desc, KyubuBody* out, char* why, uint32_t why_cap)`.
- **C++:** `kyubu::Mod::body_define(desc, &body, why, cap)`.
- **Lua:** `kyubu.game.body_define{name =, rig = "<rig text>", pieces = {node = slot, ...}, clips = {name = "<clip text>", ...}}` returns the body
  id, or `nil` and the reason.
- **Side:** both; no world is needed.
- **Errors:** `KY_E_INVALID_ARG` (refused: the reason is in `why`: a rig the Animator's check refuses, a piece with no model slot, named by its node,
  a clip that fails the clip check or writes a pose value that no rig node reads), `KY_E_UNAVAILABLE`; and the guard's ([Result codes](#result-codes)).

### `body_name`

The name of piece number `index` (`kind` 0) or clip number `index` (`kind` 1) of a body, so a caller can list them.

- **C:** `KyubuResult body_name(KyubuModHandle self, KyubuBody body, uint32_t kind, uint32_t index, char* buf, uint32_t cap)`.
- **Lua:** `kyubu.game.body_name(body)` returns `{pieces = {...}, clips = {...}}`, or `nil`.
- **Side:** both.
- **Errors:** `KY_E_NOT_FOUND` (no such body or past the last), `KY_E_BUFFER`, `KY_E_INVALID_ARG`.

### `creature_body_attach`

Attaches a body to a creature with a scale in blocks a voxel; body 0 detaches. The creature's own parts are not drawn; the body ends with the
creature or the world.

- **C:** `KyubuResult creature_body_attach(KyubuModHandle self, KyubuCreature c, KyubuBody body, float scale)`.
- **Lua:** `kyubu.game.creature_body_attach(c, body, scale)`; a `nil` body detaches.
- **Side:** both.
- **Errors:** `KY_E_GONE`, `KY_E_NOT_FOUND` (no such body), `KY_E_INVALID_ARG` (scale not above 0); and the guard's.

### `creature_body_play`

Plays a clip of the body by name from a start time. `KY_BODY_LOOP` loops it, otherwise it holds its last pose; `fade_ms` cross-fades from the
clip before (0 at once).

- **C:** `KyubuResult creature_body_play(KyubuModHandle self, KyubuCreature c, const char* clip, float start_ms, uint32_t flags, float fade_ms)`.
- **Lua:** `kyubu.game.creature_body_play(c, clip, {start = 0, loop = false, fade = 0})`.
- **Side:** both.
- **Errors:** `KY_E_GONE`, `KY_E_NOT_FOUND` (no body attached, or no such clip), `KY_E_INVALID_ARG`; and the guard's.

### `creature_body_offset`

Sets the offset of one pose value, added to the clip's, in a weight group (0 to 7). Part 0 to 5 is root, body, head, hand_r, hand_l, tail; channel
0 is pos, 1 rot. `v` NULL clears every offset.

- **C:** `KyubuResult creature_body_offset(KyubuModHandle self, KyubuCreature c, uint32_t part, uint32_t channel, const float* v, uint32_t group)`.
- **Lua:** `kyubu.game.creature_body_offset(c, "head", "rot", {x, y, z}, group)` (part and channel by name); `kyubu.game.creature_body_offset(c)` clears.
- **Side:** both.
- **Errors:** `KY_E_GONE`, `KY_E_NOT_FOUND` (no body), `KY_E_INVALID_ARG`; and the guard's.

### `creature_body_weight`

Sets the weight (0 or more, normally 0 to 1) of an offset group; every group starts at 1.

- **C:** `KyubuResult creature_body_weight(KyubuModHandle self, KyubuCreature c, uint32_t group, float weight)`.
- **Lua:** `kyubu.game.creature_body_weight(c, group, weight)`.
- **Side:** both.
- **Errors:** `KY_E_GONE`, `KY_E_NOT_FOUND` (no body), `KY_E_INVALID_ARG`; and the guard's.

### `creature_body_read`

Reads the playing clip (index, time, fade left, loop) and the markers passed since the last read.

- **C:** `KyubuResult creature_body_read(KyubuModHandle self, KyubuCreature c, KyubuBodyState* state, KyubuBodyMarker* markers, uint32_t cap, uint32_t* n)`.
- **Lua:** `kyubu.game.creature_body_read(c)` returns `{clip =, time =, fade_left =, loop =, markers = {"name", ...}}`, or `nil` and the reason.
- **Side:** both.
- **Errors:** `KY_E_GONE`, `KY_E_NOT_FOUND` (no body), `KY_E_INVALID_ARG`; and the guard's.

### `creature_body_point`

The world point in blocks (x east, y north, z up) of a point `p` in the voxels of a piece's model, for the playing pose (`clip` NULL) or for a clip
alone at `t_ms`, at the creature's place and yaw (`body_yaw`).

- **C:** `KyubuResult creature_body_point(KyubuModHandle self, KyubuCreature c, const char* piece, const float* p, const char* clip, float t_ms, float* out)`.
- **Lua:** `kyubu.game.creature_body_point(c, piece, {x, y, z}, clip, t_ms)` returns `x, y, z`, or `nil` and the reason.
- **Side:** both, so the server can place an attack on the rig.
- **Errors:** `KY_E_GONE`, `KY_E_NOT_FOUND` (no body, no such piece or clip), `KY_E_INVALID_ARG`; and the guard's.

### `creature_give_item`

Puts an item into a creature's bags (its inventory), the way loot does: with the game's own insert.

- **C:** `KyubuResult creature_give_item(KyubuModHandle self, KyubuCreature c, const KyubuItem* item)`. Set
  `item->size` first. `KyubuItem` has `type` (1 to `KY_ITEM_TYPE_MAX`, 3 is a weapon), `sub_type`, `rarity` (0 to 4),
  `material`, `modifier` (the seed of the stat roll) and `level` (1 to 32767).
- **C++:** `KyubuResult kyubu::Mod::creature_give_item(KyubuCreature c, KyubuItem item)`: sets `item.size` itself.
- **Lua:** `kyubu.game.creature_give_item(c, {type =, sub_type =, level =, rarity =, material =, modifier =})`, or
  `c:give_item{...}`, returns `true`, or `nil` and the reason. Only `type` is required; the other fields default to 0,
  and `level` to 1. A field of the wrong type or range raises.
- **Primitive:** `items` (and `entity_state`'s `handles`).
- **Side:** client only, for the local player's bags, in single-player and on a client joined to a server (the bags are the client's own: the server never sees them). Cube.exe only for now: on `Server.exe` the primitive is
  unavailable ("not mapped for Server.exe yet").
- **What the game does:**
  - `level` is the item's stored level; the game shows its power (stored 21 is power 51, stored 1981 is power 100).
  - Coins (`type` 12) go to the coin counter, not a slot: `material` 10 is copper, 12 silver, 11 gold, and `level` is the
    amount.
  - The other stackable types (`type` 0x0B with sub_type 0x0E, 0x14, 0x15, 0x17, 0x18 and 0x19) take `level` as
    the quantity, and the game stores level 1.
  - Everything else goes into the first empty slot of the bag the type belongs to, and a bag with no empty slot grows.
  - An inventory window that is already open shows the item when it is opened again.
- **Errors:** `KY_E_INVALID_ARG` (`item` NULL, `size` too small, `type`, `rarity` or `level` out of range, or coins
  with another material; the log names the field and nothing is added), `KY_E_GONE`, `KY_E_SIDE` (a creature the
  server side owns); and the guard's ([Result codes](#result-codes)).
- **Since:** 1.1.

```lua
local player = kyubu.game.creature_player()
player:give_item{ type = 3, sub_type = 17, level = 21, rarity = 2 }    -- a weapon (stored level 21: power 51)
player:give_item{ type = 12, material = 11, level = 500 }              -- 500 gold coins
```

### `creature_find`

The creature with a game id, as a handle on the calling side. The server side has no handle of the player (only the
client side has, [`creature_player`](#creature_player)), so a client script sends `creature_get`'s `id` over a channel
and the server side finds the creature with it: the `owner` of a companion, for one.

- **C:** `KyubuResult creature_find(KyubuModHandle self, int64_t id, KyubuCreature* out)`: the handle in `out` (0
  when the call fails).
- **C++:** `KyubuResult kyubu::Mod::creature_find(int64_t id, KyubuCreature& out)`.
- **Lua:** `kyubu.game.creature_find(id)` returns the creature object, or `nil` and the reason.
- **Primitive:** `entity_state` (and `handles`).
- **Side:** both. The handle belongs to the calling side, and a creature the other side owns (the player, seen from
  the server side) can be read and named as an `owner`, not changed.
- **Errors:** `KY_E_NOT_FOUND` (this side's World has no creature with that id; one spawned during the current World
  step enters the map after it), `KY_E_INVALID_ARG` (`out` is NULL); and the guard's ([Result codes](#result-codes)).
- **Since:** 1.1.

```lua
-- client side: send the player's id; server side: find the player and spawn a companion for it
local id = kyubu.game.creature_player():get().id
kyubu.channel.send("my_mod:pet", kyubu.channel.SERVER, { owner = id })
-- server side
local owner = kyubu.game.creature_find(msg.owner)
```

### `creature_spawn`

Builds a creature the way the game builds its own run-time creatures (the recipe in
[`primitives.md`](../mapping/atlas/primitives.md), Spawn): the id is the game's rule (the lowest id, but not above 0,
minus 1), then the game's allocator, the creature's constructor, the fields, `hp` set to the game's max hp, the type's
look (a random variation), and the game's creature-map insert. The game treats it like its own: it removes the
creature when its hp reaches 0 or when it is left behind, and its handle then reports `KY_E_GONE`. By default it has
no behaviour and stands still; the other kinds give it one of the game's own behaviours (recipe step 8), a sequence
of the classes the game builds for that kind, each built with the game's constructor:

| Kind | The sequence | What the creature does |
|---|---|---|
| `KY_SPAWN_STILL` (0) | none | stands still |
| `KY_SPAWN_MONSTER` (1) | combat, look at player, random walk | the zone default of a monster: it fights (its hostile type decides whom: a hostile creature comes to the player and hits it), turns towards the player within about 9 blocks, and wanders |
| `KY_SPAWN_FRIENDLY` (2) | combat, look at player, random walk | turns towards the player, wanders, and fights the creatures that are hostile to it when they come close (the hostile type decides who those are: a creature of the player's side, type 2, 3 or 5, fights the hostile ones, type 1, and no one else) |
| `KY_SPAWN_COMPANION` (3) | combat, companion | follows `owner` (2 to 3 blocks, over any distance) and fights what its owner fights, as a pet or a summon does; needs `owner`, whose id the creature also keeps (`parent_owner`), as the game's pet and summon store it. Type 3 is a summon (a name and a talk prompt), type 2 a nameless creature of the player's side. Do not use type 5 (the game's pet) for a spawn: the game kills a type-5 creature that is not the one pet its owner has registered (the owner's creature holds the pet's id at +0x11C8, and the check is at 0x60D441), so a spawned type-5 creature disappears within a few seconds |
| `KY_SPAWN_GUARD` (4) | combat, walk path | fights and walks the waypoints of `path` in turn; needs `path` |
| `KY_SPAWN_DEFAULT` (5) | as the game would | what the game gives a creature nobody chose a behaviour for: the zone default (the monster's) and, for a target (hostile type 6), nothing |

The game has no rule that gives a behaviour by entity type: its region generators choose one per spawn, and the zone
default covers a spawn without one, so `KY_SPAWN_DEFAULT` follows the hostility only. The game's other per-kind
exception (no walk for some classes) is not applied. The behaviour's objects belong to the creature: the game frees
only the sequence's own block when the creature goes, so the children (and a path's waypoints) stay allocated, a small
leak per creature that the game has with its own.

A creature that wanders (friendly, monster, default) has no leash, because `SpawnLocationBehavior` is not wired: after
a few minutes it may walk away in a straight line, and the game removes it when it is far from the player (probe
`entity-spawner-behaviors`: two friendly slimes walked to about 215 blocks and were gone). A hostile creature fights
the player only when its hostile type says so (type 1); the same behaviour on a friendly one (type 2) stands next to the
player and does no harm.

- **C:** `KyubuResult creature_spawn(KyubuModHandle self, const KyubuSpawn* spawn, KyubuCreature* out)`: the new
  creature's handle in `out` (0 when the call fails). Set `spawn->size` to `sizeof(KyubuSpawn)`.

  | Field | Type | Unit |
  |---|---|---|
  | `entity_type` | `uint32_t` | a `cube::EntityType` value: exactly the values [`game_enum`](#game_enum) lists |
  | `hostile_type` | `uint8_t` | a `cube::HostileType` value other than 0 (a player): 1 hostile, 2 friendly, 3 named friendly (it has a name and a talk prompt), 5 pet (accepted, but the game removes a spawned one: see `KY_SPAWN_COMPANION`), 6 target |
  | `behavior` | `uint8_t` | `KY_SPAWN_STILL` (0), `KY_SPAWN_MONSTER` (1), `KY_SPAWN_FRIENDLY` (2), `KY_SPAWN_COMPANION` (3), `KY_SPAWN_GUARD` (4) or `KY_SPAWN_DEFAULT` (5), as above |
  | `reserved` | `uint16_t` | 0 |
  | `level` | `int32_t` | 1 or more |
  | `pos[3]` | `int64_t` | world units: 65,536 per block; x east, y north, z up |
  | `body_yaw` | `float` | facing, degrees counter-clockwise from north (+y) |
  | `path_count` | `uint32_t` | `KY_SPAWN_GUARD`: the number of waypoints in `path`, 1 to `KY_SPAWN_PATH_MAX` (64); ignored for the other kinds |
  | `owner` | `KyubuCreature` | `KY_SPAWN_COMPANION`: a handle, on the spawning (server) side, of the creature to follow: one it spawned, or any creature [`creature_find`](#creature_find) gives it (the player); the behaviour keeps its game id. Ignored for the other kinds |
  | `path` | `const int64_t (*)[3]` | `KY_SPAWN_GUARD`: `path_count` waypoints in world units (x east, y north, z up); ignored for the other kinds |

- **C++:** `KyubuResult kyubu::Mod::creature_spawn(KyubuSpawn spawn, KyubuCreature& out)`: sets `spawn.size` itself.
- **When:** outside a World step the creature is in the creature map when the call returns. Inside a step (code the
  step calls, such as a hook) it is built at once and enters the map after the step; its handle works in between,
  and two spawns in one step get different ids.
- **Lua:** `kyubu.game.creature_spawn{ type = 37, hostile = 1, level = 5, pos = {x, y, z}, yaw = 90 }` returns the
  creature object, or `nil` and the reason. `pos` is `{x, y, z}` or `{x =, y =, z =}` (as `creature_get` gives it; a
  fraction of a unit is dropped); `yaw` is 0 when left out; `behavior`, when given, is `"still"` (the default),
  `"monster"`, `"friendly"`, `"companion"`, `"guard"` or `"default"`; `owner` is a creature object (a `"companion"`
  follows it) and `path` a list of points `{x, y, z}` or `{x =, y =, z =}` (a `"guard"` walks it). A field of the wrong
  type raises.
- **Primitive:** `spawn` (and `entity_state`, `handles`; `behavior` too for any behaviour, `behavior_companion` for
  `KY_SPAWN_COMPANION` and `behavior_path` for `KY_SPAWN_GUARD`).
- **Side:** server only (`KY_E_SIDE` from the client side: a new creature belongs to the server side, and its handle
  is a server-side handle). A client-side script sends a channel message to its server side, and the server sends
  back the game id, not the handle.
- **Errors:** `KY_E_INVALID_ARG` (`spawn` or `out` NULL, or a field out of range: the reason names it, for example
  `entity_type is 999, not a value of cube::EntityType`; a companion without an `owner` fails with `owner required`
  and a guard without `path` with `path required`; nothing is built), `KY_E_GONE` (the `owner` no longer exists),
  `KY_E_UNAVAILABLE` (on `Server.exe`: `spawn is unavailable: not mapped for Server.exe yet`; the same for a kind
  whose primitive is, for example `behavior_companion is unavailable: ...`), and the guard's
  ([Result codes](#result-codes)).
- **Since:** 1.1.

```lua
-- server side: a green slime two blocks north of a position
local slime, why = kyubu.game.creature_spawn{ type = 37, hostile = 1, level = 5,
                                              pos = {x = x, y = y + 2 * 65536, z = z}, yaw = 180 }
if not slime then kyubu.warn("no slime: " .. why) return end
kyubu.info("spawned " .. tostring(slime))
```

```cpp
KyubuSpawn s{};   // server side: the same slime from C++
s.entity_type = 37;
s.hostile_type = 1;
s.level = 5;
s.pos[0] = x, s.pos[1] = y + 2 * 65536, s.pos[2] = z;
s.body_yaw = 180;
KyubuCreature slime = 0;
if (KyubuResult r = mod.creature_spawn(s, slime); r != KY_OK) mod.warn("no slime: result %d", r);
```

### `ability_get`

Reads the ability a creature is in: its number, its registry id (vanilla or a mod's) and the milliseconds since it started.

- **C:** `KyubuResult ability_get(KyubuModHandle self, KyubuCreature c, KyubuAbilityInfo* out)`. Set `out->size` first.
  `KyubuAbilityInfo` has `number` (0 when the creature is in no ability), `elapsed_ms` (the creature's mode time) and
  `id` (a string of up to `KY_ABILITY_ID_MAX - 1` characters; empty for number 0 or for a number the registry has no
  name for).
- **Lua:** `kyubu.game.ability_get(c)`, or `c:ability()`, returns `{number =, id =, elapsed_ms =}`, or `nil` and the
  reason. `number == 0` means "in no ability".
- **Primitive:** `ability_state` (and `handles`).
- **Side:** both, for any creature the calling side has a handle of.
- **Errors:** `KY_E_INVALID_ARG` (`out` NULL, or `out->size` below 8), `KY_E_GONE`, `KY_E_SIDE` (a handle of the other
  side); and the guard's ([Result codes](#result-codes)).
- **Since:** 1.1.

```lua
local player = kyubu.game.creature_player()
local a = player:ability()
if a.number ~= 0 then kyubu.info(("%s for %d ms"):format(a.id, a.elapsed_ms)) end   -- "cubeworld:smash for 120 ms"
```

### `ability_cooldown`

The milliseconds of cooldown a creature has left for an ability: vanilla or a mod's, from the creature's own cooldown
map. A cooldown starts at the ability's first World step, so it reads 0 until then.

- **C:** `KyubuResult ability_cooldown(KyubuModHandle self, KyubuCreature c, uint32_t number, uint32_t* out_ms)`:
  `out_ms` is 0 when the creature has no cooldown for `number` (and when the call fails).
- **Lua:** `kyubu.game.ability_cooldown(c, ability)`, or `c:cooldown(ability)`, returns the milliseconds, or `nil` and
  the reason. `ability` is the number (0 to 255) or the id (`"demo:burst"`).
- **Primitive:** `ability_state` (and `handles`).
- **Side:** both, for any creature the calling side has a handle of.
- **Errors:** `KY_E_INVALID_ARG` (`out_ms` NULL, or `number` above 255), `KY_E_GONE`, `KY_E_SIDE`; and the guard's
  ([Result codes](#result-codes)).
- **Since:** 1.1.

### `ability_cooldown_set`

Sets the milliseconds of cooldown a creature has left for an ability; 0 gives the cooldown back. The creature's own
cooldown map is written when it has an entry for the number: the game makes one at the ability's first World step, and
this call makes none. On `Server.exe` it also sets when the server admits a remote player's start of that ability again
([abilities.md](abilities.md), "The server refuses what the player cannot afford"): an ability that gives its cooldown back
on the client (a fizzled charge) does the same in its server module, so the player's next start is not refused.

- **C:** `KyubuResult ability_cooldown_set(KyubuModHandle self, KyubuCreature c, uint32_t number, uint32_t ms)`.
- **Lua:** `kyubu.game.ability_cooldown_set(c, ability, ms)`, or `c:set_cooldown(ability, ms)`, returns `true`, or
  `nil` and the reason. `ability` is the number (1 to 255) or the id; `ms` a whole number from 0 to 3,600,000.
- **Primitive:** `ability_state` (and `handles`).
- **Side:** both, for a creature the calling side owns (the client side: the local player; on `Server.exe`, every
  creature).
- **Errors:** `KY_E_INVALID_ARG` (`number` 0 or above 255, `ms` above 3,600,000), `KY_E_GONE`, `KY_E_SIDE`; and the
  guard's ([Result codes](#result-codes)).
- **Since:** add-skills-unleashed-multiplayer 8.1 (appended last to `KyubuApi`).

```lua
-- Server.exe: a run that cast nothing gives its cooldown back, as the client did
on_end = function(ctx) if not ctx.state.cast then kyubu.game.ability_cooldown_set(ctx.creature, ctx.number, 0) end end
```

```lua
local left = kyubu.game.creature_player():cooldown("cubeworld:smash")
kyubu.info(("smash: %d ms left"):format(left))
```

### `ability_start`

Starts a **mod** ability on a creature, with the checks of a press of key 4 except the key. The step runner then runs
it: `on_start`, `on_step`, `on_end`, its mp taken once, its cooldown started by the game.

- **C:** `KyubuResult ability_start(KyubuModHandle self, KyubuCreature c, uint32_t number)`.
- **Lua:** `kyubu.game.ability_start(c, ability)` returns `true`, or `nil` and the reason (a refusal says why: `on
  cooldown: 3377 ms left`, `not enough mp`, `busy`). `ability` is the number or the id.
- **Primitive:** `ability_use` (the game's `modeInterruptible` and `canUseAbility`), with `ability_state` and the
  fourth slot's five primitives.
- **Side:** both, for a creature the calling side owns (on the client side, the local player; on the server side, the
  monsters of a single-player World), and only a creature the step runner serves (the local player, and monsters in
  single-player). On `Server.exe` the step runner exists (`ability_step`, `ability_stats`) but `ability_use` does not:
  there the call skips the `modeInterruptible` and `canUseAbility` checks and refuses only a creature already in a mode.
  A remote player's own start is not made by this call: the server admits it by itself (rank, cooldown, mp), see
  [abilities.md](abilities.md), "Limits".
- **What it checks, in this order:** the number is a defined mod ability; the creature is owned and served; its cooldown
  for the ability is 0; its mp is at least the ability's cost; the game's `modeInterruptible` allows a new ability;
  the game's `canUseAbility` allows it. Then it writes the ability's number to the creature's mode and 0 to its mode
  time, as the game's input does. It takes no mp and writes no cooldown: the first World step does.
- **Errors:** `KY_E_INVALID_ARG` (a number that is not a defined mod ability, a vanilla one too: the creature is
  unchanged), `KY_E_REFUSED` (the game state refuses: on cooldown, not enough mp, busy, or a creature the step runner
  does not serve; the reason is in the log and in Lua's second result), `KY_E_UNAVAILABLE` (the fourth slot or
  `ability_use` is not available on this build), `KY_E_GONE`, `KY_E_SIDE` (a creature the other side owns); and the
  guard's ([Result codes](#result-codes)).
- **Since:** 1.1.

```lua
local player = kyubu.game.creature_player()
local ok, why = kyubu.game.ability_start(player, "demo:burst")
if not ok then kyubu.info("burst refused: " .. why) end
```

### `ability_end`

Ends the creature's current mod ability. At the next safe point after the World step the mode is set to 0 and `on_end`
runs with the reason `cancelled`.

- **C:** `KyubuResult ability_end(KyubuModHandle self, KyubuCreature c)`.
- **Lua:** `kyubu.game.ability_end(c)` returns `true`, or `nil` and the reason.
- **Primitive:** `ability_state` (and `handles`), with the fourth slot's five primitives.
- **Side:** both, for a creature the calling side owns, as for [`ability_start`](#ability_start).
- **Errors:** `KY_E_NOT_FOUND` (the creature is in no mod ability, or only in a watched one: [abilities.md](abilities.md#watching-another-player)), `KY_E_UNAVAILABLE` (the fourth slot is not
  available), `KY_E_GONE`, `KY_E_SIDE`; and the guard's ([Result codes](#result-codes)). An ability started by
  `ability_start` that has not had its first World step is taken back at once, and no callback runs.
- **Since:** 1.1.

```lua
kyubu.game.ability_end(kyubu.game.creature_player())
```

### `ability_rank_get`

Reads the local player's rank in a mod ability: the number kept in `Save\characters.kyubu-abilities.json` next to the
game's character save, keyed by the character's slot and checked against its name. A character with no entry has rank 1.

- **C:** `KyubuResult ability_rank_get(KyubuModHandle self, uint32_t number, uint32_t* out_rank)`.
- **Lua:** `kyubu.game.ability_rank_get(ability)` returns the rank, or `nil` and the reason.
- **Primitive:** `ability_ranks`.
- **Side:** client only (`KY_E_SIDE` from the server side): the ranks are the local player's.
- **Errors:** `KY_E_INVALID_ARG` (not a defined mod ability: a vanilla one has no rank; or `out_rank` is `NULL`),
  `KY_E_UNAVAILABLE` (the ranks are not mapped for this build: the reason names the blocking entry, and every rank reads
  1), `KY_E_REFUSED` (no character is selected yet); and the guard's ([Result codes](#result-codes)).
- **Since:** 1.1.

```lua
local rank = kyubu.game.ability_rank_get("demo:burst")
```

### `ability_rank_set`

Sets that rank. The value is clamped to 0 up to the ability's `max_rank`, written to the ranks file at once, and slot "4"
follows within one frame: rank 0 empties the slot. The console does the same: `ability rank <id> [rank]`.

- **C:** `KyubuResult ability_rank_set(KyubuModHandle self, uint32_t number, uint32_t rank, uint32_t* out_rank)`;
  `out_rank` may be `NULL` and gets the rank stored.
- **Lua:** `kyubu.game.ability_rank_set(ability, rank)` returns the rank stored, or `nil` and the reason.
- **Primitive:** `ability_ranks`.
- **Side:** client only.
- **Errors:** as for [`ability_rank_get`](#ability_rank_get); `KY_E_REFUSED` also when the file could not be written (the
  rank then holds until the game exits).
- **Since:** 1.1.

```lua
kyubu.game.ability_rank_set("demo:burst", 3)
```

### `animation_define_from_text`

A call for dev mods (`add-ability-animation`; the standalone Animator does not call it): validates the text of a clip as
the loader validates an `.anim.toml` file and publishes it as the clip `<caller's id>:<name>` in a new registry snapshot,
with no file written. A clip of that id that a file gives is replaced for as long as the game runs: a scan of the files
(the dev poll, `anim reload`) does not read that file again. The next draw can use the clip, and `anim list` shows it.

- **C:** `KyubuResult animation_define_from_text(KyubuModHandle self, const char* name, const char* text, char* err, uint32_t err_cap)`;
  `name` is 1 to 64 characters of `a-z`, `0-9`, `_`, `-` and `.`. `err` may be `NULL`; a bad clip leaves its errors there,
  one per line, `<source>:<line>: <path>: <message>`, cut to `err_cap`.
- **C++:** `kyubu::Mod::animation_define_from_text(name, text, err, err_cap)`.
- **Lua:** `kyubu.game.animation_define_from_text(name, text)` returns `true`, or `nil` and the errors.
- **Primitive:** none: it holds no game memory.
- **Side:** client only.
- **Errors:** `KY_E_TRUST` (not full trust), `KY_E_DEV_ONLY` (not dev mode), `KY_E_SIDE` (the server side),
  `KY_E_INVALID_ARG` (a bad name, a bad text or a NULL pointer: the registry is unchanged).
- **Since:** 1.1.

### `animation_preview`

The preview clock: the local player is drawn in the pose of a clip at a time, whatever its real mode, until
`animation_preview_clear`. No ability starts, so no mp, cooldown or cast happens. A second call moves the clock. The pose provider
that reads the clock is task 4.2's; until it exists the call only sets the state.

- **C:** `KyubuResult animation_preview(KyubuModHandle self, const char* clip_id, double t_ms)`; `t_ms` is 0 or more.
- **C++:** `kyubu::Mod::animation_preview(clip_id, t_ms)`.
- **Lua:** `kyubu.game.animation_preview(clip, t_ms)` returns `true`, or `nil` and the reason.
- **Primitive:** none.
- **Side:** client only.
- **Errors:** as for `animation_define_from_text`, and `KY_E_NOT_FOUND` (no such clip), `KY_E_INVALID_ARG` (a NULL id, or a
  `t_ms` below 0 or not finite).
- **Since:** 1.1.

### `animation_preview_clear`

Turns the preview clock off: the local player is drawn in the game's pose again. `KY_OK` when it was off.

- **C:** `KyubuResult animation_preview_clear(KyubuModHandle self)`.
- **C++:** `kyubu::Mod::animation_preview_clear()`.
- **Lua:** `kyubu.game.animation_preview_clear()` returns `true`.
- **Primitive:** none.
- **Side:** client only.
- **Errors:** as for `animation_define_from_text` without the argument errors.
- **Since:** 1.1.

```lua
kyubu.game.animation_define_from_text("slam", io.open("slam.anim.toml"):read("*a"))
kyubu.game.animation_preview("demo:slam", 240)
kyubu.game.animation_preview_clear()
```

### `ability_animation_bind`

Binds an animation clip to an ability: the ability is drawn with the clip while it runs (the clip format is in [animations.md](animations.md)). A clip is data, so every trust level may
call it for its own ability. The ability must exist: define it first with `ability_define`. One clip per ability: binding again
replaces the first. A binding works in any phase after the ability exists, so a dev tool can rebind while the game runs.
`kyubu.ability.define` does the same through its `anim` key, after the definition: a clip that is not there fails the call with
`nil` and the reason, and the ability stays defined, without a clip. A server side only records the binding, so that the registry
can give the clip's duration and markers.

- **C:** `KyubuResult ability_animation_bind(KyubuModHandle self, const char* ability_id, const char* clip_id)`.
- **C++:** `kyubu::Mod::ability_animation_bind(ability_id, clip_id)`.
- **Lua:** `kyubu.animation.bind(ability_id, clip_id)` returns `true`, or `nil` and the reason (any trust level; full-trust mods also have it as `kyubu.game.ability_animation_bind`); `anim = "<mod>:<clip>"` in `kyubu.ability.define`.
- **Primitive:** `animation_pose`, on the client side only (the call needs no World, so it works in the Register phase).
- **Side:** both.
- **Errors:** `KY_E_NAMESPACE` (another mod's ability), `KY_E_INVALID_ARG` (a vanilla ability, `kyubu:placeholder`, or a NULL
  argument), `KY_E_NOT_FOUND` (no such ability, or no such clip: the reason names the id), `KY_E_UNAVAILABLE` (client side: the
  pose provider's entries are not verified for this build; the reason is `game_available("animation_pose")`'s). Nothing changes on an error.
- **Since:** 1.1.

### `ability_animation_bind_for`

Binds an animation clip to an ability for one creature, named by its game id (`creature:get().id`): while that creature runs the
ability it is drawn with this clip, and its steps' markers come from it; every other creature keeps the ability's own clip
(`ability_animation_bind`). A Fire Mage's release clip after its charge is one: with the ability's clip rebound instead, every
other Fire Mage in sight would switch clips too. A NULL (Lua `nil`) clip drops the creature's binding. A binding lasts until it is
dropped, the creature is destroyed or the world is left. The checks are `ability_animation_bind`'s.

- **C:** `KyubuResult ability_animation_bind_for(KyubuModHandle self, const char* ability_id, uint64_t creature_id, const char* clip_id)`.
- **C++:** `kyubu::Mod::ability_animation_bind_for(ability_id, creature_id, clip_id)`.
- **Lua:** `kyubu.animation.bind_for(ability_id, creature_id, clip_id)` returns `true`, or `nil` and the reason (any trust level; full-trust mods also have it as `kyubu.game.ability_animation_bind_for`). A creature id that is not a whole number from 1 is a Lua error.
- **Primitive:** `animation_pose`, on the client side only, as for `ability_animation_bind`.
- **Side:** both; any thread.
- **Errors:** those of `ability_animation_bind`, and `KY_E_INVALID_ARG` for creature id 0. Nothing changes on an error.
- **Since:** 1.4 (appended after `creature_set_yaw`).

```lua
kyubu.ability.define("demo:slam", { duration_ms = 700, cooldown_ms = 6000, anim = "demo:slam" })
kyubu.animation.bind("demo:slam", "demo:slam_wide")   -- replaces it
```

### `ui_node_find`

Finds a node of the UI's scene tree by name. The tree is plasma's: one root node (named `root`) whose children are the HUD, the
menu panels, the start scene with the title logo and the other screens, all present from the first frame
([mapping/atlas/ui.md](../mapping/atlas/ui.md), "The title screen and the scene tree" lists what is in it). The walk is
depth-first in the order the nodes are drawn in, and names repeat (each of the logo's four letter layers has a `C`), so `nth`
counts the matches: 0 is the first.

```c
KyubuUiNode n = {0};
n.size = sizeof n;
if (api->ui_node_find(self, "cubeworld", 0, &n) == KY_OK) {
    /* n.node is the logo's address, n.children is 7, n.colour is {1, 1, 1, 1} */
}
```

- **C:** `KyubuResult ui_node_find(KyubuModHandle self, const char* name, uint32_t nth, KyubuUiNode* out)`. `out->size` is set by
  the caller; `KyubuUiNode` has `children`, `depth`, `node`, `parent` (addresses of `plasma::Node` objects, 0 for the root's
  parent), `colour` (RGBA of the node's display colour row, `1, 1, 1, 1` when it has none) and `name` (UTF-8, at most 63 bytes).
- **C++:** `kyubu::Mod::ui_node_find(name, nth, &out)`.
- **Lua:** `kyubu.game.ui_node_find(name, nth)` returns `{node, parent, name, children, depth, colour}`, or `nil` and the
  reason (full trust, as all of `kyubu.game`).
- **Primitive:** `ui`.
- **Side:** client only (Cube.exe). Main thread only: the call that draws and changes the tree; use `run_on_main_thread` from
  another thread. Any phase after the game controller has its engine (the first frame).
- **Errors:** `KY_E_NOT_FOUND` (fewer than `nth + 1` nodes have the name), `KY_E_PHASE` (the engine does not exist yet),
  `KY_E_INVALID_ARG` (a NULL `name`, a NULL `out` or an `out->size` below 8), `KY_E_UNSAFE` (not the main thread),
  `KY_E_SIDE` (the server side), `KY_E_UNAVAILABLE` (the `ui` primitive's entries are not verified for this build).
- **Since:** 1.1.
- **Limits:** the walk visits at most 20,000 nodes and goes 40 levels down; a node's first 256 children are walked.

### `ui_node_get`

Describes the node at an address: one a `KyubuUiNode` gave, or the `node` a hooked `plasma::D3D9Drawing::draw` call was given,
which is how a draw hook asks "which node is this?". The address is not trusted: it is read only through guarded reads and is
accepted only when the tree holds it (its parent links lead to the root and each node on the way is in its parent's child
list), so the address of a node that was freed is `KY_E_GONE`. The check costs a few reads, not a walk. Address 0 is the root.

- **C:** `KyubuResult ui_node_get(KyubuModHandle self, uintptr_t node, KyubuUiNode* out)`.
- **C++:** `kyubu::Mod::ui_node_get(node, &out)`.
- **Lua:** `kyubu.game.ui_node_get(node)` (nil is the root) returns the node table, or `nil` and the reason.
- **Primitive:** `ui`.
- **Side:** client only; main thread only.
- **Errors:** `KY_E_GONE` (the tree does not hold that address), `KY_E_PHASE`, `KY_E_INVALID_ARG`, `KY_E_UNSAFE`, `KY_E_SIDE`,
  `KY_E_UNAVAILABLE`, as for `ui_node_find`.
- **Since:** 1.1.

### `ui_node_child`

The `index`th child of a node, in drawing order, to walk a subtree without knowing its names (the logo's letters, a panel's
buttons). The node is located as `ui_node_get` locates it.

- **C:** `KyubuResult ui_node_child(KyubuModHandle self, uintptr_t node, uint32_t index, KyubuUiNode* out)`.
- **C++:** `kyubu::Mod::ui_node_child(node, index, &out)`.
- **Lua:** `kyubu.game.ui_node_child(node, index)` returns the node table, or `nil` and the reason.
- **Primitive:** `ui`.
- **Side:** client only; main thread only.
- **Errors:** `KY_E_NOT_FOUND` (`index` is past the last child, or past the first 256), and those of `ui_node_get`.
- **Since:** 1.1.

```lua
-- the letters of the logo's first layer, in order
local logo = kyubu.game.ui_node_find("cubeworld")
local layer = kyubu.game.ui_node_child(logo.node, 0)
for i = 0, layer.children - 1 do
  print(kyubu.game.ui_node_child(layer.node, i).name)   -- C U B E W O L R D
end
```

### `render_get`

The game's Direct3D 9 device and the size of what the engine draws into, for a mod that is not inside a `render.*` event: a
hooked `plasma::D3D9Drawing::draw` call, or a task on the main thread. The device is the game's own and is not reference
counted: use it and do not `Release` it. A draw hook should read `plasma::D3D9Engine.world_view` through the `cubeworld/` headers
for the transform of the shape it is given, and draw its own triangles after the last node of the screen, not inside the
game's draw call (the atlas page [rendering](../mapping/atlas/rendering.md), "How a plasma shape is drawn", says why).

- **C:** `KyubuResult render_get(KyubuModHandle self, KyubuRenderData* out)`; `out->device` is an `IDirect3DDevice9*` and
  `out->width` and `out->height` the client size in pixels (the window's, not `options.cfg`'s).
- **C++:** `kyubu::Mod::render_get(&out)`.
- **Lua:** `kyubu.game.render_get()` returns `{device = address, width =, height =}`, or `nil` and the reason.
- **Primitive:** `ui`.
- **Side:** client only; main thread only.
- **Errors:** `KY_E_PHASE` (no device yet), `KY_E_INVALID_ARG` (a NULL `out`), `KY_E_UNSAFE`, `KY_E_SIDE`, `KY_E_UNAVAILABLE`.
- **Since:** 1.1.

### `particle_spawn`

Starts one of the game's own particle effects: `count` small cubes that fly out of a position and fall, such as the spray of a hit
or a bomb. It is what the game's hits and spells use, so the effect looks like theirs.

- **C:** `KyubuResult particle_spawn(KyubuModHandle self, const KyubuParticle* p)`. Set `p->size` first. `KyubuParticle` has `type`
  (`KY_PARTICLE_SOLID` 0, `KY_PARTICLE_BOMB` 1, `KY_PARTICLE_NO_SPREAD` 2, `KY_PARTICLE_NO_ACCELERATION` 3 and
  `KY_PARTICLE_NO_GRAVITY` 4: cuwo's names, of which the game's own sites use 1 and 2), `pos` (world units, 65,536 to a block),
  `accel` (the game's hit effect has 0, 0, 10), `color` (RGBA, 0 to 1), `scale` (above 0 to 16; the sites use 0.1 to 0.5), `count`
  (1 to `KY_PARTICLE_COUNT_MAX`, 1000; the sites make 3 to 20) and `spreading` (0 to 64; the sites use 3 and 8).
- **C++:** `KyubuResult kyubu::Mod::particle_spawn(KyubuParticle p)`: sets `p.size` itself.
- **Lua:** `kyubu.game.particle_spawn{pos = {x, y, z}, type =, count =, scale =, spreading =, accel = {x, y, z}, color = {r, g, b, a}}`
  returns `true`, or `nil` and the reason. Only `pos` is required; the defaults are the bomb type, 8 particles, scale 0.25, spreading 3,
  acceleration 0, 0, 10 and white. A field of the wrong type raises. Full trust only, as the rest of `kyubu.game`.
- **Primitive:** `particles`.
- **Side:** client only, in single-player and on a client joined to a server. Cube.exe only for now: on `Server.exe` the primitive is
  unavailable ("not mapped for Server.exe yet"). The effect is local: it is put on the client's own World step output, which the
  controller plays, and it is not sent to a server or to other players.
- **What the game does:**
  - The call queues the effect; the loader pushes it on the output of the next World step, after the game's own particles of that step
    (at most about 20 ms later).
  - The effect plays like the game's: the game decides the look of each type.
  - At most 512 effects wait for a step.
- **Errors:** `KY_E_INVALID_ARG` (`p` NULL, `size` too small, a number that is not finite, `type`, `count`, `scale`, `spreading` or a
  colour out of its range; the log names the field and nothing is queued), `KY_E_REFUSED` (512 are waiting), `KY_E_SIDE` (called from
  the server side); and the guard's ([Result codes](#result-codes)).
- **Since:** 1.1.

### `light_spawn`

Starts a point light: it lights the models, the particles and the ground round a position, as the game's lamps, projectiles and spell
flashes do. The Fire Mage's circle (SkillsUnleashed) is lit this way.

- **C:** `KyubuResult light_spawn(KyubuModHandle self, const KyubuLight* l, uint32_t* out_id)`. Set `l->size` first. `KyubuLight` has
  `flags` (0), `pos` (world units, 65,536 to a block), `radius` (blocks, above 0 to `KY_LIGHT_RADIUS_MAX`, 64; a lamp of rarity 4
  reaches 42), `color` (red, green, blue, 0 to `KY_LIGHT_COLOR_MAX`, 16: the strength is in it; the game's lamps give 0.45 to 1.05),
  `life_ms` (0: until it is removed) and `fade_ms` (the last part of the life over which it dims to nothing; at most `life_ms`).
  `*out_id` gets the light's id, 1 or more.
- **C++:** `KyubuResult kyubu::Mod::light_spawn(KyubuLight l, uint32_t* id)`: sets `l.size` itself.
- **Lua:** `kyubu.game.light_spawn{pos = {x, y, z}, radius =, color = {r, g, b}, life_ms =, fade_ms =}` returns the id, or `nil` and
  the reason. `pos` (world units) and `radius` are required; `color` defaults to a warm white (1, 0.8, 0.6), `life_ms` and `fade_ms`
  to 0. A field of the wrong type raises. Full trust only, as the rest of `kyubu.game`.
- **Primitive:** `lights`.
- **Side:** client only. Cube.exe only: Server.exe draws nothing. The light is local: it is drawn by the client that made it and sent
  to no one.
- **What the game does:**
  - Every frame the render fills a vector of `cube::Light` (one per lit lamp, projectile, spell flash and lit world object), sorts it
    by the squared distance to the camera, hands each light to the terrain regions its radius reaches (16 a region) and gives the
    first 16 to the models (the vertex shader uses 10). The loader pushes the lights of mods just before the sort, with a sort key of
    0, so they come first in both.
  - The lighting is per vertex. Daylight hides it: a light shows at night and in dark places.
  - At most `KY_LIGHT_MAX` (16) lights of mods are alive at once, all mods together.
  - A light ends when its life is over, when `light_remove` ends it, when the world is left, when its mod faults or is disabled, and
    when its mod's Lua scripts are reloaded.
- **Errors:** `KY_E_INVALID_ARG` (`l` or `out_id` NULL, `size` too small, `flags` not 0, a number that is not finite, `radius`, a
  colour or `fade_ms` out of its range; the log names the field), `KY_E_REFUSED` (16 are alive), `KY_E_SIDE` (called from the server
  side); and the guard's ([Result codes](#result-codes)).
- **Since:** 1.1.

### `light_set`

Gives a light of the mod new fields: call it every frame to make the light follow something, or to pulse or flicker it.

- **C:** `KyubuResult light_set(KyubuModHandle self, uint32_t id, const KyubuLight* l)`. Every field of `*l` is taken; a `life_ms`
  above 0 counts again from now, and 0 makes the light live until it is removed.
- **C++:** `KyubuResult kyubu::Mod::light_set(uint32_t id, KyubuLight l)`.
- **Lua:** `kyubu.game.light_set(id, {pos =, radius =, color =, life_ms =, fade_ms =})` returns `true`, or `nil` and the reason.
- **Primitive:** `lights`. **Side:** client only.
- **Errors:** `KY_E_NOT_FOUND` (no light of this mod has that id: it ended, or it is another mod's), and those of `light_spawn`.
- **Since:** 1.1.

### `light_remove`

Ends a light of the mod now.

- **C:** `KyubuResult light_remove(KyubuModHandle self, uint32_t id)`.
- **C++:** `KyubuResult kyubu::Mod::light_remove(uint32_t id)`.
- **Lua:** `kyubu.game.light_remove(id)` returns `true`, or `nil` and the reason.
- **Primitive:** `lights`. **Side:** client only.
- **Errors:** `KY_E_NOT_FOUND` (no light of this mod has that id), and the guard's.
- **Since:** 1.1.

### `world_block`

The block at a point of the calling side's World. A rule that needs the ground or a wall gets the same answer on `Server.exe`
as on `Cube.exe`, and it can tell an unloaded place from rock: `cube::World::getBlock` answers a static solid block for both.

- **C:** `KyubuResult world_block(KyubuModHandle self, int32_t x, int32_t y, int32_t z, KyubuBlock* out)`.
- **C++:** `KyubuResult kyubu::Mod::world_block(int32_t x, int32_t y, int32_t z, KyubuBlock* out)`.
- **Lua:** `kyubu.game.world_block(x, y, z)` returns the type byte and the red, green and blue, or `nil` and `"unknown"`, or
  `nil` and the reason. The coordinates are rounded down.
- **Units:** x east, y north, z up, in blocks (65,536 units to a block). The kind is `type & KY_BLOCK_KIND_MASK` (`type % 32`
  in Lua): 0 air, 1 solid, 2 water, 3 wet ground, 4 and up the ground kinds.
- **What it answers:** in a loaded zone, the stored block; under a column, the game's solid block (grey 200, kind 1); above a
  column, its water block (white, kind 2) at z 0 and below, else its air block (white, kind 0); a stored air block at z 0 and
  below that is not inside a building (`KY_BLOCK_INTERIOR`) reads as water. These are `cube::World::getBlock`'s rules.
- **When:** on the game thread (a World step, `frame.begin`, `sim.tick`). The call holds `cube::World.lock`, under which the
  zone thread clears a zone's slot before it deletes the zone, and a zone enters its slot only when it is built.
- **Primitive:** `terrain`. **Side:** either.
- **Errors:** `KY_E_NOT_FOUND` when the point is in no loaded zone, x or y outside 0 to 2^24 - 1 included (never read it as
  air); `KY_E_INVALID_ARG` when `out` is NULL; and the guard's.
- **Since:** 1.4 (`add-skills-unleashed-multiplayer` 10.1).

### `creature_hit`

A hit by one creature on another through the game's own `cube::World::hitCreature`, as a melee hit: armor (resistance for a
spell), dodge, block and mana shield, the hit flash, the damage number, the kill, its XP and the threat. The stun and the knock
are the mod's, exactly: no damage-free second hit for a stun, and no frame offset.

- **C:** `KyubuResult creature_hit(KyubuModHandle self, KyubuCreature attacker, KyubuCreature target, const KyubuHit* hit, int* landed)`.
- **C++:** `KyubuResult kyubu::Mod::creature_hit(KyubuCreature attacker, KyubuCreature target, const KyubuHit* hit, int* landed)`.
- **Lua:** `kyubu.game.hit(attacker, target, {damage =, stun_ms =, knock = {x, y, z}, magic =})` (also `kyubu.game.creature_hit`) returns `true` when the hit
  went through, `false` when it was void (a dead target), dodged or blocked, or `nil` and the reason. `attacker` may be `nil`.
- **Units:** `damage` before armor (negative heals); `stun_ms` 0 to `KY_HIT_STUN_MAX` (60000); `knock` x east, y north, z up, in
  blocks per second, the direction the game's own hits carry (applyHit turns it into the knockback); `KY_HIT_MAGIC` takes the
  target's resistance off instead of its armor.
- **When:** inside a World step: a `sim.world_step` handler, or later in the same step. Elsewhere `KY_E_PHASE`. The step's
  output and set of changed zones are the loader's (`cube::World::worldStepSite`).
- **Joined client:** the game does not apply a client's hit there but sends it to the server, as it does its own; the target
  may not be the client's own player (the game skips it: `false`).
- **Primitive:** `world_step`. **Side:** either.
- **Errors:** `KY_E_INVALID_ARG` for a NULL `hit`, a `size` below `sizeof(KyubuHit)`, a number that is not finite, `stun_ms` out of
  range or an undefined flag; `KY_E_PHASE` outside a step; `KY_E_UNSAFE` from inside another `creature_hit`; `KY_E_GONE`; and
  the guard's.
- **Since:** 1.4 (`add-skills-unleashed-multiplayer` 10.2).

### `creature_state_get`

Reads a creature's movement and combat state into a `KyubuCreatureState`: position (units), velocity, knockback (`extra_vel`), mode,
ms in the mode, stun left, slow left, charge (`charged_mp`) and aim (`ray_hit`, blocks from the position), and, read only, the
hitbox `box` (`AppearanceData.scale`, blocks, centred on the position: the feet are at `pos z - box z / 2`). The caller sets `size`;
a `size` that ends before `box` (the 10.3 struct) gets everything but `box`. `fields` comes back as `KY_STATE_ALL`.

- **C:** `KyubuResult creature_state_get(KyubuModHandle self, KyubuCreature c, KyubuCreatureState* out)`.
- **C++:** `KyubuResult kyubu::Mod::creature_state_get(KyubuCreature c, KyubuCreatureState* out)`.
- **Lua:** `kyubu.game.creature_state_get(c)`, or `c:state()`, returns `{pos = {x, y, z}, velocity, knock, aim, box, mode, mode_ms,
  stun_ms, slowed_ms, charge}`, or `nil` and the reason.
- **Primitive:** `creature_state` (and `handles`).
- **Side:** both, any creature of the calling side's World, on the game thread.
- **Errors:** `KY_E_INVALID_ARG` (a NULL or short `out`), `KY_E_GONE`; and the guard's.
- **Since:** 1.4 (add-skills-unleashed-multiplayer 10.3).

### `creature_state_set`

Writes the parts of a `KyubuCreatureState` that its `fields` names (`KY_STATE_POS`, `_VELOCITY`, `_KNOCK`, `_MODE`, `_MODE_MS`,
`_STUN`, `_SLOWED`, `_CHARGE`, `_AIM`). A rooted creature is still held at its next move step, whatever is written.

- **C:** `KyubuResult creature_state_set(KyubuModHandle self, KyubuCreature c, const KyubuCreatureState* in)`.
- **C++:** `KyubuResult kyubu::Mod::creature_state_set(KyubuCreature c, const KyubuCreatureState* in)`.
- **Lua:** `kyubu.game.creature_state_set(c, t)`, or `c:set_state(t)`: writes the keys `t` has (those `creature_state` returns; a
  vector's part left out keeps its value); returns `true`, or `nil` and the reason.
- **Primitive:** `creature_state` (and `handles`).
- **Side:** both, for a creature the calling side owns, on the game thread.
- **Errors:** `KY_E_INVALID_ARG` (a NULL or short `in`, a field bit that is not defined, a float that is not finite, a mode above
  255, a position beyond 2^47 units), `KY_E_GONE`, `KY_E_SIDE`; and the guard's.
- **Since:** 1.4 (add-skills-unleashed-multiplayer 10.3).

### `shot_take`

The projectile packets (packet 9) the joined clients sent since the last take, oldest first. Each `KyubuShot` holds the connection
it came on (the player's creature id + 1) and the game's 0x70-byte `cube::Projectile` as sent: the shooter's id, the start, the
velocity, the client's damage, the kind. The game's own packet is not changed: the server still relays it to the other clients.
Packets are kept only from a mod's first call on, at most 256 (past that the oldest go, logged once).

- **C:** `KyubuResult shot_take(KyubuModHandle self, KyubuShot* out, uint32_t max, uint32_t* count)`.
- **C++:** `KyubuResult kyubu::Mod::shot_take(KyubuShot* out, uint32_t max, uint32_t* count)`.
- **Lua:** `kyubu.game.shots()` returns a list of `{ connection =, data = }` (data is the 0x70 bytes as a string), an empty list
  when none, or `nil` and the reason.
- **Primitive:** `player_shots`.
- **Side:** server, Server.exe only, on the game thread.
- **Errors:** `KY_E_INVALID_ARG` (a NULL `count`, a NULL `out` with `max` above 0, a short `out[i]`); and the guard's.
- **Since:** 1.4 (add-skills-unleashed-multiplayer 10.9).

### `projectile_push`

Copies one `cube::Projectile` into the server World's projectile list with the game's own push. The World's projectile step flies it
from the next step on, stops it on blocks and makes its hits, as for a monster's shot. It is not sent to the clients.

- **C:** `KyubuResult projectile_push(KyubuModHandle self, const uint8_t* data, uint32_t len)` (`len` is `KY_SHOT_DATA_SIZE`).
- **C++:** `KyubuResult kyubu::Mod::projectile_push(const uint8_t* data, uint32_t len)`.
- **Lua:** `kyubu.game.projectile_push(data)` (a 0x70-byte string) returns `true`, or `nil` and the reason.
- **Primitive:** `player_shots`.
- **Side:** server, Server.exe only; inside a World step (`sim.world_step`, or later in the same step).
- **Errors:** `KY_E_PHASE` outside a step, `KY_E_INVALID_ARG` (a NULL `data`, another `len`); and the guard's.
- **Since:** 1.4 (add-skills-unleashed-multiplayer 10.9).

## Adding a function

Functions come from mods. A mod that needs something the API does not have yet (the driving mod) goes through these
steps, and its own run in the game becomes the function's in-game check. The example is "give a creature an item",
as `creature_give_item` in a primitive `items`.

0. **The need.** Name what the mod needs from the game, at game level ("add an item to a creature's inventory"), not
   the mod's own logic, which stays in the mod. Where the mapping has only `candidate` entries or none, try the idea
   first with raw access to find out what is really needed.
1. **Mapping.** Every entry the function needs is `verified` for the builds that need it, and every function it calls
   or detours has a `prototype` (the mapping workspace: [mapping/setup.md](mapping/setup.md)).
2. **`primitives.md`.** List the entries in a table row each under the primitive's section of
   [`mapping/atlas/primitives.md`](../mapping/atlas/primitives.md), or add a section for a new primitive.
3. **`entries.inc`.** A `GAME_PRIMITIVE(items, "Items")` line for a new primitive, a
   `GAME_ENTRY(items, "<entry>", <kind>, <builds>)` line per entry (kinds `field`, `type`, `global`, `call`, `detour`,
   `site`; builds `CLIENT`, `SERVER`, `BOTH`), and `GAME_API(creature_give_item, items)`. A primitive whose
   `call`s are mapped for one build only gets `GAME_BUILDS(items, CLIENT)`: on the other build it is unavailable
   ("not mapped for Server.exe yet"), and the entries test fails when a `call`, `detour`, `global` or `site` entry of
   it names another build.
4. **Loader.** The function in `loader/src/game/<primitive>.cpp`, set in `fill_table`. It starts with `GameCall`
   (`loader/src/game/call.h`), which checks the mod, the primitive, the side's World and the game thread, and holds
   the World's lock until it goes out of scope. Then:
   - `g.creature(h)` turns a handle into the creature (`KY_E_GONE`, `KY_E_SIDE`, `KY_E_INVALID_ARG`);
   - `g.owns(cr)` for a change, or anything done for the creature (`KY_E_SIDE` for the other side);
   - `g.handle(cr)` makes the handle of a creature the function hands to the mod;
   - `after_step` (`loader/src/game/world.h`) for any change to the creature map; `before_step` for work that needs
     the step's own state;
   - layouts from `cubeworld/types.h` (`cr->data.hp`), whose compile-time checks pin every offset.

   ```cpp
   KyubuResult KYUBU_CALL k_creature_give_item(KyubuModHandle self, KyubuCreature c, const KyubuItem* item) {
       GameCall g(self, Prim::items, "creature_give_item");
       if (!g) return g.result();
       cube_Creature* cr = g.creature(c);
       if (!cr || !g.owns(cr)) return g.result();
       // ... the function's own logic (KyubuItem is part of the example, not of the API)
       return KY_OK;
   }
   ```

5. **ABI.** Append the member to the game section of `KyubuApi` in `sdk/include/kyubu_api.h`, after the last game
   function and before `/* ---- mod options ---- */`, with a comment giving its version, units and errors. Add its row
   to `kSideRules` in `loader/src/api/api.cpp`: `KY_SIDE_BOTH` unless the function belongs to one side (the owner
   check already decides who may change a creature; `GameCall` reads the row and refuses the other side of a
   one-side function with `KY_E_SIDE`). Name it `<object>_<verb>`, take creatures as `KyubuCreature`, return
   `KyubuResult`, write results through out-parameters, and keep the game's own units. Bump
   `KYUBU_API_VERSION_MINOR` for the first function of a new release (1.1 is not released yet: its functions need no
   bump). Add a wrapper to `kyubu::Mod` in `sdk/include/kyubu.hpp` that checks `api_->size`.
6. **Lua.** A C function in `loader/src/lua/luart.cpp` (registered in the `K` table) and a key in the `api.game`
   table of `loader/src/lua/prelude.lua`. Its Lua name is the member's name without a leading `game_`
   (`creature_give_item` stays `kyubu.game.creature_give_item`; `game_available` is `kyubu.game.available`). Add its
   stub to `sdk/lua/kyubu.lua` as `function game.<name>(...)` with `---@param` and `---@return` lines, and copy the
   file to `sdk/templates/lua-mod/types/kyubu.lua`.
7. **Docs.** An entry in this page's [Functions](#functions): a `### ` heading with the member's name in backticks,
   and at least a `**Side:**` line and an `**Errors:**` line; give the units of every value. Add the function to its
   primitive's row in [Primitives and availability](#primitives-and-availability).
8. **Checks.** These pass, and each fails when its step is skipped:

   | Check | Command | Fails when |
   |---|---|---|
   | Entries test | `pytest tests/tools/test_game_api_entries.py` | an entry is not listed in `primitives.md` (step 2), not `verified`, or has no `prototype` (step 1); a game member has no `GAME_API` line (step 3) |
   | Side-rules test | `kyubu_tests` (`tests/unit/test_api.cpp`) and `pytest tests/tools/test_game_api_checklist.py` | a member has no `kSideRules` row (step 5) |
   | LuaLS test | `pytest tests/tools/test_lua_stubs.py` (runs LuaLS when `lua-language-server` is on `PATH` or `KYUBU_LUALS` names it) | a game member has no key in `api.game` or no stub, or the template's copy differs (step 6) |
   | Docs check | `python tools/check_docs.py` | a game member has no entry here, or a primitive no row (step 7) |

   `tests/tools/test_game_api_checklist.py` walks these steps for a test-only function and skips each in turn. Then
   the driving mod, switched from raw access to the new function, runs in the game: a gamepilot replay under Wine or
   a documented manual check.
