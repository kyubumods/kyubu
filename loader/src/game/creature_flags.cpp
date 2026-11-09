// The creature_flags primitive: KY_CREATURE_UNSTOPPABLE and KY_CREATURE_INVULNERABLE (openspec add-dragon-boss, D3; docs/game-api.md,
// creature_set_flags).
//
// The flags live in the loader, keyed by the creature's game id (a Cube.exe process has one World, so an id is one creature), and end
// with the creature or the world. The game's own code is changed only through mid hooks, which cannot skip code: each effect is
// neutralised by state, as the probe hit-skip-sites measured:
//  - hitPushSite stores the knockback from XMM5-7 into extra_vel: they are set to the current extra_vel;
//  - hitVelocityResetSite zeroes the velocity of a stunning hit: it is saved there and written back at hitVelocityResetEndSite;
//  - hitSlowSite and hitStunSite raise a timer with cmp/cmovg/mov through ECX: ECX is set to the timer's own value, so the store
//    writes back what was there (the stun is also held at or below 0);
//  - hitListSite (the Hit about to be copied into the output's hit list, which the damage number is read from) and hitDamageSite (where
//    hp falls) get the Hit's damage cut to 0 for an invulnerable target.
// KY_CREATURE_ROOTED (add-skills-unleashed-multiplayer 10.3, D12) has its own primitive, creature_root, mapped on both builds:
//  - aiTickSite (a computer creature's AI tick, server copy): the behaviour timer is set to 0, so the behaviour's update is skipped
//    this step, and the mode and its clock to 0, so the creature does not act; its behaviour stays in place;
//  - moveStepSite (every creature's move of the step, after the push of the others): the step is cut to a fall (no x, y, no rise),
//    and the acceleration, the x and y velocity and the knockback are zeroed, so nothing builds up for the next step.
#include "creature_flags.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "call.h"
#include "cubeworld/functions.h"
#include "game.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

std::mutex g_mu;
std::unordered_map<uint64_t, uint32_t> g_flags;
std::atomic<unsigned> g_count{0};   // entries in g_flags: the hooks return at once when 0 (nearly always)

constexpr uint32_t kHitBits = KY_CREATURE_UNSTOPPABLE | KY_CREATURE_INVULNERABLE;
constexpr uint32_t kKnown = kHitBits | KY_CREATURE_ROOTED;
constexpr size_t kData = offsetof(cube_Creature, data);
constexpr size_t kExtraVel = kData + offsetof(cube_CreatureData, extra_vel);
constexpr size_t kVelocity = kData + offsetof(cube_CreatureData, velocity);
constexpr size_t kStun = kData + offsetof(cube_CreatureData, stun_time);
constexpr size_t kSlowed = kData + offsetof(cube_CreatureData, slowed_time);
constexpr uintptr_t kHitAtListSite = 0xC4;   // from ESP at hitListSite
constexpr size_t kAccel = kData + offsetof(cube_CreatureData, accel);
constexpr size_t kMode = kData + offsetof(cube_CreatureData, current_mode);
constexpr size_t kModeTime = kData + offsetof(cube_CreatureData, mode_start_time);
constexpr size_t kBehaviorTime = offsetof(cube_Creature, behavior_time);
constexpr uintptr_t kCreatureAtStep = 0x2B14;   // [EBP-0x2B14] the creature, at aiTickSite and moveStepSite
constexpr uintptr_t kMoveAtStep = 0x1C;         // EBP-0x1C the step (3 floats), at moveStepSite

template <class T> T rd(uintptr_t a) { T v{}; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v); return v; }
template <class T> void wr(uintptr_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v); }

uint32_t flags_of_creature(uint32_t creature_address) {
    if (!creature_address || g_count.load(std::memory_order_relaxed) == 0) return 0;
    return creature_flags_of(rd<uint64_t>(creature_address + offsetof(cube_Creature, id)));
}

struct SavedVelocity {
    bool valid = false;
    uint32_t creature = 0;
    float v[3] = {};
} g_saved;   // applyHit runs on the game thread, so one slot is enough

KyubuId g_hooks[7] = {};
KyubuId g_root_hooks[2] = {};
const char* const kSiteNames[7] = {CW_SYM_cube_World_hitPushSite,  CW_SYM_cube_World_hitVelocityResetSite,
                                   CW_SYM_cube_World_hitVelocityResetEndSite, CW_SYM_cube_World_hitSlowSite,
                                   CW_SYM_cube_World_hitStunSite,  CW_SYM_cube_World_hitListSite,
                                   CW_SYM_cube_World_hitDamageSite};
