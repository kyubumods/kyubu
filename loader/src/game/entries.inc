/* The game API's entry list (openspec add-game-api, design D1): its primitives, the mapping entries each one uses,
   and its API functions. This is the only place that says what the game API uses. The loader checks it at boot
   against the running build (game.cpp), and tests/tools/test_game_api_entries.py checks it against
   mapping/atlas/primitives.md, the mapping and kyubu_api.h.

     GAME_PRIMITIVE(id, "section of primitives.md")
     GAME_BUILDS(primitive, builds)              (optional) the builds the primitive is mapped for
     GAME_ENTRY(primitive, "mapping entry", kind, builds)
         kind:   field | type | global | call (the API calls it) | detour | site (a mid-function hook)
         builds: CLIENT (Cube.exe) | SERVER (Server.exe) | BOTH
     GAME_API(member of KyubuApi after the game marker, primitive or none)

   A primitive is available on the running build when each of its entries for that build is verified; every
   primitive also needs `world`. Without a GAME_BUILDS line a primitive is for both builds; with one it is unavailable
   on any other build ("not mapped for Server.exe yet"), whatever its entries say. The `call`, `detour`, `global` and
   `site` entries of a limited primitive name only its builds (layouts are shared, so `field` and `type` say BOTH). */

#ifndef GAME_PRIMITIVE
#define GAME_PRIMITIVE(id, section)
#endif
#ifndef GAME_BUILDS
#define GAME_BUILDS(primitive, builds)
#endif
#ifndef GAME_ENTRY
#define GAME_ENTRY(primitive, entry, kind, builds)
#endif
#ifndef GAME_API
#define GAME_API(member, primitive)
#endif

/* The World of each side, the game thread and the safe points around each World step. */
GAME_PRIMITIVE(world, "Foundation")
GAME_ENTRY(world, "cube::game_controller", global, CLIENT)
GAME_ENTRY(world, "cube::GameController.world", field, CLIENT)
GAME_ENTRY(world, "cube::World::update", detour, BOTH)
GAME_ENTRY(world, "cube::World.lock", field, BOTH)
GAME_ENTRY(world, "win32::CriticalSection", type, BOTH)
GAME_ENTRY(world, "cube::World.creatures", field, BOTH)
GAME_ENTRY(world, "std::map", type, BOTH)
GAME_ENTRY(world, "std::tree_node", type, BOTH)
GAME_ENTRY(world, "cube::World.local_player", field, BOTH)
GAME_ENTRY(world, "cube::World.joined", field, BOTH)

/* Creature handles and ownership. */
GAME_PRIMITIVE(handles, "Foundation")
GAME_ENTRY(handles, "cube::Creature::~Creature", detour, BOTH)
GAME_ENTRY(handles, "cube::Creature.id", field, BOTH)

/* A creature's state (openspec add-entity-spawner, D4). Its functions pass creatures as handles, so they need
   `handles` too (GameCall checks it when it makes or resolves a handle). */
GAME_PRIMITIVE(entity_state, "Entity state")
GAME_ENTRY(entity_state, "cube::Creature.data", field, BOTH)
GAME_ENTRY(entity_state, "cube::CreatureData.pos", field, BOTH)
GAME_ENTRY(entity_state, "cube::CreatureData.velocity", field, BOTH)
GAME_ENTRY(entity_state, "cube::CreatureData.body_yaw", field, BOTH)
GAME_ENTRY(entity_state, "cube::CreatureData.hp", field, BOTH)
GAME_ENTRY(entity_state, "cube::CreatureData.level", field, BOTH)
GAME_ENTRY(entity_state, "cube::CreatureData.entity_type", field, BOTH)
GAME_ENTRY(entity_state, "cube::CreatureData.hostile_type", field, BOTH)

/* Building a creature with the game's own recipe (D3; primitives.md, Spawn, steps 1 to 7). It writes Entity state's
   fields, so creature_spawn needs `entity_state` and `handles` too. Its calls are mapped on Cube.exe only (milestone C
   maps their Server.exe twins). */
