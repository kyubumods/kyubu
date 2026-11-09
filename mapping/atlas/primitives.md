# API primitives

The hand-off to `add-game-api` (game-atlas, "Hand-off to the game-API specification"): the primitives the game API
plans, the mapping entries each one builds on, their statuses, and the gaps that remain. The subsystem pages explain
the entries; this page only inventories them. `kmap check` resolves every qualified name below, and
`test_ready_for_the_api_spec` checks that each stated status matches the mapping.

The first two API slices are **creatures** (entity state, entity list, spawn) and **combat** (animation and mode,
damage). Their primitives cite only `verified` entries. The other five are scoped: each cites the best entries
there are, most of them `candidate`, and lists its gaps.

**Abilities**, the last section, serves `add-ability-api`: seven primitives (design D11) in one section, at the
statuses of 2026-10-05.

**Foundation** is not a primitive of its own: it lists what `add-game-api`'s base layer uses (the World of each side,
the World step, creature handles and ownership), which every primitive builds on. Its entries are `verified` in both
builds unless the Use column says otherwise. The game API's entry list (`loader/src/game/entries.inc`) names this
page's sections, and its entries test checks every entry it uses against them.

Statuses are Cube.exe's. Where a Server.exe twin has a different status, the Use column says so. Format of each
section: a `Slice:` line, a table (Entry | Status | Use), then a `Gaps:` line.

## Foundation

Slice: base (`add-game-api`). Pages: [core](core.md), [world](world.md), [creatures](creatures.md).

| Entry | Status | Use |
|---|---|---|
| `cube::game_controller` | verified | Cube.exe only: the way to the client's World |
| `cube::GameController.world` | verified | Cube.exe only: the World of both sides in single-player, and of a joined client |
| `cube::World::update` | verified | the World step, detoured: captures Server.exe's World (`this`), marks the step, runs the safe points |
| `cube::World.lock` | verified | the World's critical section: the guard holds it for each call (other threads take it on every side) |
| `win32::CriticalSection` | verified | the lock's type: its address goes to `EnterCriticalSection` and `LeaveCriticalSection` |
| `cube::World.creatures` | verified | the creature map: its size for the `game` command, and the look-up behind `game hold` |
| `std::map` | verified | the VC110 map layout, for the size and the look-up by id |
| `std::tree_node` | verified | the map's nodes: the `game` command walks them to count the creatures |
| `cube::World.local_player` | verified | the client side's own creature, the one creature the client side owns |
| `cube::World.joined` | verified | tells a joined client's World from single-player's, for the `game` command |
| `cube::Creature.id` | verified | the creature's id, shown by the `game` command |
| `cube::Creature::~Creature` | verified | the destructor, detoured: invalidates the creature's handles |

Gaps: Server.exe reaches its World only through `cube::World::update`'s `this`, so its server side has no World
before the first step. The two detoured functions have their prototypes (task 2.1), and Server.exe's tick, for
`sim.tick`, is `cube::World::update` itself: `main` calls it exactly once per loop pass (task 2.4,
`probe:server-tick`). The World's lock is taken by more threads than the game thread in every mode (task 2.2): the
zone thread, Cube.exe's 0x469B80 and, in multiplayer, each connection's receive and send threads; the game thread
holds it for the whole step (all of Cube.exe's `cube::GameController::update`).

## Entity state

Slice: 1 (creatures). Pages: [creatures](creatures.md).

| Entry | Status | Use |
|---|---|---|
| `cube::Creature.id` | verified | the entity id, the key of the creature map |
| `cube::Creature.data` | verified | the creature's state block (cuwo's creature data) |
| `cube::CreatureData.pos` | verified | position: 65,536 units per block, x east, y north, z up |
| `cube::CreatureData.velocity` | verified | velocity |
| `cube::CreatureData.body_yaw` | verified | facing: degrees counter-clockwise from +y (north), facing vector (-sin, cos) |
| `cube::CreatureData.hp` | verified | health; the step clamps it to max hp |
| `cube::CreatureData.max_hp_multiplier` | verified | scales max hp (raise it before a high hp) |
| `cube::CreatureData.mp` | verified | mana, or a warrior's rage |
| `cube::Creature.stamina` | verified | stamina |
| `cube::CreatureData.level` | verified | level (armour grows with it) |
| `cube::CreatureData.current_xp` | verified | experience |
| `cube::CreatureData.name` | verified | display name |
| `cube::CreatureData.entity_type` | verified | race or monster type |
| `cube::CreatureData.class_type` | verified | class |
| `cube::CreatureData.hostile_type` | verified | player, hostile, NPC, … |
| `cube::CreatureData.appearance` | verified | the appearance block (models, colours, scales) |
| `cube::CreatureData.equipment` | verified | the equipped items |
| `cube::EntityType` | verified | the names of `entity_type`'s values, 0 to 155 |
| `cube::HostileType` | verified | the names of `hostile_type`'s values: 0 player, 1 hostile, 2 friendly, 3 named friendly, 6 target |

Gaps: `cube::ClassType` and the appearance fields are candidate, so the API gives the class and the look as raw
numbers. body_pitch and specialization (in the creature data) are still candidate and are left out. Which writes the server keeps in multiplayer is only known for
hp (the client decides its own) and position (client-authoritative).

## Entity list

Slice: 1 (creatures). Pages: [creatures](creatures.md), [core](core.md).

