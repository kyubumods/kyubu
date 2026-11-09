# Core

Atlas page for the `core` subsystem: program entry, the main loop, the game controller and the SQLite databases.
Reference pages: [docs/game/core.md](../../docs/game/core.md).

## Purpose

Core starts the game, runs the main loop and owns the object everything else hangs from: the game controller. In
`Cube.exe` the controller owns the world, the menus and the databases, and each frame it updates and renders them. In
`Server.exe` a console `main` starts the listener and then steps the world in its own loop instead. The `cube::Database` class wraps
SQLite. It serves both the read-only asset files (`data1.db` … `data4.db`) and the save files ([saves](saves.md)).

## Key classes

- [`cube::GameController`](../../docs/game/types/cube_GameController.md) (verified, `rtti:cube::GameController`,
  base `cube::Controller`). Cube.exe only. Its `operator new` gives a size of 0x1001010 (16,781,328 bytes). The survey
  had put the class in `input` because of its base. It is now in `core`, where its methods are. The one instance is
  in the global [`cube::game_controller`](../../docs/game/core.md) (verified, `probe:game-access-client`). Members
  seen in the functions that use them (a reading aid unless marked mapped):
  - +0x2DC: `world`, the embedded [`cube::World`](../../docs/game/types/cube_World.md) (mapped, verified; see
    [world](world.md)). It is 0x80029C bytes, as in Server.exe, so it ends at +0x800578;
  - +0x2F8 (World+0x1C): the model loader, whose model table (a `std::vector` of 2569 `Model*`) starts at +0x2FC;
  - +0x310 and +0x318 (World+0x34 and +0x3C): `std::map`s keyed by `std::wstring` (444 names, 23 zone-type names);
  - +0x800590: the command line as a `std::string`;
  - +0x8005A8 and +0x8005C8: a flag the zone loader checks first, and the zone loader's critical section;
  - +0x8009D4 … +0x800A08: the world list (a vector of `cube::WorldInfo` pointers) and the selected index;
  - +0x800A48 and +0x800A4C: the current world's seed and name;
  - +0x8006C8: `player`, the local player's [`cube::Creature`](../../docs/game/types/cube_Creature.md) (mapped,
    verified, `probe:creature-collection`; see *Access paths*);
  - +0x800D3C: the embedded [`cube::WorldMap`](../../docs/game/types/cube_WorldMap.md) (RTTI checked in game,
    `probe:world-map-save`) that `cube::WorldMap::openSave()` opens ([travel](travel.md));
  - +0x1001008: the `cube::Database` that holds the world list (`Save/worlds.db`).
- [`cube::Database`](../../docs/game/types/cube_Database.md) (verified, `rtti:cube::Database`): 8 bytes, a vtable and
  the `sqlite3*`. One table, `blobs(key, value)`. The code is the same in both builds.
- [`cube::QuestText`](../../docs/game/types/cube_QuestText.md) and
  [`cube::QuestTextNode`](../../docs/game/types/cube_QuestTextNode.md) (verified, RTTI): quest text trees. They are in
  core only because the survey put them there, and move with the quest work.
- [`cube::Server`](../../docs/game/types/cube_Server.md) (verified, RTTI; Server.exe only, listed under `network`):
  84 bytes, built by Server.exe's `main`. `world` at +4 (mapped, verified, `probe:game-access-server`) points at the
  World `main` built; +8 holds the World's seed and +0xC a copy of its name string (not mapped yet).
- The VC110 containers (verified, `probe:vc110-layouts`, in `mapping/types/vc110.toml`):
  [`std::string`](../../docs/game/types/std_string.md) and [`std::wstring`](../../docs/game/types/std_wstring.md)
  (24 bytes: a 16-byte small-string buffer or pointer, size, capacity),
  [`std::vector`](../../docs/game/types/std_vector.md) (first, last, end),
  [`std::list`](../../docs/game/types/std_list.md) and [`std::map`](../../docs/game/types/std_map.md) (head node, size).

## Entry points

Cube.exe:

- [WinMain](../../docs/game/core.md) (verified): the window, the device, the `cube::GameController` constructor, and
  the message loop.
- [Game::frame](../../docs/game/core.md) (verified, `probe:menu-render-trace`): once per loop pass. It calls
  `cube::GameController::update()` with the frame time in ms, then `cube::GameController::render()`, then `Present`.
