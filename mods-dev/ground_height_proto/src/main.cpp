// ground_height_proto: probe world-ground-height (task 2.5 of add-dragon-boss, M5). Question: does cube::World::getBlock (0x42F860) give the
// terrain, and does a scan down from above find the ground top?
//  - draw_creature_end_site: the creature with hp >= 10000 is the player (the script boosts it). Once per frame (at the player) the mod takes
//    9 points on a line in +y, 2 blocks apart (point 0 is the player's own column), and for each scans down from the player's block z + 12 until
//    getBlock gives a solid block (kind, byte +3 & 0x1F, not 0 air and not 2 water). Ground top = that block's z + 1.
//  - It logs the heights (point 0 beside the player's feet = player z - size.z/2) every 120 frames and puts a red particle marker at each top.
//  - World = [game_controller 0x76B1C8] + 0x2E4 (the this of the game's own getBlock calls, asm 0x431b86).
#include <cstdint>
#include <cstring>

#include "kyubu.hpp"

namespace {

using GetBlockFn = const uint8_t*(__attribute__((fastcall)) *)(void* world, void* edx, int64_t x, int64_t y, int64_t z, int32_t flag);

kyubu::Mod* g_mod = nullptr;
GetBlockFn g_get = nullptr;
int g_frame = 0;
uintptr_t g_ctrl_addr = 0;  // the relocated address of cube::game_controller

template <class T> T rd(uintptr_t a) { T v{}; std::memcpy(&v, reinterpret_cast<void*>(a), sizeof v); return v; }

int64_t floor_block(int64_t u) { return u >= 0 ? u / 65536 : -((-u + 65535) / 65536); }

// The top (blocks) of the first solid block under the start point, or -9999 when none within 40 blocks. kind_out: the kind found.
int scan(void* world, int64_t x, int64_t y, int64_t zstart_block, int* kind_out) {
    for (int i = 0; i < 40; ++i) {
        const int64_t zb = zstart_block - i;
        const uint8_t* b = g_get(world, nullptr, x, y, zb * 65536 + 100, 0);
        const int kind = b ? (b[3] & 0x1F) : -1;
        if (kind != 0 && kind != 2 && kind != -1) { *kind_out = kind; return int(zb) + 1; }
        if (kind == 2 && i == 0) { *kind_out = 2; }
    }
    return -9999;
}

void end_site(KyubuCpuContext* ctx, void*) {
    const uintptr_t c = rd<uint32_t>(ctx->edi);
    if (!c || rd<float>(c + 0x16C) < 10000.0f) return;
    const uintptr_t ctrl = rd<uint32_t>(g_ctrl_addr);
    if (!ctrl || !g_get) return;
    void* world = reinterpret_cast<void*>(ctrl + 0x2E4);
    const int64_t px = rd<int64_t>(c + 0x10), py = rd<int64_t>(c + 0x18), pz = rd<int64_t>(c + 0x20);
    const int64_t zs = floor_block(pz) + 12;
    ++g_frame;
    const bool log = g_frame % 120 == 1;
    char line[512] = {0};
    int off = 0;
    for (int i = 0; i < 9; ++i) {
        const int64_t y = py + int64_t(i) * 2 * 65536;
        int kind = -1;
        const int top = scan(world, px, y, zs, &kind);
        if (log) off += snprintf(line + off, sizeof line - off, " %d(k%d)", top, kind);
        if (top > -9000 && g_frame % 20 == 1) {
            KyubuParticle p{};
            p.type = KY_PARTICLE_NO_GRAVITY;
            p.pos[0] = px; p.pos[1] = y; p.pos[2] = int64_t(top) * 65536 + 20000;
            p.color[0] = 1.0f; p.color[1] = 0.0f; p.color[2] = 0.0f; p.color[3] = 1.0f;
            p.scale = 0.35f; p.count = 4; p.spreading = 0.0f;
            g_mod->particle_spawn(p);
        }
    }
    if (log) {
        char col[256] = {0};
        int o2 = 0;
        for (int dz = 14; dz >= -4; --dz) {
            const uint8_t* b = g_get(world, nullptr, px, py, (floor_block(pz) + dz) * 65536 + 100, 0);
            o2 += snprintf(col + o2, sizeof col - o2, " %+d:%d/%02x", dz, b ? (b[3] & 0x1F) : -1, b ? b[3] : 0);
        }
        g_mod->info("ground_height_proto: player column (dz:kind/byte3)%s", col);
    }
    if (log)
        g_mod->info("ground_height_proto: player z %.2f size.z %.2f feet %.2f; tops at +0..+16 y:%s", double(pz) / 65536.0,
                    rd<float>(c + 0x88), double(pz) / 65536.0 - rd<float>(c + 0x88) * 0.5, line);
}

}  // namespace

struct GroundHeightProto {
    explicit GroundHeightProto(kyubu::Mod& m) : mod(m) {
        if (m.side() != KY_SIDE_CLIENT) return;
        g_mod = &m;
        const uintptr_t gb = m.symbol("cube::World::getBlock");
        g_get = reinterpret_cast<GetBlockFn>(gb);
        g_ctrl_addr = m.symbol("cube::game_controller");
        const uintptr_t a = m.symbol("draw_creature_end_site");
        KyubuId id = 0;
        if (!gb || !a || !g_ctrl_addr || m.api()->hook_mid(m.handle(), a, &end_site, nullptr, &id) != KY_OK) m.warn("ground_height_proto: setup failed");
        else m.info("ground_height_proto: hooked, getBlock %08x", (unsigned)gb);
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("ground_height_proto", KY_SIDE_CLIENT, GroundHeightProto)
