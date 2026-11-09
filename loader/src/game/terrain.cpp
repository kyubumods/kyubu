// The terrain primitive: world_block (openspec add-skills-unleashed-multiplayer, D9; docs/game-api.md, world_block).
//
// cube::World::getBlock is a function on both builds, but it answers a static solid block for a place in no loaded zone, so a
// rule cannot tell "unknown" from "rock". world_block walks the same path itself with the mapped offsets: the World's region
// slots, the region's zone slots, the zone's block columns, the column's blocks. A zone enters its region slot only once the
// zone thread has built it, and the thread clears a zone's or a region's slot while it holds cube::World.lock, before it deletes
// the object. GameCall holds that lock, so the walk sees a live object or null.
#include "terrain.h"

#include <cstddef>

#include "call.h"
#include "cubeworld/types.h"
#include "game.h"

namespace kyubu::game {

namespace {

constexpr int32_t kRegions = 1024;           // region slots per axis
constexpr int32_t kZonesPerRegion = 64;      // zone slots per axis in a region
constexpr int32_t kBlocksPerZone = 256;      // block columns per axis in a zone
constexpr int32_t kLimit = kRegions * kZonesPerRegion * kBlocksPerZone;   // 0x1000000 blocks per axis
constexpr uint8_t kKindMask = 0x1F;
constexpr uint8_t kInterior = 0x40;
constexpr uint8_t kSolid = 1, kWater = 2;

void set(KyubuBlock* out, uint8_t grey, uint8_t type) {
    out->color[0] = out->color[1] = out->color[2] = grey;
    out->type = type;
}

KyubuResult KYUBU_CALL k_world_block(KyubuModHandle self, int32_t x, int32_t y, int32_t z, KyubuBlock* out) {
    GameCall g(self, Prim::terrain, "world_block");
    if (!g) return g.result();
    if (!out) return g.refuse(KY_E_INVALID_ARG, "out is NULL");
    return terrain_block(g.world(), x, y, z, out);
}

}  // namespace

KyubuResult terrain_block(const cube_World* w, int32_t x, int32_t y, int32_t z, KyubuBlock* out) {
    if (!w || !out || x < 0 || y < 0 || x >= kLimit || y >= kLimit) return KY_E_NOT_FOUND;
    const int32_t zx = x / kBlocksPerZone, zy = y / kBlocksPerZone;
    const cube_Region* region = w->regions[(zx / kZonesPerRegion) * kRegions + zy / kZonesPerRegion];
    if (!region) return KY_E_NOT_FOUND;
    const cube_Zone* zone = region->zones[(zx % kZonesPerRegion) * kZonesPerRegion + zy % kZonesPerRegion];
    if (!zone || !zone->fields) return KY_E_NOT_FOUND;
    const cube_Field& f = zone->fields[(y % kBlocksPerZone) * kBlocksPerZone + x % kBlocksPerZone];
    // The game's static blocks: solid grey 200 below the column, white water or air above it.
    if (z < f.base_z) {
        set(out, 200, kSolid);
        return KY_OK;
    }
    if (z - f.base_z >= f.size || !f.blocks) {
        set(out, 255, z <= 0 ? kWater : 0);
        return KY_OK;
    }
    const cube_Block& b = f.blocks[z - f.base_z];
    if ((b.type & kKindMask) == 0 && z <= 0 && !(b.type & kInterior)) {
        set(out, 255, kWater);
        return KY_OK;
    }
    out->color[0] = b.color[0];
    out->color[1] = b.color[1];
    out->color[2] = b.color[2];
    out->type = b.type;
    return KY_OK;
}

void fill_terrain(KyubuApi& a) { a.world_block = k_world_block; }

}  // namespace kyubu::game
