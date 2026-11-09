# Creatures

Atlas page for the `creatures` subsystem: the creature object shared by the player, NPCs, monsters and pets, the spawns
that place creatures in the world, and the model look-ups that give them their appearance. Reference pages:
[docs/game/creatures.md](../../docs/game/creatures.md).

## Purpose

Every living thing in the world is a creature: the player, villagers, monsters, pets and companions. A creature has a
race and look, a class, equipment, health and a behaviour ([ai](ai.md)); its abilities and their stats are on the
[combat](combat.md) page. Creatures are placed in the world through spawns when a region is generated
([world](world.md)).

## Key classes

| Class or type | Builds | Size | Status | What it seems to be |
|---|---|---:|---|---|
| [`cube::Creature`](../../docs/game/types/cube_Creature.md) | both | 7,776 (`operator new`) | `verified` (`rtti:`, `probe:creature-collection`) | the creature object; one vtable slot (the deleting destructor) |
| [`cube::Spawn`](../../docs/game/types/cube_Spawn.md) | both | 4,336 (`operator new`) | `verified` (`rtti:`) | a creature placed by region generation (guess: the template a live creature is built from) |
| [`cube::CreatureData`](../../docs/game/types/cube_CreatureData.md) | | 4,456 (0x1168) | `candidate` (cuwo); 16 fields `verified` (`probe:creature-fields`) | cuwo's EntityData: the creature state as the network sends it, embedded in the Creature at +0x10 (verified in 8.1) |
| [`cube::EntityType`](../../docs/game/types/cube_EntityType.md), [`cube::HostileType`](../../docs/game/types/cube_HostileType.md) | | | `verified` (cuwo, `probe:entity-types`, `probe:hostile-types`) | the race or monster kind (0 to 155), and friend or foe |

The layout of `cube::Creature` (8.1, verified, `probe:creature-data`): the vtable at +0, the creature's `id` (a
uint64, its key in `cube::World.creatures`) at +8, and cuwo's whole `cube::CreatureData` block embedded as `data`
from +0x10 to +0x1178: the network format is the memory layout. At the start menu every field of the player's block
reads as cuwo names it (hp 100, level 1, class 1, the appearance scales and model ids, the name `Wollay`). In a new
world, walking moves `data.pos` once per frame, the field finder ranks its low word first among the words that
change while walking, and `data.hp` holds what the HUD shows. The 3,304 bytes after +0x1178 are unknown; +0x1358
holds a copy of the position's y.

The fields the deep dive needs (8.2, verified, `probe:creature-fields`, a Wine replay with a HUD screenshot after
each edit): setting `hp` to half (132.5 of 265) halves both health bars; `mp` is a fraction from 0 to 1 (0.5 shows
as 42/100, and a warrior's drains); doubling `max_hp_multiplier` raises the maximum HP from 265 to 525; `level` 2
and `class_type` 2 change the HUD's "LVL 2 WARRIOR" / "LVL 1 RANGER" line; `current_xp` 25 fills the XP bar to 25/50;
the hair colour in `appearance` recolours the model, the portrait and the map marker; a weapon's material in
`equipment` swaps the held weapon's model (a new warrior's weapon is in slot 7, rings in 8 and 9, a lamp in 10).
`name`, `entity_type` (2, a human male) and `hostile_type` (0) read as gamepilot made the character, and another
creature, a bush, reads 120 (BUSH) and 6 (TARGET). Walking and turning move `pos`, `velocity` and `body_yaw` (in
degrees); attacking changes `current_mode`, and `mode_start_time` is, despite cuwo's name, the time spent in the
current mode in ms (it counts up and restarts when the mode changes). `body_pitch` (only noise-sized changes) and
`specialization` (0 for a new character, nothing visible to edit) stay `candidate`.