| Entry | Status | Use |
|---|---|---|
| `cube::game_controller` | verified | Cube.exe's global game controller, the way in |
| `cube::GameController.world` | verified | the client's World, embedded in the controller |
| `cube::GameController.player` | verified | the local player's creature |
| `cube::Server.world` | verified | the server's World (Server.exe) |
| `cube::World.creatures` | verified | every creature, a map from id to creature |
| `cube::World.local_player` | verified | the local player, as the World sees it |
| `cube::World.joined` | verified | whether the World is a joined client's |
| `std::map` | verified | the VC110 map layout, to walk and look up the creature map |
| `cube::World::update` | verified | the World step: the per-frame point to iterate safely |

Gaps: none for reading on the client. Server.exe has no mapped global like `cube::game_controller`, so a
server-side API needs a hook that captures the Server object to reach `cube::Server.world`.

## Spawn

Slice: 1 (creatures). Pages: [creatures](creatures.md), [world](world.md).

| Entry | Status | Use |
|---|---|---|
| `cube::World.creatures` | verified | recipe step 1, the id: the lowest id in the map, but not above 0, minus 1 |
| `std::map` | verified | the map layout: the lowest id is the leftmost node |
| `operator_new` | verified | step 2: the game's allocator (MSVCR110), 0x1E60 bytes, so the game can free the creature |
| `cube::Creature::Creature` | verified | step 3: the constructor, with the new id; it zeroes the behaviour pointer |
| `cube::EntityType` | verified | step 4: the types a spawn accepts (0 to 155); the other fields are Entity state's |
| `cube::HostileType` | verified | step 4: a spawn's hostility (a player hits 1 and 6, not 2 and 3) |
| `cube::Creature::maxHp` | verified | step 5: `data.hp` is set to it. Cube.exe only for now |
| `creature_appearance_for_type` | verified | step 6: the type's look, a random variation (style NULL). Cube.exe only for now |
| `cube::CreatureData.appearance` | verified | step 6: where the look is written |
| `creature_map_slot` | verified | step 7, the insert: the map's `operator[]`, the creature goes in the returned slot. Cube.exe only for now |
| `cube::Creature::~Creature` | verified | the destructor, for despawn |
| `operator_delete` | verified | its free |
| `cube::World::creatureCreateSite` | verified | where the World inserts a creature it created (on spawn, hook here) |
| `cube::World::creatureDespawnSite` | verified | where the World removes creatures that left the loaded zones (a spawned one too) |
| `cube::World::creatureDeathRemoveSite` | verified | where the World removes a dead creature (set hp to 0 to kill; a spawned one too) |

Gaps: the recipe (steps 1 to 7, the game's own, in the World step; the creatures page has it) replaces a
hand-written tree insert. Its three calls are mapped on Cube.exe only (their Server.exe twins are not), so the first
slice spawns in single-player only; how a spawned creature reaches other players is unknown. Step 8, the behaviour,
is the next section: without it a spawned creature stands still.

## Behavior

Slice: 2 (milestone B of `add-entity-spawner`, monsters that fight). Pages: [creatures](creatures.md), [ai](ai.md).
Raised to verified on 2026-09-30 (the user saw the table of probe `spawn-behavior`). Cube.exe only for now.

| Entry | Status | Use |
|---|---|---|
| `cube::Creature.behavior` | verified | recipe step 8: the creature's behaviour pointer (+0x13E4); zero after the constructor |
| `operator_new` | verified | each behaviour object comes from it (12, 20, 4 and 8 bytes), so `~Creature` can free the sequence |
| `cube::SequentialBehavior::SequentialBehavior` | verified | the container: the zone default's outer object, thiscall, no argument, returns `this` |
| `cube::CombatBehavior::CombatBehavior` | verified | the fighting child; one stack argument (the zone default passes 20.0f) |
| `cube::LookAtPlayerBehavior::LookAtPlayerBehavior` | verified | the second child, no argument |
| `cube::RandomWalkBehavior::RandomWalkBehavior` | verified | the third child, no argument (the zone leaves it out for some kinds) |
| `std::list::push_back` | verified | appends a child: `this` = the sequence + 4, the stack argument the address of the child pointer |
| `cube::CompanionBehavior::CompanionBehavior` | verified | 16 bytes, no argument; the owner's creature id (uint64) goes to +8 and +0xC by hand, as the pet and the summon do; `behavior_companion` (8.3) |
| `cube::WalkPathBehavior::WalkPathBehavior` | verified | 28 bytes, one float (2.0f); the waypoints are a vector at +4; zero the block first (+0x14); `behavior_path` (8.3) |
| `WaypointVector::push_back` | verified | appends a `cube::LongVector3` to a walk path (`this` = the behaviour + 4); `behavior_path` (8.3) |
| `cube::RandomInteractionBehavior::RandomInteractionBehavior` | candidate | 8.1, no visible effect in 8.2, no API kind: 8 bytes, no argument |
| `cube::SpawnLocationBehavior::SpawnLocationBehavior` | candidate | 8.1, no visible effect in 8.2, no API kind: 4 bytes, no argument, no state |

The zone default is built in this order, as in the game (0x60EBAD): the sequence first, then each child is
constructed and appended. The result goes into `behavior`. Nothing else is needed: the creature's World step calls
the behaviour, and `~Creature` and `cube::Creature::reset` free only the sequence's own block (operator_delete, no
virtual destructor), so the children and the list's nodes stay allocated when the creature is removed: a small leak
the game has with its own creatures too.

