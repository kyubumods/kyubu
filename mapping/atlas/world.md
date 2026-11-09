# World

Atlas page for the `world` subsystem: the world object, its zones and regions, the terrain and buildings in them, and
the loader that brings zones in and out around the player. Reference pages: [docs/game/world.md](../../docs/game/world.md).

## Purpose

The world is the generated landscape the player walks through. It is split into zones, which are generated from the
world seed on first visit and saved once changed. Regions hold the content placed on top (missions, monsters, houses,
dungeons). The World object also owns the save database of the current world ([saves](saves.md)). Creatures live in
the world, but have their own page ([creatures](creatures.md)).

## Key classes

All RTTI classes, `verified` as classes (the `rtti:` tag). `cube::World` has fields mapped: `creatures` (below).
Since 2026-09-30 some Server.exe-only fields are candidates too:
- `cube::Zone.level` and `cube::Zone.power_base`: the zone's level and "+N";
- the level fields of `cube::Spawn`;
- the region's area record `cube::MissionData` (cuwo's name, 0x68 bytes, 64 per region at Region+0x14018).

See "Levels in world generation" below.

| Class | Builds | Size (`operator new`) | What it seems to be |
|---|---|---:|---|
| [`cube::World`](../../docs/game/types/cube_World.md) | both | 8,389,276 (Server.exe) | the world: zones, save database, seed-derived parameters |
| [`cube::Zone`](../../docs/game/types/cube_Zone.md) | both | 200 | one zone of terrain, created when first loaded |
| [`cube::Region`](../../docs/game/types/cube_Region.md) | both | 88,616 | a larger area's content: missions and monsters |
| [`cube::House`](../../docs/game/types/cube_House.md) | both | 116 | a building placed in a zone |
| [`cube::Dungeon`](../../docs/game/types/cube_Dungeon.md) | both | | a dungeon (the models `overground-dungeon01.cub` and `underground-dungeon01.cub`) |
| [`cube::Field`](../../docs/game/types/cube_Field.md) | both | | not known yet |
| [`cube::Chunk`](../../docs/game/types/cube_Chunk.md), [`cube::ChunkBuffer`](../../docs/game/types/cube_ChunkBuffer.md) | Cube.exe | | client-side terrain meshes (guess: no Server.exe counterpart) |
| [`cube::ZoneTile`](../../docs/game/types/cube_ZoneTile.md), [`cube::LandscapeTile`](../../docs/game/types/cube_LandscapeTile.md) | Cube.exe | | map tiles, next to `cube::WorldMap` ([travel](travel.md)) |
| [`cube::WorldInfo`](../../docs/game/types/cube_WorldInfo.md) | Cube.exe | 40 | one entry of the world list, with the name at +8 ([saves](saves.md)) |

In Cube.exe the World is not allocated on its own: it is embedded in `cube::GameController` at +0x2DC
([core](core.md)). `cube::World::openSave()` stores the seed at World+0x800164, seeds `rand` with it and derives the
world's generation parameters (+0x800168 … +0x800188). The world's save `cube::Database` is at World+0xAC. The
World's creatures are the `std::map` `cube::World.creatures` at World+4 (verified, `probe:creature-collection`), from
creature id to `cube::Creature*`; how to iterate it is under "Access paths" in [core](core.md).

## Entry points

Constructors and destructors (candidate, `rtti:` plus a call-graph argument; each vtable-storing pair was split by
which one vtable slot 0 calls):

- `cube::World::World()`: builds the save database, a critical section and the building models
  (`building-stone-well.cub`, `framework-wall.cub`, …). Called only by the `cube::GameController` constructor.