Units and facing (task 3.5 of add-entity-spawner, probe `creature-units`, a Wine replay with screenshots): `data.pos` is in
world units of 65,536 per block (cuwo's block scale) and 2^24 per zone, 256 blocks: the player's `pos >> 24` was the zone
(0x8020, 0x8020) of the ids of the zone creatures around it, and creatures placed at whole blocks times 65,536 from the
player read back exactly and were drawn in regular steps. x grows east, y north, z up. `data.body_yaw` is degrees,
counter-clockwise from +y seen from above: 0 faces +y (north), 90 faces -x (west), 180 (or -180) faces -y (south), -90
faces +x (east); the facing vector is (-sin yaw, cos yaw). Four spawned humans with yaw 0, 90, 180 and -90 showed their
back, left profile, face and right profile to the camera of a new world (it looks along +y whatever the player's yaw is),
and the player written to the same yaws turned the same way. So "n blocks ahead" of a creature is
`pos + n * 65,536 * (-sin yaw, cos yaw)`; the entity_spawner mod's first version used (cos, sin) and was wrong at yaw 0.

Where creatures are kept (7.3, verified, `probe:creature-collection`): every live creature is in the World's
`cube::World.creatures`, a `std::map` from a uint64 id to the `cube::Creature*`. The local player's creature is
`cube::GameController.player` (+0x8006C8), stored in that map under id 1. The paths and how to iterate the map are
under "Access paths" in [core](core.md); the probe shows both (`inspect player`, `inspect creatures`).

How creatures come and go (8.3, probes `creature-lifecycle` and `creature-lifecycle-server`): everything happens in
the World's simulation step `cube::World::update()`, which `cube::GameController::update()` calls in 20 ms steps and
Server.exe's `main` once a pass. Standing still, no creature is built or deleted. Walking for 100 s, creatures ahead
are built from their zone's creature records at `cube::World::creatureCreateSite` and the ones left behind are deleted
at `cube::World::creatureDespawnSite` (the World's map went 17, 24, 17, 16 in the discovery run). A creature whose hp
is set to 0 is deleted about a second later at `cube::World::creatureDeathRemoveSite` (the dead-creature loop takes a
non-player creature with hp <= 0 once it has spent 1000 ms in its current mode; a bomb, EntityType 144, explodes
first). The bush killed that way was rebuilt from its zone record in the same step, under the same id. Every removal
is a virtual delete (vtable slot 0, the deleting destructor 0x43D0B0, which calls `cube::Creature::~Creature()`)
followed by an erase from `cube::World.creatures`. Zone creatures carry ids made from their zone: (zone x << 24) |
(zone y << 8) | index, the zone being the position >> 24 (0x8020802006 is creature 6 of zone 0x8020, 0x8020).

The same layout on the server (8.5, probe `creature-server`: Server.exe with a Kyubu-loaded client joined over
127.0.0.1, the script running on the server): the server keeps each joined player as a `cube::Creature` in its own
World's `cube::World.creatures`, under id 1 for the first player, next to the zone creatures under their zone ids.
Read through the layout mapped on the client, the server's copy of the joined player is a complete `cube::Creature`
(RTTI) with `id` 1 at +8 and the client's creature data at +0x10: the name `Tester`, `class_type` 1, `level` 1,
`entity_type` 2, `max_hp_multiplier` 100 and `hp` 265, as the client's HUD shows. While the client walks, the
server's `data.pos` follows it. So the types in `mapping/types/` hold for both builds unchanged. The server builds
the zones around its players with its own `cube::World::generateZone()` (0x5183D0, on a worker thread, from
0x549659 in the loader 0x5493D0), builds their creatures at `cube::World::creatureCreateSite`, deletes the ones
left behind at `cube::World::creatureDespawnSite` (walking back for 120 s: 55 built, 49 deleted) and a creature set to
hp 0 at `cube::World::creatureDeathRemoveSite`, rebuilt in the same step as on the client. The deleting destructor is
0x407630 there.

The creature kinds are named in strings that `cube::World::World()` uses while it builds the model table (`Troll`,
`DarkTroll`, `SpikeCreature`, `PlantCreature`, `UndeadMale`, `UndeadFemale`, and in Server.exe also `FireBeetle` and
`WhiteCat`). The same constructor names the skills (`SkillAbility1` … `SkillAbility5`, `SkillPetTaming`,
`SkillClimbing`, …).

## Entry points

Constructors and destructors (each vtable-storing pair was split by which one vtable slot 0 calls; the Creature pair
is `verified` in both builds by `creature-lifecycle` and `creature-server`, the Spawn pair is candidate):

- `cube::Creature::Creature()`: the Cube.exe callers are `cube::GameController::GameController()` (the player's
  creature, stored in `player` and under id 1), 0x46FE90, 0x482670, 0x487770 and 0x60AEA0 (all unnamed). The Server.exe callers are 0x425290 and
  0x532260.
- `cube::Creature::~Creature()`: called only by the deleting destructor in vtable slot 0.
- `cube::Spawn::Spawn()`: called by `cube::World::generateZone()` and by the large region generators
  it calls (0x5ADD20, 0x5CAF60, 0x5D7EB0 and its helper 0x5D4A90, 0x5DB130 and 0x5EF6F0). Each of them also builds a
  `cube::CombatBehavior` ([ai](ai.md)).
- `cube::Spawn::~Spawn()`: called only by the deleting destructor in vtable slot 0.

Appearance:

- [draw_creature](../../docs/game/creatures.md) (0x4129C0): draws one creature. `this` is a draw record whose first
  word is the `cube::Creature`, and argument 4 is the model table (`ModelLoader`, GameController + 0x2F8). It picks a
  model for each body part, then poses and draws the parts. Callers: `cube::GameController::render()` (four sites),
  0x425550 and 0x604570.
- **The draw record** (probe [anim-draw-record](../probes/anim-draw-record.toml), 2026-10-05): it is a member of the
  creature, at `Creature + 0x1498`, and all six callers pass that address. +0 the creature; +4 the mode time, a copy of
  `mode_start_time` (+0x6C) that `render` writes just before each call (equal in every draw seen, about 3,500);
  +8 the **pose id**, which is not `current_mode`: `render` maps `current_mode` 1 to 110 through a switch to a pose id
  0 to 73 (read from the disassembly: several modes share a pose, 9 modes and every id above 110 give 0; a record seen at runtime: mode 57 gave pose 2, mode 67 pose 37), and sets 0x38 when `Creature + 0x130` is
  above 0. The draw's own switch reads the pose id, and it writes 0 back to +8 when a timed pose ends, so the creature's
  mode is left alone. In a world a creature is drawn through the second `render` site
  (call at 0x4B5D92) unless the float at `Creature + 0x1190` is above 0; modes 48 and 93 also reach the first site (0x4B5CD3, a
  loop of 5 trail draws with argument 5 = 0), and a creature with `+0x1190` above 0 (the game rewrites it every step) is drawn in a later pass of `render` (calls
  at 0x4BA8DE and 0x4BA985). Task 2.2 ([anim-unknown-mode](../probes/anim-unknown-mode.toml)): an id above 110 gives pose 0, which has
  no case in the draw's switch. The preview widgets did not call it during the replay. `cube::Creature::modeDuration` takes the creature, so it
  reads the creature's `current_mode`, not the pose id.
- [ModelLoader::get](../../docs/game/rendering.md) (0x412190): the model table's getter. It returns the `Model*` in
  slot n of the vector at ModelLoader + 4, or null when n is negative or past the vector's end.
- [ModelLoader::itemModel](../../docs/game/items.md) (0x4EB290) and
  [weapon_model_slot](../../docs/game/items.md) (0x51ACE0): the model of an item (see [items](items.md)).

How appearance resolves to model slots (task 8.4). A model slot is an index into the one model table that
[ModelLoader::loadAll](../../docs/game/rendering.md) fills (2569 vanilla slots). `draw_creature` fills one model per
body part:

| Part | From equipment (`cube::ItemData` slot, through `ModelLoader::itemModel`) | Else from `cube::CreatureData.appearance` |
|---|---|---|
| head | none | `head_model` (Creature + 0x8C) |
| hair | none | `hair_model` (+ 0x8E) |
| body | slot 2 (chest) | `body_model` (+ 0x94) |
| hands | slot 4 | `hand_model` (+ 0x90) |
| feet | slot 3 | `foot_model` (+ 0x92) |
| tail | none | `tail_model` (+ 0x96) |
| wings | none | `wing_model` (+ 0x9A) |
| shoulder2 | none | `shoulder2_model` (+ 0x98) |
| shoulders | slot 5 | none |
| neck | slot 1 | none |
| weapons | slots 7 and 6 | none |

- An appearance id is the model slot itself: `draw_creature` reads the int16 and indexes the table, with the same
  bounds check as `ModelLoader::get()`. A negative id, or one past the table's end, draws nothing for that part. An
  empty equipment slot gives no model either, so the appearance id is used instead.
- The bound is the vector's current size, so ids of model slots a mod adds after 2569 (add-mod-loader 7.3) resolve
  through the same path.
- Argument 9 is a mask that hides parts: 1 head, 2 hair, 8 body, 4 shoulders and 0x40 the weapons. Some abilities
  also clear the head, hands, feet and tail while they run.
- The three sites that 1.0.1 listed as model resolvers (add-mod-loader 7.7):
  - Resolver A (0x45A0E0) is `cube::GameController::GameController()` (verified in 1.0.1). Its "look-ups" are
    `ModelVector::at()` on its own widget vectors, and `ModelLoader::get()` with constant slots for the HUD models.
  - Resolver B (0x4890A0) is rejected: [matrix_transform_normal](../../docs/game/rendering.md) is 144 bytes of
    floating-point maths (a vector times the 3×3 part of a matrix). It calls nothing and never reads the table.
  - Resolver C (0x5E3280) is rejected: it is `cube::World::generateZone()` ([world](world.md)), and its look-ups index
    the new zone's own vectors.
- In game (probe `model-slots`, a Wine replay): the new warrior's appearance ids are head 1246, hair 1252, hands 430,
  feet 432 and body 1, and −1 for tail, shoulder2 and wings, so those parts are not drawn. With `hair_model` set to
  1124 (the slot `ModelLoader::itemModel` gives an iron chest piece), that armour model is drawn where the hair was,
  on the character and in the HUD portrait. With 32767 (past the table's end), no hair is drawn and the game keeps
  running. Changing the weapon's sub_type from 17 to 0 changes `weapon_model_slot`'s arguments from (17, 0, 0, 2) to
  (0, 0, 0, 2), and the drawn weapon with them.

The per-frame step:

- `cube::World::update()` (Cube.exe 0x60AEA0, Server.exe 0x532260, 60 KB twins with the same layout; the server
  addresses are 0xD8C40 lower): `this` is the World, the first argument the step in ms. It builds creatures at four
  sites, steps them (the ability stats of [combat](combat.md); 0x5ED1D0 builds a `cube::CompanionBehavior` and a
  `cube::CombatBehavior`) and removes them in two loops:
  - `cube::World::creatureCreateSite` (0x60E958): after a creature was built from a zone's creature record, which
    holds the id (EAX is the new creature). Every creature built while walking came from here.
  - 0x60D1A4 builds a creature for a spawn record that has none yet, with the lowest id in the map minus 1; 0x60FAFC
    builds a bomb (EntityType 144, HostileType TARGET); 0x61423D builds up to three summoned creatures for their
    owner (a random kind, GREEN_SLIME by default, NAMED_FRIENDLY for the player's, HOSTILE otherwise). Not seen in
    a probe yet.
  - `cube::World::creatureDespawnSite` (0x60CD5B) and `cube::World::creatureDeathRemoveSite` (0x60E04B): ECX is the
    creature about to be deleted, living (left behind) or dead.
- Outside the step, 0x487770 builds a temporary creature and deletes it again, on a worker thread (seen once, about
  75 s into the walk; it also writes through `cube::Database::setBuffer()`: guess, saving the character).

The spawn recipe (add-entity-spawner 3.1 to 3.4: a static read of the four sites above, probes `creature-recipe`,
`creature-spawn`, `entity-types` and `hostile-types`). The four run-time paths build a creature the same way, and the
same steps run by a mod in the World step (the entity_spawner mod's raw prototype, on the server side) give a creature
the game steps, draws, damages and removes like its own:

1. The id: the lowest id in `cube::World.creatures` (the leftmost node; the loops start from 0, so never above 0),
   minus 1. Ids compare as signed 64-bit, so run-time ids go -1, -2, …, and a freed one is reused.
2. `operator_new(0x1E60)`.
3. `cube::Creature::Creature(this, &id)`: thiscall, returns `this`, zeroes the behaviour pointer (+0x13E4).
4. The fields of `cube::CreatureData`: `hostile_type`, `entity_type`, `level` and `pos`.
5. `data.hp = cube::Creature::maxHp(creature)` (0x4453B0, thiscall, the float in st0): 100.0 for a level-1 green
   slime. The summon path calls it on the owner instead.
6. `creature_appearance_for_type(&data.entity_type, &data.appearance, NULL)` (0x43FDB0, cdecl): a switch with a case
   for each `cube::EntityType` 0 to 155 (a larger type falls through); style NULL picks a random variation, the
   player passes its character style. The zone create site skips it: the zone generators already filled the record.
7. `*creature_map_slot(&world->creatures, &id) = creature` (0x42CEC0, thiscall): the map's `operator[]` (find or
   insert, it returns the address of the mapped pointer). It is the game's only insert, so no hand-written tree insert
   is needed; other callers use it on 64-bit-keyed maps inside creatures.
8. The behaviour (+0x13E4, `cube::Creature.behavior`): a `cube::SequentialBehavior` (12 bytes from `operator_new`)
   holding a `cube::CombatBehavior` (20 bytes, constructor argument 20.0f), a `cube::LookAtPlayerBehavior` (4) and a
   `cube::RandomWalkBehavior` (8), each appended with `std::list::push_back` (0x4466A0) on the sequence's list at
   +4: the zone default for its monsters (0x60EBAD; TARGET creatures and a few kinds get no walk). Without it a
   spawned creature stands still. The destructor frees the sequence with `operator_delete` only. Probe
   `spawn-behavior` built it by hand for a hostile slime; `creature_spawn` with `KY_SPAWN_MONSTER` builds it in this
   order before the insert (probe `spawn-monster`: the monster came to the player and hit it, the control without
   it stood still).

In `creature-spawn` a green slime built by steps 1 to 7 got id -1 and hp = max hp and was drawn; set to hp 0, it left
at `cube::World::creatureDeathRemoveSite` about a second later, and left behind, at `cube::World::creatureDespawnSite`.
In `entity-types` every type 0 to 155 built this way was drawn (95 is a Santa Claus, `SANTA`; 129, `ENTITY_129`, is
probably an orange flower-like plant). In `hostile-types` the player hurt level-1 slimes of `cube::HostileType` 1
(HOSTILE) and 6 (TARGET) but not 2 (FRIENDLY) or 3 (NAMED_FRIENDLY); the name tag's hp bar is red for 1, blue for 2
and 3 and grey for 6, and 3 gets personal names and a talk prompt. `cube::Creature::reset` (0x446980, candidate)
frees and zeroes the behaviour and clears lists and timers; only the pet path calls it after the insert, and the
recipe does not need it. The recipe's calls are mapped on Cube.exe; Server.exe has only some of them, among them
`cube::Creature::maxHp` (0x40F730, candidate, 2026-09-30).

## Data flow

1. Zone generation (`cube::World::generateZone()` and its region generators) creates a `cube::Spawn` for each creature it places,
   together with the creature's behaviour.
2. A live `cube::Creature` is built when the region is near the player (guess: the Spawn is the template, the Creature
   the instance). The player's own creature is built by the GameController's constructor, before the start menu.
   Live creatures are kept in `cube::World.creatures` by id.
3. Each frame `cube::GameController::update()` calls `cube::World::update()` in 20 ms steps. It builds the
   creatures of nearby zones from their records, steps the creatures, their abilities and behaviours, deletes the
   ones left behind, and deletes the dead a second after they died.
4. In multiplayer the state of each creature travels as `cube::CreatureData` (cuwo's entity update packets). The
   server keeps a joined player as a creature in its own World (id 1 for the first) and copies the client's data into
   it: the player's position on the server follows the client (8.5). The server builds and removes the zone creatures
   around its players itself.
5. **Levels (2026-09-30, static):**
   - A world creature's level is fixed when its `cube::Spawn` is generated (`cube::Spawn.level`, default 1). It never
     grows, because kill XP goes only to players and pets.
   - It comes from the type table ("Creature levels by type" below), from a mission's level, or from the dungeon's
     level ([world](world.md)). The same goes for `cube::Spawn.power_base`, the "+N" the client prints after
     the name.
   - Players and pets level through creature_level_up ([combat](combat.md)). A pet's level is kept in its pet item.


## Client and server

- Both builds have `cube::Creature` (7,776 bytes) and `cube::Spawn` (4,336 bytes). The constructors are correlated by
  identical instructions.
- The creature types are shared: `cube::Creature`, `cube::CreatureData` and `cube::World.creatures` read the same on
  the server (`creature-server`, 8.5), so `mapping/types/` has one layout for both builds.
- Server.exe's zone generation is `cube::World::generateZone()` (0x5183D0), one of the seven callers of
  `cube::Spawn::Spawn()`; its only caller is 0x5493D0, a smaller loop than the client's `zone_loader_step` (not named).
- The server constructs creatures in two places, the client in five: server_accept_loop (a joining player) and
  `cube::World::update()`. The extra client callers may be the character-creation preview and the pets (guesses).
- Server.exe's `cube::World::update()` has the same creation and removal sites (the `cube::World::*Site` entries).
  With no client the server builds and deletes no creature (`creature-lifecycle-server`); with a client connected all
  three sites are passed as on the client (`creature-server`).
- `draw_creature` and the model code (`ModelLoader`, `weapon_model_slot`) are client-only.

## Hook points of interest

- `cube::Creature::Creature()` and `cube::Creature::~Creature()` (verified): every creature's creation and removal,
  in both builds. The place to attach per-creature mod state.
- `cube::Spawn::Spawn()` (candidate): world generation is placing a creature; a mod could change what spawns.
- [creature_level_range](../../docs/game/creatures.md) (candidate, both builds): change the level range of any wild
  creature type at its source.
- `cube::World::update()`: the per-step creature update on both sides.
- `cube::World::creatureCreateSite`, `cube::World::creatureDespawnSite` and `cube::World::creatureDeathRemoveSite`:
  why a creature came or went (a hook on the constructor or destructor sees every case without the reason).
- [draw_creature](../../docs/game/creatures.md): every creature drawn, with its record and the model table; the
  place to swap a part's model. `ModelLoader::itemModel()` decides the model of any item, in the world and in the
  item widgets.

## Tint and the stun's mark

Read from `cube::GameController::render` and confirmed in the game on 2026-10-12 (add-skills-unleashed 7.2c and 7.2d, probes
[taunt-target](../probes/taunt-target.toml) and [taunt-visual](../probes/taunt-visual.toml)).

- **Colour.** `draw_creature` takes a colour vec4 as its argument 6 (r, g, b, a, a multiplier, normally 1,1,1,1). A mid hook at
  `creature_draw_site` (the instruction before the call, ESI the creature, the pointer at [ESP+0x14]) can scale it for one
  draw: the creature is tinted. The game itself sets a pinkish (0.9, 0.5, 0.75) for a hostile creature with flag 0x200 at
  `Creature + 0x7E`. The two draw sites of the later pass (`creature_draw_site_b`, `_c`, ECX the creature) were not reached then; they are for a
  creature with stealth above 0 ("Stealth" below; reached in ninja-scan-skip).
  `Creature + 0x1184` is a hit flash the render passes to the shader, not a colour.
- **The mark.** A creature is drawn with the stun's mark (the model of slot 0x9F5 over the head, at the creature's scale x 0.1,
  turning) while the int at `Creature + 0x12C` is above -3000: positive while stunned, then a 3 s tail. Writing 1 there at
  `stun_mark_gate_site` for the draw (and the old value back at `stun_mark_model_site`) shows the mark on any creature, and the
  `Model*` in EAX at the second site replaces the model: slot 0x9F8 was drawn instead of 0x9F5. A model from `kyubu.model.add`
  (slot 2569 and above) is the intended use.

## Stealth

`cube::Creature.stealth` (+0x1190, a float 0 to 1, not network data), read statically on 2026-10-19 and tried in the game the
same day (add-skills-unleashed 11.2b, probe [ninja-scan-skip](../probes/ninja-scan-skip.toml)). Older notes called it an mp factor.

- **Written by** the World step of each creature (0x618490 to 0x618553): it rises with the sneak ability's power (79; lit lamps
  nearby take some off) or drains, is clamped to 0..1, and is set to 1.0 while the creature has buff kind 3, camouflage (97; see
  [combat](combat.md) "Buffs"). applyHit lowers the victim's by an amount that grows with the damage.
- **Read by** the combat AI's first-target scan (the range shrinks to as little as 0.1 of the 20 blocks; [ai](ai.md) "Target
  choice"), render (a creature above 0 is drawn in the later pass, `creature_draw_site_b` and `_c`, and looks pale white), the
  HUD (a bar while above 0), the mp regeneration and the crit chance.
- **The later pass** (read from the disassembly of `cube::GameController::render` on 2026-10-26, for the Assassin's shadows;
  static): for each creature above 0, render turns the device's colour writes off (SetRenderState 0xA8, 0), draws the creature
  once into the depth buffer alone (`creature_draw_site_b`, the call at 0x4BA8DE), turns them back on (0xF), calls
  `cube::CubeShader::setOpacity` with 1 - 0.75 x stealth (the 0.75 is the float at 0x6FBDC4), draws it again
  (`creature_draw_site_c`, the call at 0x4BA985, frame time 0) and sets the opacity back to 1. The first pass's
  `creature_draw_site` is not reached for such a creature, so a mod that draws extra copies of it (lib/shadow of
  `skills_unleashed`) draws them at `creature_draw_site_c` (ECX the creature, EDI the GameController), sets each copy's own
  opacity there and puts 1 - 0.75 x stealth back after the last. Not yet seen in the game.
- **Held by a mod:** 1.0 written at the start of the player's step (`cube::World::abilityDispatchSite`) read 0.99 at the next
  (standing still the step drains 0.01). The player was drawn in the later pass every frame and looked pale white. A hit on the
  player knocks it to about 0 until the next write. Let go, the game drains it as its own: by 0.01 a step standing still, so
  from 1 to 0 in about 2 s at the replay's 53 steps a second (worked out, not measured: hits were landing), at once under hits.


## Open questions

- The rest of `cube::Creature` after the creature data (+0x1178 to +0x1E60): the behaviour pointer and whatever
  else the client keeps per creature. The word at +4 holds a heap pointer at the menu. The player's creature is the
  object whose ability stats the skill tooltip reads ([combat](combat.md)).
- Where zone records come from and how they keep a killed creature dead (a creature set to hp 0 was rebuilt at once,
  8.3; a real kill may mark the record).
- What a `cube::Spawn` is next to a Creature and the zone records `cube::World::creatureCreateSite` reads (8.3).
  The spawn site 0x60D1A4, the bomb site and the summon site were not seen in a probe.
- What client_connect constructs creatures for (remote players?).
  - 0x482670 and 0x487770 are answered (2026-09-30): [create_character](../../docs/game/saves.md) builds the new
    character, and [save_character](../../docs/game/saves.md) builds a temporary copy to serialize.
  - [creature_data_copy](../../docs/game/creatures.md) (0x44B5C0) is the copy both of them use.
- The Cube.exe offsets of the Server.exe-only level fields (`cube::Spawn.level`, `cube::Zone.level`).

- Why the page is not at deep-dive depth yet (8.5; the user kept it at reconnaissance): of the spec's fields,
  `body_pitch` (orientation) and `specialization` are still `candidate` (8.2). Pitch may need a slope or a jump, and
  a specialization is chosen later in the game. Everything else the creature deep dive asks for is `verified` on both
  sides: the fields, the creation and removal functions, the per-frame update and the model slots.

## Depth

reconnaissance, reviewed 2026-09-30

## Creature levels by type

[creature_level_range](../../docs/game/creatures.md) (Server.exe 0x40EA30; its Cube.exe twin 0x444610 has the identical
68-entry table) gives each entity type a level range, and the generators roll uniformly within it. Names are cuwo's.
The "power" column is ⌊100·f(level) + 1⌋ from [level_curve](../../docs/game/combat.md), which is the scale the weapon
gate and the stats use.

| Types | Level range | Power |
|---|---|---|
| every type not listed below | 2 to 4 | 5 to 14 |
| Collie, ShepherdDog, Terrier, the cats, Turtle, Fly, PlainRunner, Peacock, PlantCreature, RadishCreature | 3 to 6 | 10 to 21 |
| Horse, Cow | 6 to 9 | 21 to 29 |
| the slimes, Hornet, Bat, Biter, LemonBeetle, Crab, Bumblebee | 9 to 14 | 29 to 40 |
| Wolf, Midge, Squirrel, Blowfish | 14 to 21 | 40 to 51 |
| Porcupine, InsectGuard, the Leaf, Snow and Desert Runners, DesertOnionling, Devourer, Raccoon, Camel, LanternFish, MawFish | 21 to 31 | 51 to 61 |
| SkullBull, Ogre, Rockling, Monkey, Parrot, Crocodile, Spectrino, Spitter, Koala, BarkBeetle, SnoutBeetle, Shark | 31 to 47 | 61 to 70 |
| Skeleton, PolarGnoll, FireBeetle, SnowGolem, Yeti, Piranha | 47 to 81 | 70 to 81 |
| Djinn, NomadMale, NomadFemale, Werewolf | 81 to 180 | 81 to 90 |
| Mosquito, Minotaur, Imp, Vampire, HellDemon, EmberGolem | 180 to 19,981 | 90 to 100 (91% of rolls at 100) |

## Equipment seen from the game's loops (add-skills-unleashed 8.2b and 8.2e)

Probes [weapon-swap](../probes/weapon-swap.toml) and [lamp-light](../probes/lamp-light.toml), replayed 2026-10-12 with the dev mods
`weapon_swap_proto` and none. Equipment is 13 `cube::ItemData` of 0x118 bytes from Creature+0x300 (slot n at 0x300 + n * 0x118):
slot 6 (+0x990) the left weapon, slot 7 (+0xAA8) the right weapon, 8 and 9 the rings, **10 (+0xDF0) the lamp, type 0x18**, 11 (+0xF08)
the hang glider or boat (type 0x17), 12 (+0x1020) the pet item (type 0x13). A new warrior's weapon is type 3, sub_type 17, material 2.

- **A weapon swap is one byte.** Writing `sub_type` 6 on the right weapon slot (the item keeps its level, modifier, rarity and
  material) changed, from the next frame: the skill bar from `[57, 59]` to `[22, 25]` (a bow's M1 and M2: see the combat page), the
  HUD's M1 and M2 icons to the bow's, the class line, and the model in the hand to a bow (screenshot in the evidence folder). The
  attack modes follow the same bytes (`cube::Creature::nextAttackMode`), so the player shoots as a ranger. Writing the saved weapon
  back restores all of it on the next frame.
- **The save copies equipment.** `save_character` (the 60 s autosave, the return to the menu, a disconnect) copies the player,
  equipment included, before it serializes. With the bow in the slot the autosave logged `sub_type 6`, so a character saved then keeps
  the bow. A detour of `save_character(this, slot, player)` that puts the real byte back before it calls the game's own and the bow
  after it saved the real weapon (log: "guard true", the slot read 6 at the call and was put back for it).
- **The model of an item** is `ModelLoader::itemModel` ([items](items.md)); the replay [bow-model](../probes/bow-model.toml) hooks it.
- **The lamp.** `cube::CreatureData.flags` (+0x124 of the creature, a u16) bit 0x200 means a lit lamp. Each step of
  `cube::World::update` clears it for a creature whose slot 10 is not type 0x18 (0x61D873), and the creature draw (0x4AF774)
  draws the light only when the bit is set and slot 10 holds the lamp (its radius is 8 times the lamp's rarity plus 10 in the
  code read). Setting the bit by hand on the player stayed set (the flags read 0x240 five seconds later). The pictures were taken
  by day and show no difference: a light there is not seen. Nearby creatures with the bit also change a sum in the player's step
  (0x618322) that the sneak ability's power (id 79) scales: a lit lamp costs stealth. Bit 0x10 is the glider (slot 11 type 0x17) and 0x4 is
  cleared by `cube::GameController::update` each frame (0x4A7001).
- **Quiver and lantern in `draw_creature`** (read statically 2026-10-17, not run). The quiver is model 0x347, drawn on the back for a left or right weapon of sub_type 6 or 7 (sites 0x421193 and 0x421366); the lit lamp's model is drawn in the left hand (site 0x42330C). At each site EAX is the model and EDI the draw record; zero in EAX skips the part (`draw_creature_quiver_left_site`, `_right_site`, `draw_creature_lamp_model_site`). The lamp's light is a different function (0x4AF774) and is not affected.

## A model drawn at a point of the mod's choice (add-skills-unleashed 8.2d)

Probe [world-model-draw](../probes/world-model-draw.toml), replayed 2026-10-12 with the dev mod `prop_proto` (both sides). The game
has no call that draws a model at a world position, and a new draw path is large (EMTweaks draws its rings with its own Direct3D
calls). A creature is the cheap way, because `draw_creature` draws each part from its own id and a part whose model id is -1 draws
nothing:

- Spawn a still creature on the server side (`creature_spawn`; a client script asks with a channel message), then write -1 on the
  eight appearance ids (`head_model` +0x8C, `hair_model` +0x8E, `hand_model` +0x90, `foot_model` +0x92, `body_model` +0x94,
  `tail_model` +0x96, `shoulder2_model` +0x98, `wing_model` +0x9A, int16 each) and the item to draw on the right weapon slot
  (equipment 7, +0xAA8: type 3 and the `sub_type`; its model is the one `ModelLoader::itemModel` gives it, so a mod's own model
  through the hook of the items page works too).
- Only that weapon is drawn, at the creature's position, turned by its yaw (+0x30, degrees). Moving it is a write of `pos` (+0x10,
  three int64 in world units) and of the yaw each server tick, with the velocity (+0x34) zeroed. The creature's size is its scale
  vector at +0x80 (0.8 for a green slime; 0.3 drew a bow a third of the size).
- It is a creature for every other purpose: it has an id, hp, a hostile type (2 friendly and 6 target were tried), and a name and a
  health bar that show while the crosshair is on it. It stays one more entry in the World's creature map, so the mod has to remove
  it when the prop is done (not tried: set hp to 0, or the game's own removal).
- In a joined game the creature belongs to the server, and its weapon slot goes to every client in its network update: expected,
  not tried (the multiplayer task, 8.2g of the sniper).
- Seen in play 2026-10-12: a prop that holds a bow gets the quiver drawn with it (so `skills_unleashed` gives its prop a marked
  sword, sub_type 0, and the mod's model through the `ModelLoader::itemModel` detour), and its name and health bar float over it
  while the crosshair is on it.
- **Taking the name away** (read statically 2026-10-12, after a first try failed in play). Once a frame
  `cube::GameController::update` fills a list with the creatures of the World that are near the player and whose hp (+0x16C) is
  above 0 (the loop at 0x490B20). Everything about the creature under the crosshair reads only that list: the test of the
  crosshair against each creature's box, the look-at target (the id at GameController+0x800A68), and the floating name and health
  bar, which are drawn for the entries the crosshair is on (and for players). So a creature whose hp reads 0 for the length of that
  loop is never named: `cube::GameController::lookListFillSite` is just before the loop and `lookListFilledSite` just after it
  (both candidates), a mod writes 0 at the first and the creature's own hp back at the second, and nothing else runs in between.
  Both hooks must be in place: a creature left at hp 0 is deleted a second later.
- **Creature+0x11C8 is not the look-at target.** It is the id of the creature's pet: the World step writes a pet's id (hostile
  type 5) there on its owner (0x60C7FD), and `cube::GameController::update` shows the pet's frame of the HUD from it. The first
  version of the mod's prop wrote 0 there and the name stayed.

## Posing

How a creature's pose reaches the screen (the `add-ability-animation` change; evidence in `kyubu-mapping/evidence/ability-animation-2026-10-06/`). Words only: the
rig reference (task 2.8) is the last part of this section, and no rest-pose numbers belong in this repository.

- **One record per creature.** [draw_creature](../../docs/game/creatures.md) (0x4129C0) takes the creature's draw record (a block inside the creature, with the pose id,
  the pose vectors and the model slots). A pose id picks a case of a switch (a mode above 110 gives pose 0); the case writes target vectors, and the record eases
  towards them every draw, so the record is the smoothed pose and keeps easing back after a pose ends.
- **Where to write.** 0x41F135 is a safe mid-hook site (verified, task 2.4): it sits after the switch and the smoothing and before the four adds of the hand vectors,
  it runs once per drawn creature, and the draw record is in EDI there. A plain float write at the record's offsets is drawn in the same frame.
- **The parts** are root, body, head (the hair follows), hand A, hand B and tail. A part has a position in model units and an euler in degrees; the angle helpers
  work in degrees on column-major matrices, post-multiplied, so the first call is the outermost rotation.
- **Hand B is the player's right hand** (the one on the left of the screen in the front view the game starts with). The warrior's club is **not** moved by the hands while
  it is holstered on the back at idle: writing the hand vectors left it where it was. Whether it follows hand B once it is drawn (a swing) is not tested: in task 2.8 the club followed the root and the trunk and never a hand, at idle. Answered for a mod ability (task 5.1, probe anim-tracks): in pose 0 of a mode above 110 the club is drawn in hand B, and it follows hand B's position and euler.
- **Measured at the join (task 2.5, probe pose-write, Wine, 1024x768).** With no write the hand's picture is the same to the pixel in every baseline phase. A write of
  90 degrees on each of x, y and z changes the fist's outline; a position write of 20 on z takes the fist out of its place; a position write of 20 on x together with
  90 degrees on z moves it a long way to the side, while 90 degrees on z alone does not move it. **Corrected 2026-10-11 (task 7.5, the parity check over the
  baked poses and a clip capture):** that does not show the position going through the rotation. Measured on the part matrices, a hand's position is **not turned by
  the hand's own angles**: hand B with 14 on z stays 14 voxels up at 0, 80 and 160 degrees on x, and in a baked pose with 90 degrees on all three axes it sat where the
  position put it in the body's frame (the body's turn carries it). A hand's three angles run z, then y, then x (all six orders tried: only this one is exact).
  The head's and the other parts' positions were not measured with a turned part. Not settled: a position write on y showed no change, and one on x alone only a
  small one in the screenshots; the matrices show x, y and z moving the hand by that many voxels.
- **Time and weapon family (task 2.6, probe draw-arg5).** The fifth argument of `draw_creature` is the frame time in whole milliseconds (the game's own, the same for every creature in a frame; 0 in the trail and glow calls). The record's easing uses it, so it eases at the same pace at any frame rate (half-way in about 70 ms). Four pose cases ask `creature_two_hand_stance` (the right weapon slot holds a bow, crossbow, boomerang, staff, wand, longsword or one of the great weapons) and give hand A a grip on hand B's weapon when it is true; the test reads the item, not the class. Static for the weapon part.
- **A trap for offsets.** The creature's data starts 0x10 into the creature object, so an offset of the cuwo layout needs 0x10 added before it is read or written through a creature pointer (the cuwo class and mode offsets are 0x130 and 0x58, and the probes read them at 0x140 and 0x68; a yaw write at the cuwo offset 0x20 was followed by the player falling through the world, the position being the likeliest thing it hit).

- **A creature as a thrown body (task 9.2a of add-skills-unleashed, probe [trap-body](../probes/trap-body.toml), 2026-10-17).** A spawned creature that stands still is moved by the game's own physics from a velocity written at `Creature+0x34` (blocks a second; z up): gravity is about 30 blocks/s^2, the on-ground bit (`Creature+0x5C` bit 0) comes up at the landing, the vertical velocity is zeroed there and the ground friction brings the horizontal speed to 0 in about 1.5 s. A write of the vertical velocity at the landing (fastest fall seen times 0.55) makes it bounce. The creature is hidden by the hide mask of `draw_creature` (stack argument 9, `[ESP+0x20]` at `creature_draw_site`): 0xFFFFFFFF there leaves it undrawn, while the colour's alpha only darkens it and a write to the appearance scale at `Creature+0x80` did not shrink it. A creature whose hp is set to 0 is gone by the next lookup, without a visible effect.
- **A creature frozen in place (task 9.2b of add-skills-unleashed, probe [snare-freeze](../probes/snare-freeze.toml), 2026-10-17).** Holding a creature still takes three writes each World step: its horizontal velocity and knockback at 0, and its stun timer (`Creature+0x12C`) at 1000, which stops its attacks (a zombie beside the player did no damage for 16 s) and puts it in the stunned mode. Its pose is frozen at the draw join site (`draw_creature_join_site`): the draw record's words +0x10 to +0x4EC saved at the first draw and written back at every draw gave the same picture for 5 s; saving only up to +0xEC let the arms drift for about 1.5 s. The stun's mark model is kept away by writing -4000 to the stun timer at `stun_mark_gate_site` (the model site is then not reached), but star shapes like the stun's still showed over the frozen creature from the second second on, from a source not found.
- **Part matrices (task 7.2, probe rig-read).** When `draw_creature` returns, the record holds one 4x4 matrix per part, 0x40 bytes each from `rec + 0xEC` (17 slots up to `rec + 0x4EC`), in Direct3D's row-vector form (the translation in the fourth row) with a uniform scale of one voxel in world units and the creature's own yaw. The player fills slot 0 (root), 1 (trunk hub), 2 (body), 3 (head), 4 (hair), 7 (the holstered weapon), 9 (hand A), 10 (hand B, drawn mirrored), 15 and 16 (the feet); the slots of a tail, wings, the shoulders and the neck are zero for this player. Written at the join site, a head euler changes only the head and hair matrices, by the angle written; a body euler also turns the hands and the weapon and moves (does not turn) the head; a root euler also turns the feet; a hand euler changes that hand only. A head, hand or body rotation turns about the part's own pivot (the neck, the hand's centre, the trunk hub). The root's x and y offset is in **world units** (10 moved the root 116 voxels); the other positions are in voxels. A model's origin is its minimum corner. Numbers are in the private `kyubu-mapping/evidence/rig-read-2026-10-10/`.

### Rig reference (task 2.8, probe pose-axes)

Each part's euler was written at +30 and -30 degrees on each axis in the front view and the left profile (84 screenshots, `kyubu-mapping/evidence/ability-animation-2026-10-06/pose-axes-run1/`). Read from the sheets, not measured.

- **The frame of every euler.** x is toward the player's right, y is forward, z is up; angles are degrees and right-handed. Positive x tips the top of a part backward (the head looks up, the trunk leans back); negative x tips it forward. Positive y rolls the top toward the player's right; negative y toward the left. Positive z turns the part counter-clockwise seen from above (toward the player's left); negative z the other way. The sides are the player's own: **hand A is the left hand** (the right side of the screen in the front view), **hand B the right hand**.
- **root** (rotation about a pivot near the middle of the figure): moves the whole figure, feet and the club on the back included. x leans it back or forward with the feet swinging the other way; y rolls it; z turns it about the vertical.
- **body** (the trunk): moves the trunk, the club on the back and the two hands, which hang from it. The head stays upright over the neck and the feet stay planted. x leans, y rolls, z turns the trunk.
- **head** (about the neck): moves the head and its hair only. x looks up (positive, the chin shows) or down (negative, the top of the hair shows), y tilts, z turns. The cleanest part of the rig: all six writes read plainly.
- **hand A, hand B**: turn the fist and swing it about its parent, a weapon held in the hand following. A 30 degree write moves a fist by less than its own width and changes which face of the cube shows; the club holstered on the back does not follow the hands at idle. The direction of a hand's swing is read from two views only.
- **tail**: moves the tail where the creature has one. The player has none (model slot empty): no change in either view.
- **Which creatures lack a part.** The part's model slot in the record decides (0 = not drawn, see the slots above): the human male warrior has head, hair, body, hands, feet and the club, and no tail, wings, shoulder2, shoulders or neck. Feet, wings, neck and shoulders have no pose vector at all.
- **Safe range.** Only 30 degrees was drawn, and nothing faulted or vanished in any of the 84 phases. Larger angles, position writes per axis and a weapon in the hand are not tested.
- **A limit of the run.** The front view's root row starts in a turned view (the player a third of a turn off the camera for the first 30 s): read the root's axes in the left profile.