The rows marked 8.1 (the two candidates left) are milestone D's (`add-entity-spawner`, section 8): the constructors the zone default does not
use, statically read only, candidate until probe `spawn-behavior-all` (8.2). The [ai](ai.md) page's "Behaviour
catalogue" lists what the game gives to whom: no rule keys on the entity type, the region generators choose a
behaviour per Spawn, the zone default covers a Spawn without one (nothing for a TARGET), and the pet and summon
paths build a companion whose owner is a creature id. 8.2 (probe `spawn-behavior-all`, see the `ai` page) built
each class by hand: combat (alone), companion, random walk, look at player and walk path act; spawn location and random
interaction show nothing. Hand-built objects need a zeroed block (`WalkPathBehavior` +0x14, `CompanionBehavior` +4) and
the creature's +0x1C0. Gaps: the meaning of the floats of `CombatBehavior` and `WalkPathBehavior`.

## Animation and mode

Slice: 2 (combat). Pages: [combat](combat.md).

| Entry | Status | Use |
|---|---|---|
| `cube::CreatureData.current_mode` | verified | the current mode (ability or animation id); writing it starts one |
| `cube::CreatureData.mode_start_time` | verified | time spent in the mode; write 0 with a new mode |
| `cube::AbilityId` | verified | the mode ids (0 to 110; most names are placeholders) |
| `cube::CreatureData.charged_mp` | verified | the charge a held ability builds |
| `cube::Creature.ability_cooldowns` | verified | per-ability cooldowns |
| `cube::Creature::modeInterruptible` | verified | whether the current mode may be interrupted |
| `cube::GameController::abilityStartSite` | verified | where the player's key press starts an ability |
| `cube::World::abilityDispatchSite` | verified | the per-creature, per-step switch on the mode (Server.exe twin: candidate) |

Gaps: most `cube::AbilityId` values are placeholder names (MODE_<id>); the primary attack
(`cube::Creature::primaryAttack`) and the mode's duration function are candidate. Mode 60 has no timed end, so the
API must end a forced one itself.

## Damage

Slice: 2 (combat). Pages: [combat](combat.md).

| Entry | Status | Use |
|---|---|---|
| `cube::World::hitCreature` | verified | deal damage: builds the hit, sends it, applies it (both builds) |
| `cube::World::applyHit` | verified | where a hit lands: change or veto it (both builds) |
| `cube::World::hitDamageSite` | verified | where the hit's damage is subtracted (Server.exe twin: candidate) |
| `cube::World::hitForwardSite` | verified | Server.exe: where the server forwards a player's hit (Cube.exe's is candidate) |
| `cube::World::receivedHitSite` | verified | Server.exe: where a client's hit arrives |
| `cube::World::abilityCostSite` | verified | where an ability's cost is paid |
| `cube::World.projectiles` | verified | the projectile list, for ranged damage |
| `cube::Projectile` | verified | a projectile (shooter, velocity, kind, age verified) |
| `cube::World::projectileSpawnSite` | verified | where a projectile is stored (Server.exe twin: candidate) |
| `cube::CreatureData.ray_hit` | verified | the shooter's aim (read-only for the player) |

Gaps: the hit's own fields (`cube::Hit`) are candidate, so the API passes damage through `cube::World::hitCreature`
and does not build hits itself yet. `cube::World::canHit`, armour and resistance are candidate, and so are the
attacker's damage stats (`creature_damage`, `creature_projectile_damage`, 2026-09-30). The projectile's damage,
position and skill fields are candidate.

## Items

Slice: later. Pages: [items](items.md).

| Entry | Status | Use |
|---|---|---|
| `cube::CreatureData.equipment` | verified | the equipped items |
| `cube::ItemData` | verified | an item (cuwo's layout) |
| `cube::ItemData.type` | verified | the item kind: creature_give_item takes 1 to 0x19 (inventory-add) |
| `cube::ItemData.sub_type` | verified | the kind within the type |
| `cube::ItemData.modifier` | verified | the seed of the stat roll |
| `cube::ItemData.rarity` | verified | 0 to 4 |
| `cube::ItemData.material` | verified | the material; for coins 10, 11 or 12 |
| `cube::ItemData.level` | verified | the stored level; the quantity for coins and the stackable types |
| `cube::Creature.inventory` | verified | the creature's bags (the player's, at least): the object `inventory_add` takes, at +0x11DC |
| `cube::Inventory.tabs` | verified | the bags: a vector of vectors of 0x11C-byte slots (a count word, then the item) |
| `cube::Inventory.coins` | verified | the coin counter; coins are not kept in a slot |
| `inventory_add` | verified | adds an item to the bags (the API's insert); tab -1 picks the bag. Cube.exe only for now |
| `cube::ItemId` | candidate | item type ids |
| `item_name` | candidate | an item's display name |
| `item_tooltip` | candidate | builds an item's tooltip |
| `item_received_message` | candidate | the "you received" chat line |
| `stat_scale` | candidate | how an item's level, upgrades and rarity scale its stats (both builds) |
| `item_damage` | candidate | a weapon's DMG (both builds) |
| `item_hp` | candidate | an item's HP bonus (both builds) |
| `item_armor` | candidate | an item's ARMOR (both builds) |
| `item_resistance` | candidate | an item's RESI (both builds) |

Gaps: the game's own refresh of an inventory window that is already open (the loot paths call 0x4C6290 and 0x4C6400 after the
insert) is not mapped, and a function that drops an item is not; the item fields not listed above are cuwo's, unprobed. The stat getters (the four above, and tempo, crit, regeneration and heal on Cube.exe) are named by
the info box's labels but not probed (2026-09-30).

## Camera

Slice: later. Pages: [input](input.md), [rendering](rendering.md).

| Entry | Status | Use |
|---|---|---|
| `cube::CreatureData.ray_hit` | verified | the camera's hit point, rewritten every frame from the camera |
| `cube::GameController::render` | verified | the frame's render: where the camera is applied |
| `cube::GameController::onMouseMove` | candidate | mouse look |
| `matrix_transform_normal` | candidate | a matrix helper used by the renderer |