- `cube::World::~World()`: saves the loaded zones through `cube::World::saveZone()`, then frees them.
- `cube::World::generateZone()` (Cube.exe 0x5E3280; 1.0.1's model resolver C, alias `ModelResolverC`): builds one
  zone. `this` is the World, the arguments the zone's x and y (0 to 0xFFFF). It first makes the 2×2 regions around the
  zone exist (0x5D8CD0), returns if the zone is already loaded (0x435500), and otherwise builds a `cube::Zone`, seeds
  `rand` from the world seed (World + 0x800188) plus the coordinates, and fills the zone: its creature records, which
  get the zone's ids (8.3), and the spawns of [creatures](creatures.md). Task 8.4 found that its "model-slot
  look-ups" index the new zone's own vectors: it never reads the model table. Its only caller is zone_loader_step.
  In game (probe `model-slots`) it runs on a worker thread, not the draw thread. Its arguments are zones around the
  player's (0x8020, 0x8020), and each call built one `cube::Zone`: three just after the world started, none later
  while standing, and ten in 60 s of walking.
- `cube::Zone::Zone()`: called only by `cube::World::generateZone()`.
- `cube::Region::Region()`: called by 0x5D8CD0 (unnamed, uses the strings `mission` and `monster`), which
  `cube::World::generateZone()` calls.

Regions, missions and dungeons (Server.exe, read statically on 2026-09-30):

- [region_generate](../../docs/game/world.md) (0x50DE10, candidate): the only caller of Server.exe's
  `cube::Region::Region()`, so probably the twin of Cube.exe's 0x5D8CD0 above.
  - It runs a counter from 0 to 63 over the region's missions (areas). An even counter gives its mission a level and +N, so at most 32
    areas get one.
  - It marks every second area (an even counter whose half is odd) as a dungeon zone tile.
- [level_from_power_fraction](../../docs/game/world.md) (0x410A20, candidate): (1/(1 − p) − 1)·20 + 1, the inverse of
  [level_curve](../../docs/game/combat.md). It is shared with four other callers.
- [region_at](../../docs/game/world.md) (0x405DB0) and [mission_at](../../docs/game/world.md) (0x428460),
  candidates: look-ups in the World's 1024 × 1024 region grid (World+0xBC) and in a region's 8 × 8 missions.
- [spawn_level_from_mission](../../docs/game/world.md) (0x4D22B0, candidate): the covering mission's level, or 1.
  mission_influence (0x52C5F0, guess) decides what counts as inside.
- [dungeon_generate](../../docs/game/world.md) (0x5000A0, candidate), with `cube::Dungeon::Dungeon()` (0x4F70D0,
  a u16 layout grid) and `cube::Dungeon::~Dungeon()` (0x4F74A0): builds a dungeon in a zone whose tile kind is 3 or 5,
  with its monsters and chests.
- site_generate (0x4E2630) and spawn_add_loot (0x4FDB20), both guesses: a large spawn generator read only in part,
  and the call that probably fills a dungeon chest.

The zone loader:

- [zone_loader_step](../../docs/game/world.md) (candidate, Cube.exe 0x46AD00): one pass of the loader. It returns at
  once unless the controller's flag is set; otherwise it takes the controller's critical section, walks the zone grid
  around a position, generates the missing zones (`cube::World::generateZone()`) and passes others to 0x5A31B0, then sleeps 20 ms.
  After a new world is set up it calls `cube::World::openSave()` and `cube::WorldMap::openSave()`, and adds the world
  to the world list. It has no direct caller: probably the body of a worker thread's loop.
- The World methods it calls under the critical section: 0x5A30A0, 0x5A3120 and 0x5A31B0 (unnamed; 0x5A31B0 calls
  `cube::World::saveZone()`, so it likely unloads a zone). 0x5A31B0 has the same bytes as Server.exe 0x4D75F0.
- `cube::World::saveZone()` (candidate): serialises a changed zone and writes it under a key built from the string
  `zone` and the zone's coordinates.

The simulation step (8.3, probes `creature-lifecycle` and `creature-lifecycle-server`):

- `cube::World::update()` (Cube.exe 0x60AEA0, Server.exe 0x532260, twins of about 60 KB with the same layout):
  `this` is the World, the first argument the milliseconds to step. `cube::GameController::update()` calls it in
  steps of 20 ms plus the rest (about 1,570 calls in 30 s at 4 fps under Wine); Server.exe's `main` calls it once a
  pass with the elapsed ms (495 calls in 10 s). It builds, steps and removes the creatures of `cube::World.creatures`
  ([creatures](creatures.md) has the three sites).

Blocks (add-skills-unleashed-multiplayer 10.1, design D9; probes [world-block-client](../probes/world-block-client.toml) and
[world-block-server](../probes/world-block-server.toml); all verified, build 07-20):

- `cube::World::getBlock()` (Cube.exe 0x42F860, Server.exe 0x406050) reads one block. It walks `cube::World::getRegion()`,
  `cube::World::getZone()` and `cube::World::getField()` (Cube.exe 0x434A90, 0x434D10, 0x4347A0; Server.exe 0x406210, not
  mapped, then 0x406290 and 0x406100). The code is the same in both exes.
- The layout, from the top:
  - `cube::World.regions`: the region slots, index x * 1024 + y, where region x, y = zone x, y / 64.
  - `cube::Region` (0x15A28 bytes) holds `cube::Region.zones`: index (zone x % 64) * 64 + zone y % 64, where zone x, y =
    block x, y / 256.
  - `cube::Zone.fields`: the 256 by 256 columns of a zone, index (y % 256) * 256 + x % 256.
  - `cube::Field` (0x20 bytes) is one column: `cube::Field.base_z` (the z of its first block), `cube::Field.blocks` (the
    blocks from base_z up) and `cube::Field.size` (how many).
  - `cube::Block` (4 bytes) is a colour, then `cube::Block.type`: the kind in bits 0 to 4, and bit 6 set inside a building.
- Thread safety: the zone thread puts a zone into its region slot only once the zone is built. It clears a zone's or a
  region's slot under `cube::World.lock` before it deletes it. So a reader on another thread holds that lock (the loader's
  `world_block` does).
