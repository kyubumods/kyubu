# Travel and the map

Atlas page for the `travel` subsystem: the world map and the minimap, map discovery and its save file, and the ways to
travel (hang glider, boat, the teleport ability). Reference pages: [docs/game/travel.md](../../docs/game/travel.md).

## Purpose

The world map shows the part of the world the player has discovered, as tiles (plains, village, forest, mountains,
hills), with an overlay screen and a minimap. Discovery is saved per world. The player crosses the world on foot, with
a hang glider or a boat (items and skills), or with the teleport ability. The map sits between [world](world.md)
(zones and regions, which it summarises), [saves](saves.md) (its file) and [ui](ui.md) (its screen).

## Key classes

- [`cube::WorldMap`](../../docs/game/types/cube_WorldMap.md) (verified as a class, `rtti:cube::WorldMap`; Cube.exe
  only). One vtable with one slot, the deleting destructor (0x5FA4B0). A very large object (more than 8 MB):
  - +0xB0: a 1024 × 1024 grid of pointers to tile blocks, each block 0x1000 `cube::ZoneTile` entries of 0x34 bytes;
  - +0x4000B0: a second 1024 × 1024 array (not read yet);
  - +0x8000B0 and +0x8000B4: two models built in the constructor (the skull and the tiles?);
  - +0x8000BC: the dword saved as discovered;
  - +0x8000F0: its own [`cube::Database`](../../docs/game/types/cube_Database.md), the map save.

  The controller's constructor builds it in place at GameController+0x800D3C: it is embedded, and at the menu that
  object is a complete `cube::WorldMap` with its vtable (`probe:world-map-save`, 7.5).
- [`cube::ZoneTile`](../../docs/game/types/cube_ZoneTile.md) (verified as a class, RTTI; listed under
  [world](world.md)): one map tile, 0x34 bytes. 0x5FA200 stores its vtable (constructor) and 0x5FA340 is its
  destructor (called by vtable slot 0, 0x5FA4E0, and by the map's clean-up).
- [`cube::LandscapeTile`](../../docs/game/types/cube_LandscapeTile.md) (verified as a class, RTTI; under
  [world](world.md)): 0x600F60 stores its vtable; it reads the land key and builds a model, called by 0x469500 (see
  data flow).
- [`cube::MapOverlayWidget`](../../docs/game/types/cube_MapOverlayWidget.md) (verified as a class, RTTI; [ui](ui.md)):
  the map screen, 356 bytes. Its draw slot (vtable slot 1, 0x4C9490, 6.6 KB) calls 0x5F8ED0, 0x600760, 0x600E40
  and 0x600ED0, which look like map and world queries.
- The travel values from cuwo: HANG_GLIDER and BOAT in [`cube::ItemId`](../../docs/game/types/cube_ItemId.md),
  HANG_GLIDING and BOAT_DRIVING in [`cube::SkillId`](../../docs/game/types/cube_SkillId.md), TELEPORT = 49 in
  [`cube::AbilityId`](../../docs/game/types/cube_AbilityId.md) (confirmed statically in 6.3).

## Entry points

All candidate, Cube.exe:

- `cube::WorldMap::WorldMap()` (0x5F9850): called only by `cube::GameController::GameController()`. Loads
  skull.cub and the five map-tile-*.cub models, and constructs the map's `cube::Database`.
- `cube::WorldMap::~WorldMap()` (0x5FA270): called only by vtable slot 0. Calls
  [world_map_close](../../docs/game/travel.md), then the database's destructor.
- [world_map_close](../../docs/game/travel.md) (0x600A10): writes discovered to the map save with
  `cube::Database::setBuffer()`, then frees every tile block. Called by the destructor and by
  `cube::WorldMap::openSave()`.
- `cube::WorldMap::openSave()` (verified in 7.5, alias `Map::openSave`; 0x5FA670, [saves](saves.md)): called by
  [zone_loader_step](../../docs/game/world.md) on the controller's map. Closes the current map with
  [world_map_close](../../docs/game/travel.md), opens Save/map_&lt;name&gt;.db and reads discovered back with
  `cube::Database::getBuffer()`.
- `cube::MapOverlayWidget::MapOverlayWidget()` ([ui](ui.md)): the map screen, built by the game controller.

## Data flow

1. **Start-up.** `cube::GameController::GameController()` constructs the `cube::WorldMap` (tile models, empty grid,
   closed database) and the `cube::MapOverlayWidget`, and finds the minimap node; teleport.png is loaded there too.
2. **Entering a world.** [zone_loader_step](../../docs/game/world.md) calls `cube::WorldMap::openSave()`, which saves
   and frees the previous map ([world_map_close](../../docs/game/travel.md)) and loads discovered from
   Save/map_&lt;name&gt;.db.
3. **Discovering.** 0x469500 (no direct caller: probably a job or a virtual slot) builds tiles as zones are explored:
   it constructs `cube::LandscapeTile` (0x600F60), reads and writes the tile and discovered keys (0x602490, and
   0x5FA8B0 → 0x600BF0, which writes discovered with `cube::Database::setBuffer()`). Tile data goes through a zlib
   helper (0x5FAAB0, zlib 1.2.3).
4. **Showing.** The map screen's draw slot and `cube::GameController::render()` both call 0x600ED0; the draw slot
   reads the tiles through 0x600E40 and 0x600760.
5. **Leaving.** `cube::WorldMap::~WorldMap()` or the next `cube::WorldMap::openSave()` writes discovered back.
6. **Travelling.** The glider and boat models (glider.cub, boat2.cub) come from `ModelLoader::loadAll()`;
   [skill_description_fill](../../docs/game/combat.md) fills @boatspeed and @teleportcooldown, the latter from
   [ability_cooldown](../../docs/game/combat.md) with ability 49. The string Teleport is used in
   `cube::GameController::update()`; "Teleport to City" and "[R] Teleport" are reached only through pointer tables;
   AbilityTeleport, HangGlider and Boat are keys in the world-start table (0x58D370). Where the teleport moves the
   player is not found yet.

## Client and server

- The map is client-only: Server.exe has no `cube::WorldMap`, no map tiles, no Save/map_ string and no map screen.
- Server.exe's world-start table (0x4C8580) has the same AbilityTeleport, HangGlider and Boat keys as Cube.exe's.
- Per cuwo, a client sends its own position, so movement (gliding, boats, teleporting) is decided by the client and
  the server only relays it. To confirm in the binaries with the packet handlers ([network](network.md), 6.6).

## Hook points of interest

- `cube::WorldMap::openSave()` (verified): a world's map is being opened; a mod can keep its own per-world map data
  next to it.
- [world_map_close](../../docs/game/travel.md) (candidate): the map is about to be saved and freed.
- `cube::WorldMap::WorldMap()` (candidate): the map object exists; its address gives the tile grid.
- [ability_cooldown](../../docs/game/combat.md) (candidate, [combat](combat.md)): with ability 49, change the teleport
  cooldown.

## Open questions

- What the discovered dword counts, and what the tile key holds (one key per tile block?).
- What starts 0x469500 (a thread job, a virtual slot), and whether it is the map's discovery step.
- The teleport ability: which function applies it (the per-frame creature step 0x60AEA0 or
  `cube::GameController::update()`), and where the target (the city?) comes from. A probe with a trace on
  [ability_cooldown](../../docs/game/combat.md)'s callers while teleporting would find it (9.1).
- Gliding and boats: the movement code and how the equipped travel items are checked.
- The meaning of the map screen's draw calls (0x5F8ED0, 0x600760, 0x600E40, 0x600ED0).

## Depth

reconnaissance, reviewed 2026-09-27