Gaps: the camera's own angles and distance are not mapped in the game controller. The camera's position and the
render space's origin are (`cube::GameController.camera_position` and `.render_origin`, candidate; the matrices the
world shader gets are in [rendering.md](rendering.md), "The world's camera and the order of a frame").

## Drawing

Slice: later. Pages: [rendering](rendering.md).

| Entry | Status | Use |
|---|---|---|
| `cube::GameController::render` | verified | the frame's render: the hook point for API drawing |
| `draw_creature` | verified | draws one creature's models; `animation_pose`: the function hook of the pose provider (phase 1, the borrowed look; installed since add-ability-animation 4.2) |
| `draw_creature_join_site` | verified | `animation_pose`: Cube.exe 0x41F135, after the pose switch and before the hand adds; EDI is the draw record (phase 2, the override) |
| `cube::game_controller` | verified | `animation_pose`: the global that gives the local player (the Animator's preview clock poses only that creature); the same global as `world` |
| `plasma::Drawing` | verified | the engine's drawing class (RTTI) |
| `plasma::D3D9Engine` | verified | the Direct3D 9 engine (RTTI) |
| `render_init` | candidate | sets the device up |
| `reset_device` | candidate | device reset (resources an API draws with must survive it) |
| `cube::CubeShader::CubeShader` | candidate | the voxel shader |

Gaps: no 2D call (text, rectangles, sprites) is mapped; the loader's own Direct3D hook may be the simpler way to
draw overlays.

## UI widgets

Slice: later. Pages: [ui](ui.md).

| Entry | Status | Use |
|---|---|---|
| `plasma::Widget` | verified | the widget base class (RTTI) |
| `plasma::Widget::Widget` | candidate | its constructor |
| `plasma::load_plx` | candidate | loads a .plx interface file |
| `cube::load_interface_plx` | candidate | loads the game's interface |
| `plasma::set_node_widget` | candidate | attaches a widget to a node |
| `plasma::connect_widget` | candidate | connects a widget's signal to a handler |
| `cube::ChatWidget::ChatWidget` | candidate | the chat box, a place for API messages |

Gaps: the widget tree's root, showing and hiding a widget, and the event slots are not mapped.

## Interface

Slice: later. Pages: [ui](ui.md), [rendering](rendering.md). Serves the `ui` primitive (`ui_node_find`, `ui_node_get`,
`ui_node_child`, `render_get`); Cube.exe only.

| Entry | Status | Use |
|---|---|---|
| `cube::game_controller` | verified | the controller that holds the engine |
| `cube::GameController.engine` | verified | the one `plasma::D3D9Engine`, at +0x800708 |
| `plasma::D3D9Engine` | verified | the engine: owns the scene root and the device |
| `plasma::D3D9Engine.root` | verified | the scene's root node |
| `plasma::D3D9Engine.width` | verified | the client width the engine draws into |
| `plasma::D3D9Engine.height` | verified | the client height |
| `plasma::D3D9Engine.device` | verified | the `IDirect3DDevice9*` |
| `plasma::Node` | verified | a scene-tree node |
| `plasma::Node.name` | verified | its name, a `std::wstring` |
| `plasma::Node.parent` | verified | its parent node |
| `plasma::Node.children` | verified | its children, a `std::list` of node pointers in drawing order |
| `plasma::Node.display` | verified | its display: the colour and mask settings |
| `plasma::Display` | verified | the display part of a node |
| `plasma::Display.colour_index` | verified | which row of the colour table the draw call uses |
| `plasma::Display.colour_table` | verified | the colour rows (RGBA floats) |
| `std::wstring` | verified | the name's string type |
| `std::list` | verified | the children's list type |
| `std::list_node` | verified | its node: `{next, prev}`, then the element |

Gaps: nothing here changes the tree (no hide, show, rename or recolour), and the widget a node holds is not
reachable (`plasma::Widget` has no mapped fields). The node's transformation is not mapped, so a node's place on the
screen is only known while it is drawn (`plasma::D3D9Drawing.bounds` with `plasma::D3D9Engine.world_view`).

## Particles

Slice: later. Pages: [combat](combat.md), [world](world.md), [rendering](rendering.md). Serves the `particles` primitive
(`particle_spawn`); Cube.exe only. The effect is a `cube::ParticleData` pushed on the World step's output.

| Entry | Status | Use |
|---|---|---|
| `cube::World::update` | verified | the step the push runs after: its `step_out` argument is the output |
| `step_output_push_particle` | candidate | the game's list push for the output's particle list |
| `cube::ParticleData` | candidate | the effect: position, acceleration, colour, scale, count, type and spreading |
| `cube::StepOutput` | candidate | what a World step writes |
| `cube::StepOutput.particles` | candidate | the output's particle list, at +0x10 |

Gaps: what each particle type looks like and the units of `accel`, `scale` and `spreading` are not measured (the replay
`particle-types` does it); the Server.exe twin of the push is not mapped; the rest of the output's lists are listed in
`cube::StepOutput` without a verified layout.

## Lights

Slice: later. Pages: [rendering](rendering.md). Serves the `lights` primitive (`light_spawn`, `light_set`, `light_remove`);
Cube.exe only. A light is a `cube::Light` pushed into the frame's light vector before the render sorts it.

| Entry | Status | Use |
|---|---|---|
| `light_sort_site` | verified | after the render's last light push, before the sort and the hand-out to the terrain: the vector at ESP+0xC8 |
| `light_vector_push` | verified | the vector's push_back, as the render's own pushes call it |
| `cube::Light` | verified | the record: position, radius, colour, the squared distance to the camera (the sort key) |

Gaps: the hand-out to the terrain regions and the shader upload are read from the code, not mapped as entries; the flash list
(GameController + 0x800740, the spell flashes) and its push are not mapped; a light pushed at `light_collect_site` lights the
models only.

## Creature flags

Slice: later (`add-dragon-boss` 3.2). Pages: [combat](combat.md). Serves the `creature_flags` primitive (`creature_set_flags`); Cube.exe
only. A flagged creature is unstoppable (applyHit's push, velocity reset, slow and stun are neutralised) or invulnerable (the hit's
damage is cut to 0), by mid hooks that edit registers and state (probe `hit-skip-sites`, `creature-flags`).

| Entry | Status | Use |
|---|---|---|
| `cube::World::hitPushSite` | verified | applyHit: the knockback is stored from XMM5-7; set them to the current extra_vel |
| `cube::World::hitVelocityResetSite` | verified | applyHit: the velocity is zeroed on a stunning hit; save it here |
| `cube::World::hitVelocityResetEndSite` | verified | applyHit: the join after the reset; write the saved velocity back |
| `cube::World::hitSlowSite` | verified | applyHit: the timer at Creature+0x130 is raised through ECX; set ECX to its value |
| `cube::World::hitStunSite` | verified | applyHit: stun_time is raised through ECX; set ECX to its value |
| `cube::World::hitListSite` | verified | hitCreature: the Hit about to be copied to the hit list (damage number) is at ESP+0xC4 |
| `cube::World::hitDamageSite` | verified | applyHit: the hit's damage is subtracted from hp (Hit at EBX, target in ESI) |

Gaps: the sleeping-wake stun write (0x595BCB) is left alone; `cube::Hit` is candidate (target_id at +8 and damage at +0x10 are the
fields used); the Server.exe twins of the five effect sites are not mapped (6.1).

## Creature bodies

Slice: later (`add-dragon-boss` 3.3). Pages: [creatures](creatures.md), [rendering](rendering.md). Serves the `creature_body` primitive
(`body_define`, `creature_body_*`); Cube.exe only. A body of model pieces on a rig is drawn in place of a creature's own parts: the hide
mask argument of `draw_creature` is set at `creature_draw_site`, each piece is drawn at `draw_creature_end_site` with `setColour`,
`setMatrices` and `Model::draw` (probe `model-part-draw`), and the pieces are burst at `creature_death_burst_site` with `createParticles`
(probe `death-burst-call`). The probe
`creature-body` is the check of the whole.

| Entry | Status | Use |
|---|---|---|
| `creature_draw_site` | verified | render: before draw_creature; ESI the creature, the hide mask at [ESP+0x20] is set to all ones |
| `draw_creature_end_site` | verified | the exit of draw_creature: EDI the draw record (+0 the creature), EBP the frame (shader, view, projection, colour) |
| `creature_death_burst_site` | verified | render: a creature with hp <= 0; the pieces are burst here and the draw flag at +0x1D10 cleared |
| `Model::draw` | verified | draws one model with the matrices set before |
| `cube::CubeShader::setColour` | verified | the colour of a draw |
| `cube::CubeShader::setMatrices` | verified | world, view and projection of a draw (verified by probes model-part-draw and creature-body) |
| `cube::GameController::createParticles` | verified | the death burst of a model at a matrix |

Gaps: `creature_draw_site_b` and `_c` (the stealth passes) are hooked when they resolve; they are candidates not reached in a probe yet.

## Terrain

Slice: `add-skills-unleashed-multiplayer` 10.1 (design D9). Pages: [world](world.md). Serves the `terrain` primitive (`world_block`) on
both builds. The loader walks the path that `cube::World::getRegion`, `cube::World::getZone` and `cube::World::getField` take
(Cube.exe 0x434A90, 0x434D10, 0x4347A0; Server.exe 0x406210, 0x406290, 0x406100, the same code) and applies the block rules of
`cube::World::getBlock` (Cube.exe 0x42F860, Server.exe 0x406050). The zone thread puts a zone into its region slot only once it is
built, and clears a zone's or a region's slot under `cube::World.lock` before it deletes it; `world_block` reads under that lock.

| Entry | Status | Use |
|---|---|---|
| `cube::World.regions` | verified | the region slots, index x * 1024 + y (region x, y = zone x, y / 64) |
| `cube::Region` | verified | 0x15A28 bytes (operator new on both builds) |
| `cube::Region.zones` | verified | the zone slots, index (zone x % 64) * 64 + zone y % 64 (zone x, y = block x, y / 256) |
| `cube::Zone.fields` | verified | the 256 by 256 columns, index (y % 256) * 256 + x % 256 |
| `cube::Field` | verified | one column, 0x20 bytes |
| `cube::Field.base_z` | verified | the z of the first block |
| `cube::Field.blocks` | verified | the blocks from base_z up |
| `cube::Field.size` | verified | how many blocks |
| `cube::Block` | verified | 4 bytes: colour, then the type byte |
| `cube::Block.type` | verified | the kind in bits 0 to 4; bit 6 inside a building |

Gaps: `cube::Zone` itself is candidate (only the level fields and `fields` are mapped); the zone thread's writes to a built zone
(block changes) are not traced, and the game's own World step reads the same columns without more locking.

## World step

Slice: `add-skills-unleashed-multiplayer` 10.2 (design D11). Pages: [world](world.md), [combat](combat.md). Serves the `world_step`
primitive (the `sim.world_step` event and `creature_hit`) on both builds. `cube::World::worldStepSite` (Cube.exe 0x60CEAA, Server.exe
0x532C6A) is where both paths of the step's received-hits test join, once per `cube::World::update`; its frame, the same on both
builds, holds the World, the step output and the step's set of changed zones, the two pointers that `cube::World::hitCreature` and
`cube::World::applyHit` take last. `hitCreature` takes a stun chance, not a time (a stun it gives is the target's own maximum, and
only 3 s after the last one), so `creature_hit` passes chance 0 and writes the stun and the direction into the `cube::Hit` at
`cube::World::hitListSite`, for its own call only.

| Entry | Status | Use |
|---|---|---|
| `cube::World::worldStepSite` | verified | the World at [EBP-0x2B18], the output at [EBP-0x2B28], the zone set at EBP-0x2BE8, dt at [EBP+8] |
| `cube::World::hitCreature` | verified | 14 stack arguments (prototype in the mapping); returns false for a void, dodged or blocked hit |
| `cube::World::hitListSite` | verified | the Hit at ESP+0xC4, before it is pushed and applied (Server.exe raised in 10.2) |
| `cube::Hit` | verified | 0x48 bytes |
| `cube::Hit.stun_duration` | verified | the stun, ms |
| `cube::Hit.hit_dir` | verified | the direction, the knockback applyHit gives |

Gaps: the dodge, block and mana-shield paths of `hitCreature` push their own Hit without the list site, so they keep the game's
stun (none) and direction; `creature_hit` gives no crit, no powered hit and no ability id (the ambush poison).

## Creature root

Slice: `add-skills-unleashed-multiplayer` 10.3 (design D12). Pages: [world](world.md), [ai](ai.md). Serves `creature_root`
(`KY_CREATURE_ROOTED`) on both builds. Two sites of `cube::World::update`'s creature loop, at the same frame offsets on both:
`cube::World::aiTickSite` (Cube.exe 0x611B0A, Server.exe 0x5378CA), right after the behaviour timer grows by dt, where the 32 ms
test and the behaviour's update follow (the server copy and computer creatures only); and `cube::World::moveStepSite` (Cube.exe
0x61B3E9, Server.exe 0x5411A9), after the step is dt times velocity plus knockback plus the push of overlapping creatures, before it
is split into parts of a block. A rooted creature gets its behaviour timer and mode set to 0 at the first, and its step cut to a fall
and its acceleration, x and y velocity and knockback zeroed at the second.