- The probes compared the walk with the game: 22,032 blocks equal to Server.exe's getBlock (none differ), and 81 column tops
  equal between the server and a joined client.

The step's sites (add-skills-unleashed-multiplayer 10.2 and 10.3, designs D11 and D12; verified on both exes, same frame):

- `cube::World::worldStepSite` (Cube.exe 0x60CEAA, Server.exe 0x532C6A): reached once per `cube::World::update()`, where both
  paths of the received-hits test join. Its frame holds the World at [EBP-0x2B18], the step output at [EBP-0x2B28], the step's
  set of changed zones at EBP-0x2BE8 and dt at [EBP+8]. The output and the zone set are the last two arguments of
  `cube::World::hitCreature` and `cube::World::applyHit` ([combat](combat.md)). The loader's `sim.world_step` event runs here.
- `cube::World::moveStepSite` (Cube.exe 0x61B3E9, Server.exe 0x5411A9), in the creature loop: the creature at [EBP-0x2B14]
  and its step for this update at EBP-0x1C (3 floats, blocks). The step is dt times `cube::CreatureData.velocity`, plus
  `cube::CreatureData.extra_vel` (the knockback), plus the push of overlapping creatures. After this site the step is split
  into parts of a block. dt times `cube::CreatureData.accel` (the walk push) is added to the velocity. The loader's rooted
  flag cuts the step to a fall here and zeroes the acceleration, the x and y velocity and the knockback.
- `cube::World::aiTickSite`, in the same loop, is on the [ai](ai.md) page.

## Data flow

1. The controller's constructor builds the World, which loads the building models. Both constructors also call
   `cube::World::openSave()`; which world they open at start-up is open.
2. Choosing or creating a world sets the seed and name in the controller. The zone loader calls
   `cube::World::openSave()` (seed, parameters, `Save/world_<name>.db`) and `cube::WorldMap::openSave()` (the
   discovered map).
3. As the player moves, the zone loader creates zones around the player: a saved zone is read back, a new one is
   generated from the seed (`cube::Zone::Zone()`, regions through `cube::Region::Region()`, houses and dungeons).
4. Far zones are unloaded, and changed ones are written with `cube::World::saveZone()`; the World's destructor saves
   the rest.
   - The zone blob ([zone_serialize](../../docs/game/saves.md)) keeps ground items and a little per-spawn state, but no
     levels.
   - Spawn levels therefore come from generation on every load.
   - Levels during generation (Server.exe; see the section below):
     - [region_generate](../../docs/game/world.md) sets the region's area levels and +N;
     - `cube::World::generateZone()` copies the zone tile's level and +N into `cube::Zone.level` and
       `cube::Zone.power_base` (0x51A721 to 0x51A740);
     - it rolls wild monsters' levels from [creature_level_range](../../docs/game/creatures.md);
     - [dungeon_generate](../../docs/game/world.md) gives every dungeon monster the zone's level and +N.
5. Each frame `cube::GameController::update()` and `cube::GameController::render()` call into the World code
   (18 and 6 functions in 0x58B000–0x5A8000); Cube.exe then turns zones into meshes (`cube::Chunk`, guess). The
   simulation itself is `cube::World::update()`, called by the controller's update in 20 ms steps (Server.exe: by
   `main` each pass).

