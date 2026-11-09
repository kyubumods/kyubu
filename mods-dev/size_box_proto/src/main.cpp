// size_box_proto: probe appearance-size-hit (task 2.4 of add-dragon-boss, M4). Question: is appearance.size (Creature+0x80, three floats,
// blocks) the box the game's melee test uses, so that a creature with a wide size is hit from farther away than one with the default?
//  - draw_creature_end_site: the creature with hp >= 10000 is the player (the script boosts it). The first two other hostile creatures with
//    0 < hp < 10000 are the targets: 0 gets size (big, big, tall) written every frame, 1 keeps its own size. Both are parked at the same
//    spot, `dist` blocks in front of the player (+y), so one swing tests both boxes.
//  - the mod logs both sizes once and both hp values every 60 frames.
#include <cstdint>
#include <cstring>

#include "kyubu.hpp"

namespace {

kyubu::Mod* g_mod = nullptr;
uintptr_t g_player = 0, g_tgt[2] = {0, 0};
float g_dist = 4.0f, g_big = 8.0f, g_tall = 12.0f;
int g_frame = 0;

template <class T> T rd(uintptr_t a) { T v{}; std::memcpy(&v, reinterpret_cast<void*>(a), sizeof v); return v; }
template <class T> void wr(uintptr_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v); }

void end_site(KyubuCpuContext* ctx, void*) {
    const uintptr_t c = rd<uint32_t>(ctx->edi);
    if (!c) return;
    const float hp = rd<float>(c + 0x16C);
    if (hp >= 10000.0f) { g_player = c; return; }
    if (!g_player || hp <= 0.0f) return;
    { const uint8_t h = rd<uint8_t>(c + 0x60); if (h == 0 || h == 6) return; }
    int k = c == g_tgt[0] ? 0 : (c == g_tgt[1] ? 1 : -1);
    if (k < 0) {
        const int slot = g_tgt[0] ? 1 : 0;
        if (g_tgt[slot]) return;
        g_tgt[slot] = c;
        g_mod->info("size_box_proto: target %d = %08x, default size %.2f %.2f %.2f, hp %.1f", slot, (unsigned)c, rd<float>(c + 0x80),
                    rd<float>(c + 0x84), rd<float>(c + 0x88), hp);
        return;
    }
    if (!g_tgt[1]) return;  // wait for both
    int64_t* tp = reinterpret_cast<int64_t*>(c + 0x10);
    const int64_t* pp = reinterpret_cast<const int64_t*>(g_player + 0x10);
    tp[0] = pp[0]; tp[1] = pp[1] + int64_t(double(g_dist) * 65536.0); tp[2] = pp[2];
    wr<float>(c + 0x34, 0); wr<float>(c + 0x38, 0); wr<float>(c + 0x3C, 0);
    if (k == 0) { wr<float>(c + 0x80, g_big); wr<float>(c + 0x84, g_big); wr<float>(c + 0x88, g_tall); }
    if (k == 1 && ++g_frame % 60 == 1)
        g_mod->info("size_box_proto: dist %.1f hp big %.2f control %.2f, sizes big %.2f %.2f %.2f control %.2f %.2f %.2f", g_dist,
                    rd<float>(g_tgt[0] + 0x16C), hp, rd<float>(g_tgt[0] + 0x80), rd<float>(g_tgt[0] + 0x84), rd<float>(g_tgt[0] + 0x88),
                    rd<float>(c + 0x80), rd<float>(c + 0x84), rd<float>(c + 0x88));
}

}  // namespace

struct SizeBoxProto {
    explicit SizeBoxProto(kyubu::Mod& m) : mod(m) {
        if (m.side() != KY_SIDE_CLIENT) return;
        g_mod = &m;
        g_dist = float(m.config_float("dist", 4.0));
        g_big = float(m.config_float("big", 8.0));
        g_tall = float(m.config_float("tall", 12.0));
        const uintptr_t a = m.symbol("draw_creature_end_site");
        KyubuId id = 0;
        if (!a || m.api()->hook_mid(m.handle(), a, &end_site, nullptr, &id) != KY_OK) m.warn("size_box_proto: hook failed");
        else m.info("size_box_proto: hooked, dist %.1f big %.1f tall %.1f", g_dist, g_big, g_tall);
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("size_box_proto", KY_SIDE_CLIENT, SizeBoxProto)