| Entry | Status | Use |
|---|---|---|
| `cube::World::aiTickSite` | verified | the creature at [EBP-0x2B14] (and EAX) |
| `cube::World::moveStepSite` | verified | the creature at [EBP-0x2B14], the step (3 floats, blocks) at EBP-0x1C |
| `cube::Creature.behavior_time` | verified | ms since the behaviour ran; below 32 it is not run |
| `cube::CreatureData.velocity` | verified | x and y zeroed, z kept when it falls |
| `cube::CreatureData.current_mode` | verified | held at 0 (none) |
| `cube::CreatureData.mode_start_time` | verified | held at 0 |
| `cube::CreatureData.accel` | verified | the walk push; dt times it is added to the velocity |
| `cube::CreatureData.extra_vel` | verified | the knockback, added to the step |

Gaps: followPath still turns a rooted creature (it is not moved); a rooted player is only held in place (players skip the AI tick).

## Creature state

Slice: `add-skills-unleashed-multiplayer` 10.3 (design D12). Serves `creature_state_get` and `creature_state_set` on both builds:
the fields by their mapped offsets.

| Entry | Status | Use |
|---|---|---|
| `cube::CreatureData.pos` | verified | position, units |
| `cube::CreatureData.velocity` | verified | blocks per second |
| `cube::CreatureData.extra_vel` | verified | the knockback |
| `cube::CreatureData.current_mode` | verified | the mode |
| `cube::CreatureData.mode_start_time` | verified | ms in the mode |
| `cube::CreatureData.stun_time` | verified | the stun left |
| `cube::CreatureData.slowed_time` | verified | the slow left |
| `cube::CreatureData.charged_mp` | verified | the charge |
| `cube::CreatureData.ray_hit` | verified | the aim |
| `cube::CreatureData.appearance` | verified | where the appearance starts |
| `cube::AppearanceData.scale` | verified | the box, blocks (the feet: pos z - box z / 2); the struct itself is a candidate |