void (*const kSiteFns[7])(KyubuCpuContext*) = {flags_on_push_site, flags_on_velocity_site, flags_on_velocity_end_site,
                                               flags_on_slow_site, flags_on_stun_site,     flags_on_list_site,
                                               flags_on_damage_site};

// The hit bits need creature_flags (Cube.exe only), the root bit creature_root (both builds); 0 clears with whichever is there.
Prim prim_for(uint32_t flags) {
    if (flags & kHitBits) return Prim::creature_flags;
    if (flags & KY_CREATURE_ROOTED) return Prim::creature_root;
    return available(Prim::creature_root, nullptr) ? Prim::creature_root : Prim::creature_flags;
}

KyubuResult KYUBU_CALL k_creature_set_flags(KyubuModHandle self, KyubuCreature c, uint32_t flags) {
    GameCall g(self, prim_for(flags), "creature_set_flags");
    if (!g) return g.result();
    if (flags & ~kKnown) return g.refuse(KY_E_INVALID_ARG, fmt("flags 0x%x has bits that are not defined (0x%x are)", flags, kKnown));
    if ((flags & kHitBits) && (flags & KY_CREATURE_ROOTED) && !g.need(Prim::creature_root)) return g.result();
    cube_Creature* cr = g.creature(c);
    if (!cr || !g.owns(cr)) return g.result();
    creature_flags_set(cr->id, flags);
    return KY_OK;
}

}  // namespace

uint32_t creature_flags_of(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_flags.find(id);
    return it == g_flags.end() ? 0 : it->second;
}

void creature_flags_set(uint64_t id, uint32_t flags) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (flags) g_flags[id] = flags;
    else g_flags.erase(id);
    g_count = (unsigned)g_flags.size();
}

void creature_flags_drop(uint64_t id) {
    if (g_count.load(std::memory_order_relaxed) == 0) return;
    creature_flags_set(id, 0);
}

void creature_flags_world_exit() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_flags.clear();
    g_count = 0;
    g_saved.valid = false;
}

void flags_on_push_site(KyubuCpuContext* ctx) {
    if (!(flags_of_creature(ctx->esi) & KY_CREATURE_UNSTOPPABLE)) return;
    for (int i = 0; i < 3; ++i) ctx->xmm[5 + i].f32[0] = rd<float>(ctx->esi + kExtraVel + 4 * i);
}

void flags_on_velocity_site(KyubuCpuContext* ctx) {
    if (!(flags_of_creature(ctx->esi) & KY_CREATURE_UNSTOPPABLE)) return;
    g_saved.valid = true;
    g_saved.creature = ctx->esi;
    for (int i = 0; i < 3; ++i) g_saved.v[i] = rd<float>(ctx->esi + kVelocity + 4 * i);
}

void flags_on_velocity_end_site(KyubuCpuContext* ctx) {
    if (!g_saved.valid) return;
    g_saved.valid = false;
    if (g_saved.creature != ctx->esi) return;
    for (int i = 0; i < 3; ++i) wr<float>(ctx->esi + kVelocity + 4 * i, g_saved.v[i]);
}

void flags_on_slow_site(KyubuCpuContext* ctx) {
    if (flags_of_creature(ctx->esi) & KY_CREATURE_UNSTOPPABLE) ctx->ecx = rd<uint32_t>(ctx->esi + kSlowed);
}

void flags_on_stun_site(KyubuCpuContext* ctx) {
    if (!(flags_of_creature(ctx->esi) & KY_CREATURE_UNSTOPPABLE)) return;
    const int32_t stun = rd<int32_t>(ctx->esi + kStun);
    ctx->ecx = (uint32_t)(stun > 0 ? 0 : stun);   // the site stores ECX: a stun above 0 (from elsewhere) is ended
}

void flags_on_list_site(KyubuCpuContext* ctx) {
    if (g_count.load(std::memory_order_relaxed) == 0) return;
    cube_Hit* hit = reinterpret_cast<cube_Hit*>(ctx->esp + kHitAtListSite);
    if (hit->target_id && hit->damage > 0.0f && (creature_flags_of(hit->target_id) & KY_CREATURE_INVULNERABLE)) hit->damage = 0.0f;
}

