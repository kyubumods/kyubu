// hit_skip_proto: probe hit-skip-sites (task 2.3 of add-dragon-boss, M3). Question: can a mod cancel what cube::World::applyHit
// does to a target besides the damage (push, velocity reset, slow/lock timer, stun) with mid hooks that edit registers/state?
//  - draw_creature_end_site: the creature with hp >= 10000 is the player (the script boosts it); the first other creature with
//    0 < hp < 10000 is the target: until the first hit it is parked 1.5 blocks in front of the player (+y); after it the mod logs
//    its pos, velocity, extra_vel, stun and slow for a few frames.
//  - hitListSite (hitCreature, Hit at ESP+0xC4): forces Hit.stun = Hit.slow = 3000 and a push (0, 4, 6) so every hit has them.
//  - the five applyHit sites (flag = 1 only): see the comments below. A mid hook cannot skip code: each is neutralised by a register.
#include <cmath>
#include <cstdint>
#include <cstring>

#include "kyubu.hpp"

namespace {

kyubu::Mod* g_mod = nullptr;
bool g_flag = false;
uintptr_t g_player = 0, g_tgt[2] = {0, 0};
int64_t g_id[2] = {0, 0};  // creature ids: the game has more than one copy of a creature (a client and a server World)
int idx_of(uintptr_t c) { if (!c) return -1; const int64_t id = *reinterpret_cast<const int64_t*>(c + 8); return id && id == g_id[0] ? 0 : (id && id == g_id[1] ? 1 : -1); }
int g_cur = 0;  // the target in front of the player: 0 is the flagged one (when flag = 1), 1 the control
int g_hits = 0, g_after = 0, g_calls = 0;
struct Saved { bool valid = false; float v[3]; } g_saved;

template <class T> T rd(uintptr_t a) { T v{}; std::memcpy(&v, reinterpret_cast<void*>(a), sizeof v); return v; }
template <class T> void wr(uintptr_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v); }

void state(const char* what, uintptr_t c) {
    const int64_t* p = reinterpret_cast<const int64_t*>(c + 0x10);
    g_mod->info("hit_skip_proto: %s pos %.3f %.3f %.3f vel %.2f %.2f %.2f extra %.2f %.2f %.2f stun %d slow %d hp %.1f", what,
                p[0] / 65536.0, p[1] / 65536.0, p[2] / 65536.0, rd<float>(c + 0x34), rd<float>(c + 0x38), rd<float>(c + 0x3C),
                rd<float>(c + 0x4C), rd<float>(c + 0x50), rd<float>(c + 0x54), (int)rd<int32_t>(c + 0x12C),
                (int)rd<int32_t>(c + 0x130), rd<float>(c + 0x16C));
}

void end_site(KyubuCpuContext* ctx, void*) {
    ++g_calls;
    const uintptr_t c = rd<uint32_t>(ctx->edi);
    if (!c) return;
    const float hp = rd<float>(c + 0x16C);
    if (hp >= 10000.0f) { g_player = c; return; }
    if (!g_player || hp <= 0.0f) return;
    { const uint8_t h = rd<uint8_t>(c + 0x60); if (h == 0 || h == 6) return; }  // hostile_type: not the player, not a TARGET (bush, dummy)
    if (c != g_tgt[0] && c != g_tgt[1]) {
        const int k = g_tgt[0] ? 1 : 0;
        if (g_tgt[k]) return;
        g_tgt[k] = c;
        g_id[k] = rd<int64_t>(c + 8);
        g_mod->info("hit_skip_proto: target %d = %08x, player %08x, flag %d", k, (unsigned)c, (unsigned)g_player, (int)g_flag);
        return;
    }
    const int mine_k = c == g_tgt[0] ? 0 : 1;
    int64_t* tp = reinterpret_cast<int64_t*>(c + 0x10);
    const int64_t* pp = reinterpret_cast<const int64_t*>(g_player + 0x10);
    if (!g_tgt[1]) return;  // wait for both
    if (mine_k != g_cur) {  // the other one waits 40 blocks away
        tp[0] = pp[0]; tp[1] = pp[1] + int64_t(40 * 65536); tp[2] = pp[2];
        return;
    }
    if (g_after < 8) {
        if (g_hits == 0) {  // park the target 1.5 blocks in front of the player (the aim of the replay character is +y)
            tp[0] = pp[0]; tp[1] = pp[1] + int64_t(1.5 * 65536); tp[2] = pp[2];
            wr<float>(c + 0x34, 0); wr<float>(c + 0x38, 0); wr<float>(c + 0x3C, 0);
            static int n = 0;
            if (++n % 120 == 1) state("parked", c);
        } else {
            state("after", c);
            ++g_after;
        }
    } else if (g_cur == 0) {
        g_cur = 1; g_hits = 0; g_after = 0;
        g_mod->info("hit_skip_proto: now the control target");
    }
}

// hitCreature, before it copies the Hit into the output: the Hit is at ESP+0xC4
void list_site(KyubuCpuContext* ctx, void*) {
    const uintptr_t hit = ctx->esp + 0xC4;
    if (rd<int64_t>(hit + 8) == 0) return;
    wr<int32_t>(hit + 0x18, 3000);
    wr<int32_t>(hit + 0x1C, 3000);
    wr<float>(hit + 0x38, 0.0f); wr<float>(hit + 0x3C, 4.0f); wr<float>(hit + 0x40, 6.0f);
}