- `cube::GameController::GameController()` (verified): builds everything (48 KB of code). It uses the strings
  `Save/characters.db` and `Save/worlds.db`, builds the World (`cube::World::World()`, then
  `cube::World::openSave()`), and loads the models (`ModelLoader::loadAll`).
- `cube::GameController::update()` (verified, vtable slot 10) and `cube::GameController::render()` (verified, slot 11).
- `cube::Database::open()`, `cube::Database::get()` and `cube::Database::getBuffer()` (verified): the read side.
- `cube::Database::set()` and `cube::Database::setBuffer()` (candidate, new): the write side. `set` is an upsert on
  `blobs` (`SELECT 1`, then `UPDATE` or `INSERT`). `setBuffer` has 15 callers, all of them save writers.
- [asset_decode](../../docs/game/core.md) (verified): the asset cipher, applied after `getBuffer`.

Server.exe:

- [main](../../docs/game/core.md) (verified): `WSAStartup`, the World (`operator new(0x80029C)`,
  `cube::World::World()`, `openSave`, the models), the `cube::Server` constructor, a generation thread, then
  [run_server](../../docs/game/core.md) (verified): socket, bind, listen, "Server started. Enter Q to quit.". After
  that `main` itself loops, paced to 20 ms: the server tick (see *Data flow*).
- `Server::simLoop` and `Server::tickSite`, the 1.0.1 names, turned out not to be the simulation (7.5). They are
  [connection_send_loop](../../docs/game/network.md) and connection_send_pass_site (verified, [network](network.md)):
  the thread the accept loop starts for each client, which sends that client the world's changes every 20 ms, and
  the instruction after its pacing. The server's tick is `main`'s loop.
- The `cube::Database` functions, correlated from Cube.exe.

## Data flow

1. `WinMain` constructs the controller once. The constructor opens the save databases, builds the World, and loads
   the models from `data1.db` (`cube::Database::getBuffer()`, then `asset_decode`).
2. On each loop pass, `Game::frame` calls `update(dt)`, then `render()`. `update` calls 18 functions in the World code
   range (0x58B000–0x5A8000), and `render` calls 6. The simulation is advanced by `cube::World::update()`, which
   `update` calls in 20 ms steps plus the rest (8.3, [world](world.md)).
3. The zone loader ([world](world.md)) runs apart from the frame. It takes the controller's critical section, loads or
   generates the zones around the player, and sleeps 20 ms.
4. Writes go through `cube::Database::setBuffer()`: the world list into `worlds.db`, zones into the world save, and
   the discovered map into the map save.
5. Server.exe's tick is `main`'s own loop. Each pass reads `timeGetTime`, calls two `cube::Server` methods (0x427FF0,
   0x427F30; `this` is the Server) that move queues of the Server's into the step's input, locks the World (0x4D3D50
   enters `cube::World.lock`), steps it (`cube::World::update()` with the ms since the last pass, 8.3),
   calls 0x427F90, sends updates (0x427020), rebuilds a list from the World's creature map, unlocks (0x4D5910), then
   sleeps 20 ms. The pass has no branch before the step, so `cube::World::update()` runs exactly once per tick
   (`probe:server-tick`). The other calls are not named yet.

## Access paths

How a mod reaches the objects (task 7.2 for the controller and the World, 7.3 for the creatures; verified in game):

- Cube.exe: `[cube::game_controller]` is the `cube::GameController`; its `world` member (+0x2DC) is the World itself,
  embedded. In the probe notation: `[@0x7691C8] + 0x2DC` (`probe:game-access-client`).
- Server.exe: no global leads to the World or the Server; `main` keeps both as locals. The way in is `this` of a
  `cube::Server` method, then `world` (+4): for example ECX at the entry of 0x427FF0, which `main` calls every tick,
  and `[ECX + 4]` (`probe:game-access-server`, which reads it with `ecx(@0x427FF0)`).
- Both builds: the World's model loader is at World+0x1C.
- The allocator (task 7.4, verified, `probe:game-allocator`): the game has no allocator of its own. Its code calls
  MSVCR110.dll's `operator new` and `operator delete` through jmp [IAT] thunks, mapped as
  [operator_new](../../docs/game/core.md), [operator_delete](../../docs/game/core.md) and
  [operator_delete_array](../../docs/game/core.md) (Cube.exe 0x68C3F8, 0x68C3FE, 0x68C65E; Server.exe 0x54A636,
  0x54A63C, 0x54ABBC). STL containers inline their own size check (`_Xbad_alloc`) around the same `operator new`, and
  `operator new[]` is not imported. Kyubu's `game_alloc`/`game_free` call the same two exports, so a block from one
  may be freed by the other: memory a mod hands to the game must come from `game_alloc`. Both builds also import
  `HeapCreate` and `HeapAlloc`, probably for SQLite's own memory (not checked).
