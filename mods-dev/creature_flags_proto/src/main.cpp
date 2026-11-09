// creature_flags_proto: probe creature-flags (task 3.2 of add-dragon-boss). The same arrangement as hit_skip_proto, but the first of two
// hostile slimes is flagged with the loader's creature_set_flags (server side handle) instead of the mod's own hooks. The mod forces
// Hit.stun = Hit.slow = 3000 and a push (0, 4, 6) at hitListSite, so every hit that is not neutralised by the loader shows.
#include <cstdint>
#include <cstring>

#include "kyubu.hpp"

namespace {

kyubu::Mod* g_client = nullptr;
kyubu::Mod* g_server = nullptr;
uint32_t g_flags = 3;
uintptr_t g_player = 0, g_tgt[2] = {0, 0};
int g_cur = 0, g_hits = 0, g_after = 0;
bool g_set = false;

template <class T> T rd(uintptr_t a) { T v{}; std::memcpy(&v, reinterpret_cast<void*>(a), sizeof v); return v; }
template <class T> void wr(uintptr_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v); }

void state(const char* what, uintptr_t c) {
    const int64_t* p = reinterpret_cast<const int64_t*>(c + 0x10);
    g_client->info("creature_flags_proto: %s pos %.3f %.3f %.3f vel %.2f %.2f %.2f extra %.2f %.2f %.2f stun %d slow %d hp %.1f", what,
                   p[0] / 65536.0, p[1] / 65536.0, p[2] / 65536.0, rd<float>(c + 0x34), rd<float>(c + 0x38), rd<float>(c + 0x3C),
                   rd<float>(c + 0x4C), rd<float>(c + 0x50), rd<float>(c + 0x54), (int)rd<int32_t>(c + 0x12C),
                   (int)rd<int32_t>(c + 0x130), rd<float>(c + 0x16C));
}

void flag_target(uintptr_t c) {
    KyubuCreature h = 0;
    const int64_t id = rd<int64_t>(c + 8);
    if (!g_server) { g_client->warn("creature_flags_proto: no server side instance"); return; }
    KyubuResult r = g_server->creature_find(id, h);
    if (r == KY_OK) r = g_server->creature_set_flags(h, g_flags);
    g_client->info("creature_flags_proto: creature_set_flags(id %lld, %u) -> %d", (long long)id, (unsigned)g_flags, (int)r);
}

void end_site(KyubuCpuContext* ctx, void*) {
    const uintptr_t c = rd<uint32_t>(ctx->edi);
    if (!c) return;
    const float hp = rd<float>(c + 0x16C);
    if (hp >= 10000.0f) { g_player = c; return; }
    if (!g_player || hp <= 0.0f) return;
    { const uint8_t h = rd<uint8_t>(c + 0x60); if (h == 0 || h == 6) return; }
    if (c != g_tgt[0] && c != g_tgt[1]) {
        const int k = g_tgt[0] ? 1 : 0;
        if (g_tgt[k]) return;
        g_tgt[k] = c;
        g_client->info("creature_flags_proto: target %d = %08x", k, (unsigned)c);
        if (k == 0) { flag_target(c); g_set = true; }
        return;
    }
    if (!g_tgt[1]) return;
    const int k = c == g_tgt[0] ? 0 : 1;
    int64_t* tp = reinterpret_cast<int64_t*>(c + 0x10);
    const int64_t* pp = reinterpret_cast<const int64_t*>(g_player + 0x10);
    if (k != g_cur) { tp[0] = pp[0]; tp[1] = pp[1] + int64_t(40 * 65536); tp[2] = pp[2]; return; }
    if (g_after < 8) {
        if (g_hits == 0) {
            tp[0] = pp[0]; tp[1] = pp[1] + int64_t(1.5 * 65536); tp[2] = pp[2];
            wr<float>(c + 0x34, 0); wr<float>(c + 0x38, 0); wr<float>(c + 0x3C, 0);
        } else {
            state(g_cur == 0 ? "after (flagged)" : "after (control)", c);
            ++g_after;
        }
    } else if (g_cur == 0) {
        g_cur = 1; g_hits = 0; g_after = 0;
        g_client->info("creature_flags_proto: now the control target");
    }
}

void list_site(KyubuCpuContext* ctx, void*) {
    const uintptr_t hit = ctx->esp + 0xC4;
    const int64_t target = rd<int64_t>(hit + 8);
    if (target == 0) return;
    // the hook order puts the mod's forcing before or after the loader's damage cut; the damage is logged at the damage site by hp
    wr<int32_t>(hit + 0x18, 3000);
    wr<int32_t>(hit + 0x1C, 3000);
    wr<float>(hit + 0x38, 0.0f); wr<float>(hit + 0x3C, 4.0f); wr<float>(hit + 0x40, 6.0f);
    const int64_t id0 = g_tgt[0] ? rd<int64_t>(g_tgt[0] + 8) : 0, id1 = g_tgt[1] ? rd<int64_t>(g_tgt[1] + 8) : 0;
    if (target == id0 || target == id1) {
        if (target == (g_cur == 0 ? id0 : id1)) ++g_hits;
        g_client->info("creature_flags_proto: hit %d on %s, damage %.2f at the list site", g_hits, target == id0 ? "flagged" : "control",
                       rd<float>(hit + 0x10));
    }
}

}  // namespace

struct CreatureFlagsProto {
    explicit CreatureFlagsProto(kyubu::Mod& m) {
        if (m.side() == KY_SIDE_SERVER) { g_server = &m; return; }
        g_client = &m;
        g_flags = (uint32_t)m.config_float("flags", 3);
        const uintptr_t e = m.symbol("draw_creature_end_site"), l = m.symbol("cube::World::hitListSite");
        KyubuId id = 0;
        int ok = 0;
        if (e && m.api()->hook_mid(m.handle(), e, end_site, nullptr, &id) == KY_OK) ++ok;
        if (l && m.api()->hook_mid(m.handle(), l, list_site, nullptr, &id) == KY_OK) ++ok;
        m.info("creature_flags_proto: %d of 2 sites hooked, flags %u", ok, (unsigned)g_flags);
    }
};

KYUBU_MOD("creature_flags_proto", KY_SIDE_CLIENT | KY_SIDE_SERVER, CreatureFlagsProto)