GAME_PRIMITIVE(spawn, "Spawn")
GAME_BUILDS(spawn, CLIENT)
GAME_ENTRY(spawn, "operator_new", call, CLIENT)
GAME_ENTRY(spawn, "cube::Creature::Creature", call, CLIENT)
GAME_ENTRY(spawn, "cube::Creature::maxHp", call, CLIENT)
GAME_ENTRY(spawn, "creature_appearance_for_type", call, CLIENT)
GAME_ENTRY(spawn, "creature_map_slot", call, CLIENT)
GAME_ENTRY(spawn, "cube::CreatureData.appearance", field, BOTH)
GAME_ENTRY(spawn, "cube::EntityType", type, BOTH)
GAME_ENTRY(spawn, "cube::HostileType", type, BOTH)

/* Step 8 of the recipe: the behaviours (D3, D8; primitives.md, Behavior). creature_spawn checks `behavior` when a spawn
   asks for any behaviour (the sequence, combat, look-at-player and random-walk kinds: monster, friendly, default), and
   also `behavior_companion` for KY_SPAWN_COMPANION and `behavior_path` for KY_SPAWN_GUARD, so a kind whose
   constructors are candidate is refused alone. Calls are mapped on Cube.exe only. */
GAME_PRIMITIVE(behavior, "Behavior")
GAME_BUILDS(behavior, CLIENT)
GAME_ENTRY(behavior, "operator_new", call, CLIENT)
GAME_ENTRY(behavior, "cube::Creature.behavior", field, BOTH)
GAME_ENTRY(behavior, "cube::SequentialBehavior::SequentialBehavior", call, CLIENT)
GAME_ENTRY(behavior, "cube::CombatBehavior::CombatBehavior", call, CLIENT)
GAME_ENTRY(behavior, "cube::LookAtPlayerBehavior::LookAtPlayerBehavior", call, CLIENT)
GAME_ENTRY(behavior, "cube::RandomWalkBehavior::RandomWalkBehavior", call, CLIENT)
GAME_ENTRY(behavior, "std::list::push_back", call, CLIENT)
GAME_PRIMITIVE(behavior_companion, "Behavior")
GAME_BUILDS(behavior_companion, CLIENT)
GAME_ENTRY(behavior_companion, "cube::CompanionBehavior::CompanionBehavior", call, CLIENT)
GAME_PRIMITIVE(behavior_path, "Behavior")
GAME_BUILDS(behavior_path, CLIENT)
GAME_ENTRY(behavior_path, "cube::WalkPathBehavior::WalkPathBehavior", call, CLIENT)
GAME_ENTRY(behavior_path, "WaypointVector::push_back", call, CLIENT)

/* Adding an item to a creature's bags (docs/game-api.md, "Adding a function"; primitives.md, Items). It builds the item from
   a few verified ItemData fields and passes it to the game's insert. The call is mapped on Cube.exe only. */
GAME_PRIMITIVE(items, "Items")
GAME_BUILDS(items, CLIENT)
GAME_ENTRY(items, "cube::Creature.inventory", field, BOTH)
GAME_ENTRY(items, "cube::ItemData.type", field, BOTH)
GAME_ENTRY(items, "cube::ItemData.sub_type", field, BOTH)
GAME_ENTRY(items, "cube::ItemData.modifier", field, BOTH)
GAME_ENTRY(items, "cube::ItemData.rarity", field, BOTH)
GAME_ENTRY(items, "cube::ItemData.material", field, BOTH)
GAME_ENTRY(items, "cube::ItemData.level", field, BOTH)
GAME_ENTRY(items, "inventory_add", call, CLIENT)

/* The fourth ability slot (openspec add-ability-api, D11; primitives.md, Abilities). The slot runs only when the first five
   primitives (ability_state, ability_step, ability_bar, ability_stats, ability_icons) are all available: game.cpp's
   ability_slot_available. ability_stun, ability_ranks and ability_skilltree are outside that set and fall back (a stun ends an ability with
   `interrupted`; every rank reads 1; the skill tree does not show the fourth ability). ability_ranks is verified since 2026-10-05 (load_character raised with the user's table). The step site and the three stat detours are mapped on both builds (add-skills-unleashed-multiplayer 3.2: on Server.exe they run a remote
   player's mod ability); the bar, icons, ranks file, input calls and skill tree are Cube.exe only, and Server.exe's slot is state + step + stats. */