## Client and server

- Both builds carry the world code: the World, Zone and Region constructors, `cube::World::saveZone()` and
  `cube::World::openSave()` are correlated (exact instructions, unique strings or the call graph).
- Server.exe has no Chunk, ChunkBuffer, ZoneTile, LandscapeTile, WorldInfo or WorldMap: meshes, the map and the world
  list are client-only. It has no map save either (no `Save/map_` string).
- Server.exe's `cube::World::generateZone()` is 0x5183D0 (37,562 bytes against 37,430; the same imports; it calls the
  twins of `cube::Zone::Zone()` and `cube::Spawn::Spawn()`). Probe `creature-server` (8.5) saw it on a worker thread
  while a client joined and walked, `this` the World and the zones around the player as arguments. Its only caller,
  0x5493D0, has the zone loader's shape (it calls 0x4D75F0, Cube.exe 0x5A31B0) but is smaller than zone_loader_step
  (no `Sleep`, no `operator new`); not named yet.
- In multiplayer the server generates and saves the world; whether the client also generates zones itself (from the
  seed) or only receives them is open. In single-player the client does both.
- The block walk (`cube::World::getBlock()` and the three getters) and the World step's sites (`cube::World::worldStepSite`,
  `cube::World::moveStepSite`, `cube::World::aiTickSite`) are the same code at the same frame offsets in both exes. Where a
  joined client has a zone, its blocks equal the server's (probe world-block-client).

## Hook points of interest

