// The world_step primitive (openspec add-skills-unleashed-multiplayer 10.2, design D11): the sim.world_step event, once per World step
// on both builds, and creature_hit, a hit through cube::World::hitCreature with the stun and the knock direction a mod asks for.
#pragma once
#include "common.h"

struct cube_World;

namespace kyubu::game {

// The mid hook on cube::World::worldStepSite: reads the step's World, output, changed-zone set and dt from the frame (EBP), keeps
// them as the current step and raises sim.world_step on each side whose World this is.
void step_on_world_site(KyubuCpuContext* ctx);
// The mid hook on cube::World::hitListSite: during creature_hit's own call only, writes the mod's stun and direction into the
// cube::Hit that hitCreature is about to push and apply.
void step_on_list_site(KyubuCpuContext* ctx);
// cube::World::update returned (world.cpp): the step's pointers are gone.
void world_step_end(cube_World* w);
// The World of the step being run (from worldStepSite to the end of cube::World::update), or null. The game thread only.
cube_World* step_world();

void install_world_step_hooks();   // boot, when world_step is available; says why in the log when not
void remove_world_step_hooks();    // tests

void fill_world_step(KyubuApi& a);  // creature_hit

}  // namespace kyubu::game