GAME_PRIMITIVE(ability_state, "Abilities")
GAME_ENTRY(ability_state, "cube::CreatureData.current_mode", field, BOTH)
GAME_ENTRY(ability_state, "cube::CreatureData.mode_start_time", field, BOTH)
GAME_ENTRY(ability_state, "cube::CreatureData.mp", field, BOTH)
GAME_ENTRY(ability_state, "cube::CreatureData.hp", field, BOTH)
GAME_ENTRY(ability_state, "cube::Creature.ability_cooldowns", field, BOTH)
GAME_ENTRY(ability_state, "std::map", type, BOTH)
GAME_ENTRY(ability_state, "std::tree_node", type, BOTH)
GAME_PRIMITIVE(ability_stun, "Abilities")
GAME_ENTRY(ability_stun, "cube::CreatureData.stun_time", field, BOTH)
GAME_PRIMITIVE(ability_step, "Abilities")
GAME_ENTRY(ability_step, "cube::World::abilityDispatchSite", site, BOTH)
GAME_ENTRY(ability_step, "cube::World.joined", field, BOTH)
GAME_ENTRY(ability_step, "cube::World.local_player", field, BOTH)
GAME_ENTRY(ability_step, "cube::CreatureData.hostile_type", field, BOTH)
GAME_PRIMITIVE(ability_bar, "Abilities")
GAME_BUILDS(ability_bar, CLIENT)
GAME_ENTRY(ability_bar, "cube::GameController::abilityBarBuiltSite", site, CLIENT)
GAME_ENTRY(ability_bar, "cube::GameController.ability_bar", field, CLIENT)
GAME_ENTRY(ability_bar, "IntVector::push_back", call, CLIENT)
GAME_ENTRY(ability_bar, "cube::CreatureData.class_type", field, BOTH)
GAME_ENTRY(ability_bar, "cube::CreatureData.specialization", field, BOTH)
GAME_ENTRY(ability_bar, "cube::ClassType", type, BOTH)
GAME_ENTRY(ability_bar, "cube::Specialization", type, BOTH)
GAME_PRIMITIVE(ability_stats, "Abilities")
GAME_ENTRY(ability_stats, "ability_cooldown", detour, BOTH)
GAME_ENTRY(ability_stats, "ability_cost", detour, BOTH)
GAME_ENTRY(ability_stats, "cube::Creature::modeDuration", detour, BOTH)
GAME_PRIMITIVE(ability_icons, "Abilities")
GAME_BUILDS(ability_icons, CLIENT)
GAME_ENTRY(ability_icons, "cube::GameController.ability_icons", field, CLIENT)
GAME_PRIMITIVE(ability_ranks, "Abilities")
GAME_BUILDS(ability_ranks, CLIENT)
GAME_ENTRY(ability_ranks, "save_character", detour, CLIENT)
GAME_ENTRY(ability_ranks, "load_character", detour, CLIENT)
GAME_ENTRY(ability_ranks, "cube::CreatureData.name", field, BOTH)
/* ability_start (task 6.1): the game's two questions a press of key 4 asks before it writes the id. Cube.exe only, and outside
   the slot's set: a build without them has no ability_start, and slot 4 still works. */
GAME_PRIMITIVE(ability_use, "Abilities")
GAME_BUILDS(ability_use, CLIENT)
GAME_ENTRY(ability_use, "cube::Creature::modeInterruptible", call, CLIENT)
GAME_ENTRY(ability_use, "cube::Creature::canUseAbility", call, CLIENT)
/* ability_skilltree (task 8.2, design D13): the skill panel's button 9 (ABILITY4). Cube.exe only, outside the slot's set: without it
   the panel does not show the fourth ability and the ranks come from the ranks file. */
/* ability_iconfile (task 8.3, design D14): the game's icon loader, called once for a mod ability's own icon (a data3.db key). Cube.exe only,
   outside the slot's set: without it a mod ability shows its vanilla `icon`. */