- `cube::Zone::Zone()` (candidate): a zone was created; a mod could decorate or replace it.
- `cube::World::saveZone()` (candidate): a zone is about to be written to the save.
- [zone_loader_step](../../docs/game/world.md) (candidate): once per loader pass, off the main thread, with the
  controller's critical section taken. Its thread is one of three the controller's constructor starts (C++ thread
  functors, zone_loader_step's vtable 0x7019AC); it adds and drops zones under `cube::World.lock` (task 2.2).
- `cube::Region::Region()` (candidate): the start of region content (missions, monsters).
- `cube::World::worldStepSite` (verified, both exes): once per World update, with the World, the step output and the
  changed zones at hand; where a mod hits creatures with `cube::World::hitCreature` inside the step.
- `cube::World::moveStepSite` (verified, both exes): a creature's movement for this update, before it is applied; a mod
  can cut or change it (the loader's rooted flag).
- `cube::World::update()`: the world's simulation step on both sides, with the World as `this` and the step's
  milliseconds. On Server.exe it is the server's tick: `main` calls it exactly once per loop pass
  (`probe:server-tick`), so the loader's `sim.tick` there is emitted after it.
- Levels, for a mod that rescales them (Server.exe addresses, candidates; the Cube.exe twins aren't located yet):
  - [creature_level_range](../../docs/game/creatures.md): wild monsters' level ranges by type.
  - The area-level store in [region_generate](../../docs/game/world.md): the call of level_from_power_fraction at
    0x50E834 and the store after it. The area's +N is rolled from this value right afterwards, so a mod that converts
    levels must do it after the roll, or change the thresholds too.
  - The zone tile copy in `cube::World::generateZone()` (0x51A721 to 0x51A740): after it, `cube::Zone.level` holds the
    dungeon level.
  - [dungeon_generate](../../docs/game/world.md): dungeon monsters and chest loot.

## Open questions

- Which world the two constructors open with `cube::World::openSave()` before the player picks one.
- The World's layout: the position the zone loader centres on. The creature collection (7.3) and the zone container
  (`cube::World.regions`, 10.1 of add-skills-unleashed-multiplayer) are mapped. Answer with the inspector at
  GameController+0x2DC.
- The Server.exe counterpart of zone_loader_step: 0x5493D0, a thread `main` starts, calls
  `cube::World::generateZone()` (8.5) and drops zones under `cube::World.lock` (task 2.2), but differs in shape; how it
  picks the zones (around every player?) is open.
- What `cube::Chunk` and `cube::ChunkBuffer` hold. `cube::Field` and the zone and block units are answered ("Blocks"
  under Entry points). `cube::Zone` itself is a candidate: only its level fields and `cube::Zone.fields` are mapped. The
  zone thread's writes to a built zone (block changes) are not traced.
- Whether the client generates zones in multiplayer. A probe on `cube::Zone::Zone()` while connected.
- The Cube.exe twins of the 2026-09-30 world-generation entries:
  - region_generate: probably 0x5D8CD0;
  - dungeon_generate, the area-level site and the zone tile copy: not located.

  Only Cube.exe's level table was located and compared ([creatures](creatures.md)).
- What site_generate (0x4E2630) builds (spawns with hostile type 5 and the mission's +N). Also what
  mission_influence's special cases for categories 11 to 14 are, and what the other callers of
  level_from_power_fraction generate (probably item levels).

## Depth

deep dive, reviewed 2026-09-30

## Levels in world generation

Read statically in Server.exe on 2026-09-30 (candidates). There are three sources of a creature's level; none of them
is saved, and all are regenerated from the seed:

1. **The type table.** [creature_level_range](../../docs/game/creatures.md) gives a range per entity type, and the
   generator rolls uniformly within it. The ranges go up to 180 to 19,981; the table is on [creatures](creatures.md).
2. **The area ladder.**
   - [region_generate](../../docs/game/world.md) sorts a region's 64 missions (`cube::MissionData`) and runs a counter i from 0 to 63.
     Only an **even** i gives its mission `cube::MissionData.area_level` = ⌊level_from_power_fraction(i/64)⌋ (corrected on
     2026-10-04, tasks 5.1 and 5.2 of `add-level-rework`; the first reading gave a level to every rank).
   - That is vanilla level 1 to 621 over the 32 even counters, evenly spaced in power from 1 to 99 (counter 32 is level 21, power 51;
     level 1,261 would belong to the odd counter 63, which gets none).
   - Spawns inside a mission's radius can take that level ([spawn_level_from_mission](../../docs/game/world.md)).
     Others get 1.
3. **Dungeons.**
   - Every second area (an even counter whose half is odd: 2, 6, ..., 62) becomes a dungeon zone tile, so 16 per region. The tile gets kind 3, one of four styles, and
     the area's level and +N.
   - `cube::World::generateZone()` copies them into `cube::Zone.level` and `cube::Zone.power_base`, and
     [dungeon_generate](../../docs/game/world.md) gives **every** dungeon monster exactly that level and +N (at least
     level 1).
   - Dungeon monsters also get appearance flag 0x1000, which exempts them from the 1% elite roll (flag 0x200, ×10 kill
     XP).
   - Chests (entity 143, TARGET) get the level too, and spawn_add_loot is called with it.

The +N (`cube::MissionData.dword28` and the tile's copy, which becomes each monster's power_base) is rolled from the
**vanilla** area level:

| Area level | +N |
|---|---|
| below 5 | 0 |
| 5 to 9 | 0 or 1 |
| 10 to 14 | 0 to 2 |
| 15 to 18 | 1 to 3 |
| 19 and up | 1 to 4 |

Its effects on the creature are on the [combat](combat.md) page:
- HP × 2^(N/4);
- attack speed 0.75 + N/16;
- a delay of 3000 − 375·N ms;
- ability rank + N;
- kill XP through f(level + N).

The client draws it as " +N" after the creature's name, and only when it is non-zero.

## Level rework sites

Sites of the area levels that the `level_rework` mod uses (change `add-level-rework`, tasks 5.1 and 5.2). The mod's levels are in
the `level_rework` repository.

- `cube::World::regionAreaLevelSite` (0x5D9701, 0x50E841) is the 7-byte store of the area level into mission+0x24 in
  [region_generate](../../docs/game/world.md): EAX is the vanilla level and ESI the even counter. The mod writes
  `floor(1 + 99*(i/2)/31)` into EAX, so the areas climb from 1 to 100. The tile copies and the monsters read it later.
- The "+N" roll compares the stored level with 5, 10, 15 and 19 (eight compares, four for a plain mission and four for a dungeon). The mod
  patches each immediate to 20, 32, 45 and 52, the mod levels where the vanilla thresholds fall, so the same share of areas get each "+N". The
  compares are mod symbols (`level_rework:plus_cmp_*`), not mapping entries.
- The table of levels by kind (creature_level_range, also on `Server.exe`) is converted at its source with `power(lo)` and
  `power(hi)`; see [creatures](creatures.md).
- Probes: `region-area-levels` and `creature-level-range` for `Cube.exe`, `server-level-sites` for `Server.exe` (the twin of the area
  store held).