## Network transport

Slice: `add-skills-unleashed-multiplayer` 10.4 (design D10). Pages: [network](network.md). Serves `net_transport`: `kyubu.channel`
between `Server.exe` and a joined `Cube.exe`, as one Kyubu packet (id 0x4B594255, then `u32 length, u16 name length, name,
payload`) on the game's own connection. Each receive loop ends every case of its packet switch, and its default, at one loop test;
the loader reads its own packet there, on that thread, and queues it for the next frame or tick. Each connection's send thread
writes the queued packets once per pass, after its 20 ms pacing, when all of the pass's game packets are out, so a Kyubu packet
never splits a game packet. Only while the join gate is in force (D4): a peer without the same gate mods never joins.

| Entry | Status | Use |
|---|---|---|
| `Net::clientReceiveNextSite` | verified | the packet id at [EBP-0x1220], the disconnected byte at [EBP-0x11D1], [EBX] the controller |
| `Net::clientSendPassSite` | verified | [EDI] the controller, the failed byte at [EBP-0x2425] |
| `cube::GameController.socket` | verified | the joined game's socket |
| `Net::connectionReceiveNextSite` | verified | the packet id at [EBP-0x13C8], the disconnected byte at [EBP-0x13A5], EDI the closure (+4 the connections, +8 the slot) |
| `connection_send_pass_site` | verified | EBX the closure, the same layout |
| `cube::Connection` | verified | 0xB0 bytes, one per joined client |
| `cube::Connection.socket` | verified | the client's socket |
| `cube::Connection.creature_id` | verified | the player's creature id; the channel connection id is this + 1 |