GAME_PRIMITIVE(ability_iconfile, "Abilities")
GAME_BUILDS(ability_iconfile, CLIENT)
GAME_ENTRY(ability_iconfile, "cube::GameController::loadIcon", call, CLIENT)
GAME_PRIMITIVE(ability_skilltree, "Abilities")
GAME_BUILDS(ability_skilltree, CLIENT)
GAME_ENTRY(ability_skilltree, "cube::GameController::skillButton9HideSite", site, CLIENT)
GAME_ENTRY(ability_skilltree, "cube::GameController.skill_icons", field, CLIENT)
GAME_ENTRY(ability_skilltree, "cube::GameController.skill_buttons", field, CLIENT)
GAME_ENTRY(ability_skilltree, "cube::GameController.skills_open", field, CLIENT)
GAME_ENTRY(ability_skilltree, "cube::CreatureData.skills", field, BOTH)

/* animation_pose (openspec add-ability-animation, task 3.4): the pose provider's entries: the detour of 4.2, the join site of 5.1 and the game controller global (the local player, for the preview). Cube.exe only: Server.exe draws nothing. The
   binding calls (ability_animation_bind) return KY_E_UNAVAILABLE on the client side while either is not verified; a server side only
   records the binding. */
GAME_PRIMITIVE(animation_pose, "Drawing")
GAME_BUILDS(animation_pose, CLIENT)
GAME_ENTRY(animation_pose, "draw_creature", detour, CLIENT)
GAME_ENTRY(animation_pose, "draw_creature_join_site", site, CLIENT)
GAME_ENTRY(animation_pose, "cube::game_controller", global, CLIENT)

/* ui: the UI's scene tree and the render device, read only (docs/game-api.md, ui_node_find). Cube.exe only: Server.exe has no plasma
   engine. The functions take no GameCall (the title screen has no stepping World); they check the main thread themselves. */
GAME_PRIMITIVE(ui, "Interface")
GAME_BUILDS(ui, CLIENT)
GAME_ENTRY(ui, "cube::game_controller", global, CLIENT)
GAME_ENTRY(ui, "cube::GameController.engine", field, CLIENT)
GAME_ENTRY(ui, "plasma::D3D9Engine", type, CLIENT)
GAME_ENTRY(ui, "plasma::D3D9Engine.root", field, CLIENT)
GAME_ENTRY(ui, "plasma::D3D9Engine.width", field, CLIENT)
GAME_ENTRY(ui, "plasma::D3D9Engine.height", field, CLIENT)
GAME_ENTRY(ui, "plasma::D3D9Engine.device", field, CLIENT)
GAME_ENTRY(ui, "plasma::Node", type, CLIENT)
GAME_ENTRY(ui, "plasma::Node.name", field, CLIENT)
GAME_ENTRY(ui, "plasma::Node.parent", field, CLIENT)
GAME_ENTRY(ui, "plasma::Node.children", field, CLIENT)
GAME_ENTRY(ui, "plasma::Node.display", field, CLIENT)
GAME_ENTRY(ui, "plasma::Display", type, CLIENT)
GAME_ENTRY(ui, "plasma::Display.colour_index", field, CLIENT)
GAME_ENTRY(ui, "plasma::Display.colour_table", field, CLIENT)
GAME_ENTRY(ui, "std::wstring", type, BOTH)
GAME_ENTRY(ui, "std::list", type, BOTH)
GAME_ENTRY(ui, "std::list_node", type, BOTH)

/* particles (openspec add-particle-effects, D1; primitives.md, Particles): one of the game's own particle effects, pushed on the World step's
   output with the game's list push after the step ran (the controller plays the output once per frame). Cube.exe only: the push is mapped
   for it. The output's `particles` field is the list the game's own sites pass to the push. */
GAME_PRIMITIVE(particles, "Particles")
GAME_BUILDS(particles, CLIENT)
GAME_ENTRY(particles, "cube::World::update", detour, CLIENT)
GAME_ENTRY(particles, "step_output_push_particle", call, CLIENT)
GAME_ENTRY(particles, "cube::ParticleData", type, BOTH)
GAME_ENTRY(particles, "cube::StepOutput", type, BOTH)
GAME_ENTRY(particles, "cube::StepOutput.particles", field, BOTH)