bool mine(uintptr_t target) { return g_flag && idx_of(target) == 0; }

// 0x595E27: movss [esi+4C],xmm5; [esi+50],xmm6; [esi+54],xmm7 -> make the stored values the current ones
void push_site(KyubuCpuContext* ctx, void*) {
    if (idx_of(ctx->esi) >= 0)
        g_mod->info("hit_skip_proto: push site, copy %08x of target %d, push %.2f %.2f %.2f, old extra %.2f %.2f %.2f, hostility %u", (unsigned)ctx->esi, idx_of(ctx->esi),
                    ctx->xmm[5].f32[0], ctx->xmm[6].f32[0], ctx->xmm[7].f32[0], rd<float>(ctx->esi + 0x4C), rd<float>(ctx->esi + 0x50),
                    rd<float>(ctx->esi + 0x54), (unsigned)rd<uint8_t>(ctx->esi + 0x60));
    if (!mine(ctx->esi)) return;
    ctx->xmm[5].f32[0] = rd<float>(ctx->esi + 0x4C);
    ctx->xmm[6].f32[0] = rd<float>(ctx->esi + 0x50);
    ctx->xmm[7].f32[0] = rd<float>(ctx->esi + 0x54);
}
// 0x595E3C: velocity reset (hit.stun > 0): save; 0x595E51 (the join): restore
void vel_site(KyubuCpuContext* ctx, void*) {
    if (idx_of(ctx->esi) != g_cur) return;
    if (ctx->esi == g_tgt[g_cur]) ++g_hits;
    g_mod->info("hit_skip_proto: hit %d (target %d) reached the velocity reset (copy %08x ebx %08x)", g_hits, g_cur, (unsigned)ctx->esi, (unsigned)ctx->ebx);
    state("before", ctx->esi);
    g_mod->info("hit_skip_proto: hostility %u, hit dir %.2f %.2f %.2f, hit type %u, hit damage %.2f, stun %d", (unsigned)rd<uint8_t>(ctx->esi + 0x60),
                rd<float>(ctx->ebx + 0x38), rd<float>(ctx->ebx + 0x3C), rd<float>(ctx->ebx + 0x40), (unsigned)rd<uint8_t>(ctx->ebx + 0x45),
                rd<float>(ctx->ebx + 0x10), (int)rd<int32_t>(ctx->ebx + 0x18));
    if (!mine(ctx->esi)) return;
    g_saved.valid = true;
    for (int i = 0; i < 3; ++i) g_saved.v[i] = rd<float>(ctx->esi + 0x34 + 4 * i);
}
void vel_end_site(KyubuCpuContext* ctx, void*) {
    if (!g_saved.valid) return;
    g_saved.valid = false;
    if (!mine(ctx->esi)) return;
    for (int i = 0; i < 3; ++i) wr<float>(ctx->esi + 0x34 + 4 * i, g_saved.v[i]);
}
// 0x595E58 / 0x595EC1: cmp eax,ecx; cmovg; mov [esi+off],ecx -> ecx = the current value
void slow_site(KyubuCpuContext* ctx, void*) { if (idx_of(ctx->esi) >= 0) g_mod->info("hit_skip_proto: slow site, copy %08x of target %d", (unsigned)ctx->esi, idx_of(ctx->esi)); if (mine(ctx->esi)) ctx->ecx = rd<uint32_t>(ctx->esi + 0x130); }
void stun_site(KyubuCpuContext* ctx, void*) { if (idx_of(ctx->esi) >= 0) g_mod->info("hit_skip_proto: stun site, copy %08x of target %d", (unsigned)ctx->esi, idx_of(ctx->esi)); if (mine(ctx->esi)) ctx->ecx = rd<uint32_t>(ctx->esi + 0x12C); }

}  // namespace

struct HitSkipProto {
    explicit HitSkipProto(kyubu::Mod& m) : mod(m) {
        if (m.side() != KY_SIDE_CLIENT) return;
        g_mod = &m;
        g_flag = m.config_float("flag", 1) != 0;
        struct Site { const char* name; KyubuMidHookFn fn; } sites[] = {
            {"draw_creature_end_site", &end_site},         {"cube::World::hitListSite", &list_site},
            {"cube::World::hitPushSite", &push_site},      {"cube::World::hitVelocityResetSite", &vel_site},
            {"cube::World::hitVelocityResetEndSite", &vel_end_site}, {"cube::World::hitSlowSite", &slow_site},
            {"cube::World::hitStunSite", &stun_site},
        };
        int ok = 0;
        for (const Site& s : sites) {
            const uintptr_t a = m.symbol(s.name);
            KyubuId id = 0;
            if (!a) { m.warn("hit_skip_proto: no symbol %s", s.name); continue; }
            if (m.api()->hook_mid(m.handle(), a, s.fn, nullptr, &id) != KY_OK) m.warn("hit_skip_proto: hook_mid %s failed", s.name);
            else ++ok;
        }
        m.info("hit_skip_proto: %d of 7 sites hooked, flag %d", ok, (int)g_flag);
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("hit_skip_proto", KY_SIDE_CLIENT, HitSkipProto)
