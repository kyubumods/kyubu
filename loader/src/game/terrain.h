// The terrain primitive (openspec add-skills-unleashed-multiplayer, D9): world_block, the block at a point, on both builds.
#pragma once
#include "common.h"

struct cube_World;

namespace kyubu::game {

// The walk itself, with no checks of the mod or the thread: World, region, zone, column, block, with the mapped offsets
// (cube::World.regions, cube::Region.zones, cube::Zone.fields, cube::Field). The caller holds cube::World.lock. KY_OK with the
// block in `out`, or KY_E_NOT_FOUND when the point is in no loaded zone (out of range included). Under the column, the game's
// solid block; above it, its water block at z 0 and below, else its air block; inside it, the stored block, read as water at z 0
// and below when it is air and not inside a building (cube::World::getBlock's rules).
KyubuResult terrain_block(const cube_World* w, int32_t x, int32_t y, int32_t z, KyubuBlock* out);

void fill_terrain(KyubuApi& a);  // terrain.cpp: world_block

}  // namespace kyubu::game