/* lights (openspec add-light-api; primitives.md, Lights): point lights of mods, pushed into the frame's light vector at
   light_sort_site with the game's own push_back, before the render sorts the vector and hands it to the terrain and the models.
   Cube.exe only: Server.exe draws nothing. */
GAME_PRIMITIVE(lights, "Lights")
GAME_BUILDS(lights, CLIENT)
GAME_ENTRY(lights, "light_sort_site", site, CLIENT)
GAME_ENTRY(lights, "light_vector_push", call, CLIENT)
GAME_ENTRY(lights, "cube::Light", type, BOTH)

/* creature flags (openspec add-dragon-boss, D3; primitives.md, Creature flags): KY_CREATURE_UNSTOPPABLE and KY_CREATURE_INVULNERABLE,
   obeyed by mid hooks on applyHit's effect sites and on the two damage sites. The sites are mapped for Cube.exe only (3.2; 6.1 maps
   Server.exe's twins). creature_set_flags needs `entity_state` and `handles` too (GameCall checks the handle). */
GAME_PRIMITIVE(creature_flags, "Creature flags")
GAME_BUILDS(creature_flags, CLIENT)
GAME_ENTRY(creature_flags, "cube::World::hitPushSite", site, CLIENT)
GAME_ENTRY(creature_flags, "cube::World::hitVelocityResetSite", site, CLIENT)
GAME_ENTRY(creature_flags, "cube::World::hitVelocityResetEndSite", site, CLIENT)
GAME_ENTRY(creature_flags, "cube::World::hitSlowSite", site, CLIENT)
GAME_ENTRY(creature_flags, "cube::World::hitStunSite", site, CLIENT)
GAME_ENTRY(creature_flags, "cube::World::hitListSite", site, CLIENT)
GAME_ENTRY(creature_flags, "cube::World::hitDamageSite", site, CLIENT)

/* terrain (openspec add-skills-unleashed-multiplayer, D9; primitives.md, Terrain): world_block walks the World's region slots, the
   region's zone slots and the zone's block columns with these offsets, the same on both builds. */
GAME_PRIMITIVE(terrain, "Terrain")
GAME_ENTRY(terrain, "cube::World.regions", field, BOTH)
GAME_ENTRY(terrain, "cube::Region", type, BOTH)
GAME_ENTRY(terrain, "cube::Region.zones", field, BOTH)
GAME_ENTRY(terrain, "cube::Zone.fields", field, BOTH)
GAME_ENTRY(terrain, "cube::Field", type, BOTH)
GAME_ENTRY(terrain, "cube::Field.base_z", field, BOTH)
GAME_ENTRY(terrain, "cube::Field.blocks", field, BOTH)
GAME_ENTRY(terrain, "cube::Field.size", field, BOTH)
GAME_ENTRY(terrain, "cube::Block", type, BOTH)
GAME_ENTRY(terrain, "cube::Block.type", field, BOTH)

/* world step (openspec add-skills-unleashed-multiplayer, D11; primitives.md, World step): sim.world_step from a mid hook at
   worldStepSite, whose frame holds the step's output and changed-zone set, and creature_hit through hitCreature, with the stun and
   the direction written into its cube::Hit at hitListSite. */
GAME_PRIMITIVE(world_step, "World step")
GAME_ENTRY(world_step, "cube::World::worldStepSite", site, BOTH)
GAME_ENTRY(world_step, "cube::World::hitCreature", call, BOTH)
GAME_ENTRY(world_step, "cube::World::hitListSite", site, BOTH)
GAME_ENTRY(world_step, "cube::Hit", type, BOTH)
GAME_ENTRY(world_step, "cube::Hit.stun_duration", field, BOTH)
GAME_ENTRY(world_step, "cube::Hit.hit_dir", field, BOTH)

/* creature root and state (openspec add-skills-unleashed-multiplayer 10.3, D12; primitives.md, Creature root): KY_CREATURE_ROOTED,
   obeyed at the AI tick (the behaviour timer is held at 0 and the mode at none) and at the move step (the step and the knockback are
   zeroed), on both builds; creature_state_get and creature_state_set read and write the movement and combat fields. */