- The creatures (task 7.3, verified in Cube.exe, `probe:creature-collection`): `cube::World.creatures` (World+4) is a
  `std::map` from a creature id (a uint64 at node+0x10) to the `cube::Creature*` (node+0x18). Iterate it as the game
  does: start at the head node's `left`; the next node is the leftmost of the right subtree, or else the first
  ancestor reached from its left; stop at the head (its `isnil` is set). The map's `size` counts the elements. Take
  `cube::World.lock` (World+0x8000C0; *Client and server*) or expect it to change under you: the probe's walker checks the count against
  `size`. Server.exe's `main` walks the same map every tick.
- The local player (Cube.exe): `cube::GameController.player` (+0x8006C8), the creature the controller's constructor
  inserts into `world.creatures` under id 1. In the probe notation: `[[@0x7691C8] + 0x8006C8]`. The probe's
  `inspect player` and `inspect creatures` views follow these paths ([docs/probes.md](../../docs/probes.md)).

## Client and server

- Cube.exe has no `cube::Server` or `cube::Connection` RTTI class; Server.exe has both. In single-player the world
  lives inside the controller and is simulated there: there is no in-process server. `WinMain` passes a flag for
  `Cube.exe server` to the controller's constructor, which never uses it.
- Server.exe has no controller, rendering or UI. Its `cube::World` is allocated on its own (8,389,276 bytes) and
  stepped by `main`'s loop, with a thread per client. Server.exe is authoritative in multiplayer.
- Threads and the World lock (task 2.2; `probe:world-lock-server`, `probe:world-lock-client`, `probe:world-lock`):
  `cube::World.lock` (World+0x8000C0) is a critical section. The game thread holds it around the step: Server.exe's
  `main` each tick, Cube.exe's `cube::GameController::update()` for the whole update (`render` and the input handlers
  take it too). Other threads take it as well: the zone thread (Server.exe's 0x5493D0, started by `main`; Cube.exe's
  zone_loader_step, which adds and drops zones), Cube.exe's 0x469B80 (both started by the controller's constructor,
  so single-player too), and in multiplayer each connection's receive thread (it finds a creature, then writes its
  update, under the lock) and send thread. Code that reads or changes the World outside the step takes it too.
- Shared code: `cube::Database` and the world code ([world](world.md)) are in both builds with the same or nearly the
  same bytes. The survey correlates 2,166 of Server.exe's 6,152 functions.

## Hook points of interest

- `Game::frame` (verified; the loader hooks it for the frame event): once per frame, client only.
- `cube::GameController::update()` (verified; the loader hooks it): the per-frame update, with the controller in ECX.
  `cube::GameController::render()` (verified) is the draw; the call tracer has traced it (`probe:menu-render-trace`).
- `cube::Database::get()` (verified; the loader hooks it for asset overrides) and `cube::Database::setBuffer()`
  (candidate): every blob read and write, with its key. A place to observe or redirect saves.
- Server.exe's tick: `cube::World::update()` (verified), `main`'s only call of it, once per loop pass on the main
  thread, inside the World lock, with the tick's milliseconds as `dt_ms` (`probe:server-tick`). The loader's
  `sim.tick` on Server.exe is emitted after it (add-game-api 3.5). The 1.0.1 `Server::tickSite` (now
  connection_send_pass_site, [network](network.md)) runs once per client and pass, not once per tick.
- [operator_new](../../docs/game/core.md) and [operator_delete](../../docs/game/core.md) (verified): every allocation
  of the game, both builds.

## Open questions

- What the other slots of the controller's vtable (0–9 and 12) do. (The simulation step is `cube::World::update()`,
  8.3.)
- The other `WinMain` globals: 0x7691B4 the device, 0x7691B8 the `IDirect3D9`, 0x7691BC the `cube::CubeShader`,
  0x7691C0 the window, 0x7691CC the `plasma::D3D9Engine` (6.6). Not mapped yet.
- The prototypes of `cube::Database::set()` and `cube::Database::setBuffer()` (argument order and stack cleanup). A
  static read, then a tracer probe during a save.

## Depth

deep dive, reviewed 2026-09-27