Gaps: a send queued on a joined client waits for the next send pass (up to 20 ms); two joined clients (the fan-out to more than
one) are not replayed.

## Player shots

Slice: `add-skills-unleashed-multiplayer` 10.9 (design D16). Pages: [network](network.md), [combat](combat.md). Serves `player_shots`
on Server.exe only: `shot_take` and `projectile_push`. A joined client sends each projectile it shoots as packet 9; case 9 of
`connection_receive_loop` queues it on `cube::Server`'s list, which the main loop hands to the broadcast after `cube::World::update`,
so the server's World never flies it. The loader copies each packet at `Net::receiveProjectileSite` (the connection's thread, the
World lock held, before the game's push) and leaves the game's packet alone, so the relay is unchanged. A mod may push a projectile
of its own into `cube::World.projectiles` inside a World step, with the game's own push; the World's projectile step then flies it,
stops it on the server's blocks and makes its hits (replays `server-sniper-0720` run 6 and `projectile-hit-client-0720` run 7).

| Entry | Status | Use |
|---|---|---|
| `Net::receiveProjectileSite` | verified | the projectile at EBP-0x110, EDI the closure (+4 the connections, +8 the slot); raised in 10.9 |
| `std_list_projectile_push_back` | verified | thiscall (list, projectile): one 0x78-byte node; added in 10.9 |
| `cube::Projectile` | verified | 0x70 bytes, the packet and the list's element |
| `cube::World.projectiles` | verified | the World's list at +0x14 |
| `cube::Connection.creature_id` | verified | the sender: the channel connection id is this + 1 |

Gaps: Cube.exe has no twin (a client's own shots are its World's already). Packets are kept from a mod's first take on, at most 256.

## Server hits

Slice: `add-skills-unleashed-multiplayer`, after task 8.7. Pages: [combat](combat.md). Serves `server_hits` on Server.exe only: no
API function, a fix that is on whenever the entries verify. `cube::World::hitCreature` pushes its Hit on the step output's list at
`cube::World::hitListSite`, then, on a World that is not a client's, calls `cube::World::applyHit` on it; the server's World has no
local player, so for a player target applyHit only pushes the Hit again (`cube::World::hitForwardSite`). The client takes both copies
from its hp, so a joined player lost twice the damage (replay `server-guardian-0720`, run 5). The loader notes the Hit's address at
the list site and its applyHit detour returns early for that Hit when the target is a player.

| Entry | Status | Use |
|---|---|---|
| `cube::World::hitListSite` | verified | the Hit at ESP+0xC4, before hitCreature pushes it and applies it |
| `cube::World::applyHit` | verified | thiscall (hit, step output, changed zones), `ret 0xC`; prototype added for this |
| `cube::Hit` | verified | 0x48 bytes |
| `cube::Hit.target_id` | verified | the target's creature id, looked up in the World's map |
| `cube::Creature.data` | verified | the creature's data |
| `cube::CreatureData.hostile_type` | verified | 0 for a player, the test applyHit makes |

Gaps: hitCreature's miss, block and dodge paths push and apply their own Hit without the list site, so those copies still go twice;
hits made outside hitCreature are not checked. Cube.exe has no twin (its World applies a hit on its own player once).

## World travel

Slice: later. Pages: [travel](travel.md), [world](world.md).

| Entry | Status | Use |
|---|---|---|
| `cube::CreatureData.pos` | verified | the player's position; a teleport writes it |
| `cube::World::generateZone` | verified | generates a zone on arrival |
| `cube::Zone::Zone` | verified | the zone constructor |
| `cube::WorldMap::openSave` | verified | the world map's save |
| `cube::WorldMap::WorldMap` | candidate | the world map |
| `world_map_close` | candidate | closes the map |
| `zone_loader_step` | candidate | loads zones around the player |

Gaps: a teleport by writing the position is not probed (whether zones load in time, and whether the server accepts
it in multiplayer). Map markers and waypoints are not mapped.

## Abilities

Slice: later (milestone B of `add-ability-api`, the fourth ability slot). Pages: [combat](combat.md), [ui](ui.md),
[input](input.md), [saves](saves.md). Cube.exe only unless the Use column says otherwise. One section for ten
primitives (design D11, `ability_use` of task 6.1, `ability_skilltree` of task 8.2 and `ability_iconfile` of task 8.3); the Use column starts with the primitive each entry belongs to. Statuses are as of
2026-10-05, after tasks 2.1 to 2.8.