GAME_PRIMITIVE(creature_root, "Creature root")
GAME_ENTRY(creature_root, "cube::World::aiTickSite", site, BOTH)
GAME_ENTRY(creature_root, "cube::World::moveStepSite", site, BOTH)
GAME_ENTRY(creature_root, "cube::Creature.behavior_time", field, BOTH)
GAME_ENTRY(creature_root, "cube::CreatureData.velocity", field, BOTH)
GAME_ENTRY(creature_root, "cube::CreatureData.accel", field, BOTH)
GAME_ENTRY(creature_root, "cube::CreatureData.extra_vel", field, BOTH)
GAME_ENTRY(creature_root, "cube::CreatureData.current_mode", field, BOTH)
GAME_ENTRY(creature_root, "cube::CreatureData.mode_start_time", field, BOTH)
GAME_PRIMITIVE(creature_state, "Creature state")
GAME_ENTRY(creature_state, "cube::CreatureData.pos", field, BOTH)
GAME_ENTRY(creature_state, "cube::CreatureData.velocity", field, BOTH)
GAME_ENTRY(creature_state, "cube::CreatureData.extra_vel", field, BOTH)
GAME_ENTRY(creature_state, "cube::CreatureData.current_mode", field, BOTH)
GAME_ENTRY(creature_state, "cube::CreatureData.mode_start_time", field, BOTH)
GAME_ENTRY(creature_state, "cube::CreatureData.stun_time", field, BOTH)
GAME_ENTRY(creature_state, "cube::CreatureData.slowed_time", field, BOTH)
GAME_ENTRY(creature_state, "cube::CreatureData.charged_mp", field, BOTH)
GAME_ENTRY(creature_state, "cube::CreatureData.ray_hit", field, BOTH)
GAME_ENTRY(creature_state, "cube::CreatureData.appearance", field, BOTH)
GAME_ENTRY(creature_state, "cube::AppearanceData.scale", field, BOTH)

/* net transport (openspec add-skills-unleashed-multiplayer 10.4, D10; primitives.md, Network transport): a channel message to the
   other end of a joined game is one Kyubu packet on the game's connection, read at the end of each receive loop's packet switch and
   written by the connection's send thread between two game packets. */
GAME_PRIMITIVE(net_transport, "Network transport")
GAME_ENTRY(net_transport, "Net::clientReceiveNextSite", site, CLIENT)
GAME_ENTRY(net_transport, "Net::clientSendPassSite", site, CLIENT)
GAME_ENTRY(net_transport, "cube::GameController.socket", field, CLIENT)
GAME_ENTRY(net_transport, "Net::connectionReceiveNextSite", site, SERVER)
GAME_ENTRY(net_transport, "connection_send_pass_site", site, SERVER)
GAME_ENTRY(net_transport, "cube::Connection", type, SERVER)
GAME_ENTRY(net_transport, "cube::Connection.socket", field, SERVER)
GAME_ENTRY(net_transport, "cube::Connection.creature_id", field, SERVER)

/* creature bodies (openspec add-dragon-boss, D2; primitives.md, Creature bodies): model pieces on a rig drawn in place of a creature's own
   parts. Cube.exe only (the draw sites and the particle call are mapped for it). The creature_draw_site_b and _c hooks are
   installed when their addresses resolve (candidates, not reached in a probe yet). */
GAME_PRIMITIVE(creature_body, "Creature bodies")
GAME_BUILDS(creature_body, CLIENT)
GAME_ENTRY(creature_body, "creature_draw_site", site, CLIENT)
GAME_ENTRY(creature_body, "draw_creature_end_site", site, CLIENT)
GAME_ENTRY(creature_body, "creature_death_burst_site", site, CLIENT)
GAME_ENTRY(creature_body, "Model::draw", call, CLIENT)
GAME_ENTRY(creature_body, "cube::CubeShader::setColour", call, CLIENT)
GAME_ENTRY(creature_body, "cube::CubeShader::setMatrices", call, CLIENT)
GAME_ENTRY(creature_body, "cube::GameController::createParticles", call, CLIENT)