void flags_on_damage_site(KyubuCpuContext* ctx) {
    if (!(flags_of_creature(ctx->esi) & KY_CREATURE_INVULNERABLE)) return;
    cube_Hit* hit = reinterpret_cast<cube_Hit*>(ctx->ebx);
    if (hit->damage > 0.0f) hit->damage = 0.0f;
}

uint32_t creature_at_step(const KyubuCpuContext* ctx) { return rd<uint32_t>(ctx->ebp - kCreatureAtStep); }

void root_on_ai_site(KyubuCpuContext* ctx) {
    const uint32_t c = creature_at_step(ctx);
    if (!(flags_of_creature(c) & KY_CREATURE_ROOTED)) return;
    wr<int32_t>(c + kBehaviorTime, 0);   // below 32: the behaviour's update is skipped
    wr<uint8_t>(c + kMode, 0);
    wr<uint32_t>(c + kModeTime, 0);
}

void root_on_move_site(KyubuCpuContext* ctx) {
    const uint32_t c = creature_at_step(ctx);
    if (!(flags_of_creature(c) & KY_CREATURE_ROOTED)) return;
    const uintptr_t step = ctx->ebp - kMoveAtStep;
    wr<float>(step, 0.0f);
    wr<float>(step + 4, 0.0f);
    if (rd<float>(step + 8) > 0.0f) wr<float>(step + 8, 0.0f);   // it may fall, not rise
    for (int i = 0; i < 3; ++i) {
        wr<float>(c + kAccel + 4 * i, 0.0f);
        wr<float>(c + kExtraVel + 4 * i, 0.0f);
    }
    wr<float>(c + kVelocity, 0.0f);
    wr<float>(c + kVelocity + 4, 0.0f);
    if (rd<float>(c + kVelocity + 8) > 0.0f) wr<float>(c + kVelocity + 8, 0.0f);
}

void install_creature_root_hooks() {
    if (g_root_hooks[0]) return;
    std::string why;
    if (!available(Prim::creature_root, &why)) {
        KINFO("creature root: KY_CREATURE_ROOTED is off: %s", why.c_str());
        return;
    }
    const KyubuSide side = L().host.host == KY_HOST_SERVER ? KY_SIDE_SERVER : KY_SIDE_CLIENT;
    const char* const names[2] = {CW_SYM_cube_World_aiTickSite, CW_SYM_cube_World_moveStepSite};
    void (*const fns[2])(KyubuCpuContext*) = {root_on_ai_site, root_on_move_site};
    for (int i = 0; i < 2; ++i) {
        const uintptr_t site = verified_symbol(names[i], &why);
        if (!site || Hooks::get().hook_mid("kyubu", site, side, fns[i], &g_root_hooks[i], &why) != KY_OK) {
            KERROR("creature root: KY_CREATURE_ROOTED is off: %s: %s", names[i], why.c_str());
            remove_creature_root_hooks();
            return;
        }
    }
    KINFO("creature root: installed at 2 sites");
}

void remove_creature_root_hooks() {
    for (KyubuId& id : g_root_hooks) {
        if (id) Hooks::get().remove("kyubu", id);
        id = 0;
    }
}

void install_creature_flag_hooks() {
    install_creature_root_hooks();
    if (g_hooks[0]) return;
    std::string why;
    if (!available(Prim::creature_flags, &why)) {
        KINFO("creature flags: creature_set_flags is off: %s", why.c_str());
        return;
    }
    uintptr_t sites[7] = {};
    for (int i = 0; i < 7; ++i) {
        sites[i] = verified_symbol(kSiteNames[i], &why);
        if (!sites[i]) {
            KERROR("creature flags: creature_set_flags is off: no site %s: %s", kSiteNames[i], why.c_str());
            return;
        }
    }
    for (int i = 0; i < 7; ++i) {
        if (Hooks::get().hook_mid("kyubu", sites[i], KY_SIDE_CLIENT, kSiteFns[i], &g_hooks[i], &why) != KY_OK) {
            KERROR("creature flags: cannot hook %s: %s", kSiteNames[i], why.c_str());
            remove_creature_flag_hooks();
            return;
        }
    }
    KINFO("creature flags: installed at 7 sites");
}

void remove_creature_flag_hooks() {
    remove_creature_root_hooks();
    for (KyubuId& id : g_hooks) {
        if (id) Hooks::get().remove("kyubu", id);
        id = 0;
    }
}

void fill_creature_flags(KyubuApi& a) { a.creature_set_flags = k_creature_set_flags; }

}  // namespace kyubu::game