| Entry | Status | Use |
|---|---|---|
| `cube::CreatureData.current_mode` | verified | `ability_state`: the mode id; writing it starts an ability, 0 ends it. Both builds |
| `cube::CreatureData.mode_start_time` | verified | `ability_state`: the time spent in the mode; write 0 with a new mode. Both builds |
| `cube::CreatureData.mp` | verified | `ability_state`: the mana the cost is paid from. Both builds |
| `cube::CreatureData.hp` | verified | `ability_state`: health, for the fourth slot's own effects. Both builds |
| `cube::Creature.ability_cooldowns` | verified | `ability_state`: the cooldown map, ability id to ms left (+0x139C). Both builds |
| `std::map` | verified | `ability_state`: the map layout, to read and write one cooldown |
| `std::tree_node` | verified | `ability_state`: the map's nodes |
| `cube::CreatureData.stun_time` | verified | `ability_stun`: above 0 the World step holds the mode at 0 (probe `ability-interrupts`); only labels the end reason `stunned`. Both builds |
| `cube::World::abilityDispatchSite` | verified | `ability_step`: once per creature per step, the point where a mod's id runs its per-frame code (Server.exe twin: candidate, not needed) |
| `cube::World.joined` | verified | `ability_step`: tells a joined client's World, whose player still runs its own modes |
| `cube::World.local_player` | verified | `ability_step`: the creature the client side's step guard lets run |
| `cube::CreatureData.hostile_type` | verified | `ability_step`: separates a player from other creatures |
| `cube::GameController::abilityBarBuiltSite` | verified | `ability_bar`: Cube.exe 0x48BFDC, after the learned gate, where the bar is complete and an id may be pushed |
| `cube::GameController.ability_bar` | verified | `ability_bar`: +0x80080C, the skill bar, a `std::vector<int>` rebuilt every frame; 255 is the highest usable id |
| `IntVector::push_back` | verified | `ability_bar`: 0x5BE230, appends an id to the bar (`ret 4`) |
| `IntVector::clear` | verified | `ability_bar`: 0x4AC230, the game's own clear at the start of the build (not called by the API) |
| `cube::CreatureData.class_type` | verified | `ability_bar`: the class that picks the bar's three class ids |
| `cube::CreatureData.specialization` | verified | `ability_bar`: decides one class slot (probe `ability-slot4`) |
| `cube::ClassType` | verified | `ability_bar`: the names of `class_type`'s values |
| `cube::Specialization` | verified | `ability_bar`: the names of `specialization`'s values |
| `ability_cooldown` | verified | `ability_stats`: (id, level) to ms; the skill tree's tooltip reads it, so a hook shows in the tooltip |
| `ability_cost` | verified | `ability_stats`: (id, level) to a float fraction of mp; mostly called inside `canUseAbility` |
| `cube::Creature::modeDuration` | verified | `ability_stats`: no argument, reads the creature's mode; no code outside the mode blocks ends a mode with it |
| `cube::Creature::canUseAbility` | verified | `ability_stats`: one argument; the HUD, the input loop and the cooldown map use it |
| `cube::Creature::modeInterruptible` | verified | `ability_use`: no argument; false while the creature's mode may not be replaced (mode 48, a timer, a mode that has not run its duration). `ability_start` asks it, then `canUseAbility`, as the input loop does. Cube.exe 0x43EBF0 |
| `cube::GameController.ability_icons` | verified | `ability_icons`: +0x800818, a `std::map<int, T*>` from ability id to the icon; the HUD skips a null, and the destructor never frees the values |
| `save_character` | verified | `ability_ranks`: its first stack argument is the played slot (probes `character-slot-played-0` and `-1`) |
| `load_character` | verified | `ability_ranks`: the same slot argument, read in the three callers and seen by probes `character-slot-played-0` and `-1` (raised 2026-10-05 with the user's table) |
| `cube::CreatureData.name` | verified | `ability_ranks`: the player's name (Creature+0x1168), the key of a rank table |
| `cube::GameController::loadIcon` | verified | `ability_iconfile`: `(controller, key string, data3.db Database)` returns the icon object; Kyubu calls it once for a mod ability's `icon_file` with its own data3.db Database (task 8.3; the prototype was corrected from the old `(path, flag)`) |
| `cube::GameController::skillButton9HideSite` | verified | `ability_skilltree`: Cube.exe 0x48ED13, right after the game hides skill button 9 each frame; a mid hook writes 1 into the button's flag there (raised 2026-10-05 with the user's table, task 8.1) |
| `cube::GameController.skill_icons` | verified | `ability_skilltree`: +0x800820, a `std::map<int, T*>` from skill index to icon; key 9's node is null and the hook copies the holder's icon into it |
| `cube::GameController.skill_buttons` | verified | `ability_skilltree`: +0x80083C, the panel's 11 button objects; a button's flag is `node[+0x94][node[+0x68]]`, node at +0x3C |
| `cube::GameController.skills_open` | verified | `ability_skilltree`: +0x8008E9, 1 while the skill panel is open; the hook acts only then |
| `cube::CreatureData.skills` | verified | `ability_skilltree`: the 11 ranks; `skills[9]` is the fourth ability's rank when the tree teaches it (task 8.2) |

The fourth slot runs only with `ability_state`, `ability_step`, `ability_bar`, `ability_stats` and `ability_icons`
all available.

Gaps: `ability_stun`, `ability_ranks`, `ability_skilltree` and `ability_iconfile` are outside that set (the skill tree hook runs unless `[abilities] skill_tree = false`). The game keeps no rank per character for a mod's
ability: Kyubu keeps them in `Save\characters.kyubu-abilities.json`, keyed by the slot that `load_character` (the list's
SELECT) and `save_character` (the autosave) pass and checked against the player's name. `ability_ranks` is available when
all three entries are verified, which they are since `load_character` was raised on 2026-10-05. `cube::World::abilityDispatchSite`'s Server.exe twin is candidate, so a monster's use of a mod's id on the
server is not covered. A dodge roll (W and the middle button) writes 0 to `current_mode`: an `interrupted` end (task 2.7). The cast bar's switch (0x49340C) shows an empty name for
an id above 105 (static); the trail and the footsteps do not read such an id (`atlas/combat.md`, "Who reads the mode for looks").