/* player shots (openspec add-skills-unleashed-multiplayer 10.9, D16; primitives.md, Player shots): Server.exe copies each projectile
   packet a joined client sends, at the receive thread's site before the game queues it for the relay, for a mod to take at the next
   World step; a mod may push a projectile of its own into the World's list, which the World's projectile step then flies. */
GAME_PRIMITIVE(player_shots, "Player shots")
GAME_BUILDS(player_shots, SERVER)
GAME_ENTRY(player_shots, "Net::receiveProjectileSite", site, SERVER)
GAME_ENTRY(player_shots, "std_list_projectile_push_back", call, SERVER)
GAME_ENTRY(player_shots, "cube::Projectile", type, BOTH)
GAME_ENTRY(player_shots, "cube::World.projectiles", field, BOTH)
GAME_ENTRY(player_shots, "cube::Connection.creature_id", field, SERVER)

/* server hits (openspec add-skills-unleashed-multiplayer; primitives.md, Server hits): Server.exe pushes each of hitCreature's hits on
   a player twice (at hitListSite, then in the applyHit call that follows), so a joined player lost twice the damage. A mid hook at
   hitListSite notes the Hit, and the applyHit detour drops the second push when it is that Hit and the target is a player. No API
   function: the fix is on whenever the primitive is available. */
GAME_PRIMITIVE(server_hits, "Server hits")
GAME_BUILDS(server_hits, SERVER)
GAME_ENTRY(server_hits, "cube::World::hitListSite", site, SERVER)
GAME_ENTRY(server_hits, "cube::World::applyHit", detour, SERVER)
GAME_ENTRY(server_hits, "cube::Hit", type, BOTH)
GAME_ENTRY(server_hits, "cube::Hit.target_id", field, BOTH)
GAME_ENTRY(server_hits, "cube::Creature.data", field, BOTH)
GAME_ENTRY(server_hits, "cube::CreatureData.hostile_type", field, BOTH)

GAME_API(game_available, none)
GAME_API(game_enum, none)
GAME_API(creature_get, entity_state)
GAME_API(creature_player, entity_state)
GAME_API(creature_spawn, spawn)
GAME_API(creature_find, entity_state)
GAME_API(creature_teleport, entity_state)
GAME_API(creature_set_yaw, entity_state)
GAME_API(creature_give_item, items)
GAME_API(creature_set_flags, creature_root)
GAME_API(body_define, creature_body)
GAME_API(body_name, creature_body)
GAME_API(creature_body_attach, creature_body)
GAME_API(creature_body_play, creature_body)
GAME_API(creature_body_offset, creature_body)
GAME_API(creature_body_weight, creature_body)
GAME_API(creature_body_read, creature_body)
GAME_API(creature_body_point, creature_body)
GAME_API(ability_get, ability_state)
GAME_API(ability_cooldown, ability_state)
GAME_API(ability_cooldown_set, ability_state)
GAME_API(ability_start, ability_use)
GAME_API(ability_end, ability_state)
GAME_API(ability_rank_get, ability_ranks)
GAME_API(ability_rank_set, ability_ranks)
GAME_API(animation_define_from_text, none)
GAME_API(animation_preview, none)
GAME_API(animation_preview_clear, none)
GAME_API(ability_animation_bind, none)
GAME_API(ability_animation_bind_for, none)
GAME_API(ui_node_find, ui)
GAME_API(ui_node_get, ui)
GAME_API(ui_node_child, ui)
GAME_API(render_get, ui)
GAME_API(particle_spawn, particles)
GAME_API(light_spawn, lights)
GAME_API(light_set, lights)
GAME_API(light_remove, lights)
GAME_API(world_block, terrain)
GAME_API(creature_hit, world_step)
GAME_API(creature_state_get, creature_state)
GAME_API(creature_state_set, creature_state)
GAME_API(shot_take, player_shots)
GAME_API(projectile_push, player_shots)

#undef GAME_PRIMITIVE
#undef GAME_BUILDS
#undef GAME_ENTRY
#undef GAME_API
