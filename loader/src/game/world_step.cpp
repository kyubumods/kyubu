// The world_step primitive (openspec add-skills-unleashed-multiplayer 10.2, design D11; docs/game-api.md, creature_hit and
// sim.world_step).
//
// cube::World::worldStepSite is the point of cube::World::update where both paths of the received-hits test join: the step's
// changed-zone set is built, the clients' hits and buffs are applied, and the zone items and the creatures are still to come. The
// site's frame holds what hitCreature and applyHit take (the step output and the changed-zone set), the same on both builds, so the
// loader reads them there and a mod needs no frame offset.
//
// cube::World::hitCreature takes a stun chance, not a time: it tries it only when the target's last stun ended more than 3 s ago, and
// a stun it gives lasts the target's own maximum and sets the direction's z to 10. So creature_hit passes chance 0, and the mid hook on
// cube::World::hitListSite writes the mod's stun and direction into the cube::Hit there, before hitCreature pushes it on the output's
// hit list (the damage number) and applies it. Only creature_hit's own call is changed: the game's hits pass untouched.
#include "world_step.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#include "call.h"
#include "cubeworld/functions.h"
#include "cubeworld/types.h"
#include "events.h"
#include "game.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "symbols.h"
#include "world.h"

namespace kyubu::game {

namespace {

// From EBP at worldStepSite (mapping/<build>/functions/world.toml): the World, the step output, the changed-zone set, dt.
constexpr uintptr_t kFrameWorld = 0x2B18, kFrameOut = 0x2B28, kFrameEffects = 0x2BE8, kFrameDt = 8;
constexpr uintptr_t kHitAtListSite = 0xC4;    // from ESP at hitListSite
constexpr int32_t kStunMax = 60000;          // a longer stun is a mistake, not an ability

template <class T> T rd(uintptr_t a) { T v{}; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v); return v; }
template <class T> void wr(uintptr_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v); }

// The step being run, from the site to the end of cube::World::update. The game thread only.
struct Step {
    cube_World* world = nullptr;
    void* out = nullptr;
    void* effects = nullptr;
};
Step g_step;

// creature_hit's own call, while hitCreature runs.
struct Pending {
    bool on = false;
    int32_t stun = 0;
    float dir[3] = {};
    int seen = 0;
};
Pending g_pending;

KyubuId g_site_hook = 0, g_list_hook = 0;

bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

KyubuResult KYUBU_CALL k_creature_hit(KyubuModHandle self, KyubuCreature attacker, KyubuCreature target, const KyubuHit* hit,
                                      int* landed) {
    if (landed) *landed = 0;
    GameCall g(self, Prim::world_step, "creature_hit");
    if (!g) return g.result();
    if (!hit) return g.refuse(KY_E_INVALID_ARG, "hit is NULL");
    if (hit->size < sizeof(KyubuHit))
        return g.refuse(KY_E_INVALID_ARG, fmt("hit->size is %u, below %u", hit->size, (unsigned)sizeof(KyubuHit)));
    if (!std::isfinite(hit->damage) || !finite3(hit->knock)) return g.refuse(KY_E_INVALID_ARG, "damage or knock is not a finite number");
    if (hit->stun_ms < 0 || hit->stun_ms > kStunMax)
        return g.refuse(KY_E_INVALID_ARG, fmt("stun_ms is %d, outside 0 to %d", hit->stun_ms, kStunMax));
    if (hit->flags & ~KY_HIT_MAGIC) return g.refuse(KY_E_INVALID_ARG, fmt("flags 0x%x has bits that are not defined", hit->flags));
    if (!in_step() || g_step.world != g.world())
        return g.refuse(KY_E_PHASE, "not inside a World step: call it from sim.world_step, or later in the same step");
    if (g_pending.on) return g.refuse(KY_E_UNSAFE, "called from inside another creature_hit");
    cube_Creature* t = g.creature(target);
    if (!t) return g.result();
    cube_Creature* a = nullptr;
    if (attacker) {
        a = g.creature(attacker);
        if (!a) return g.result();
    }
    std::string why;
    const auto fn = (cw_cube_World_hitCreature_fn)verified_symbol(CW_SYM_cube_World_hitCreature, &why);
    if (!fn) return g.refuse(KY_E_UNAVAILABLE, "cube::World::hitCreature: " + why);
    cube_FloatVector3 dir = {hit->knock[0], hit->knock[1], hit->knock[2]};
    g_pending.on = true;
    g_pending.stun = hit->stun_ms;
    std::memcpy(g_pending.dir, hit->knock, sizeof g_pending.dir);
    g_pending.seen = 0;
    // As the game's melee call: the attacker's position, no crit, not powered, chance 0, no ability, no projectile, a block counts.
    const bool ok = fn(g.world(), nullptr, t, a, hit->damage, 0, 0, 0.0f, a ? &a->data.pos : &t->data.pos, &dir, g_step.out,
                       g_step.effects, (hit->flags & KY_HIT_MAGIC) ? 1 : 0, 0, nullptr, 1);
    g_pending.on = false;
    if (landed) *landed = ok ? 1 : 0;
    return KY_OK;
}

void emit_step(cube_World* w, int32_t dt) {
    KyubuWorldStepData d = {};
    d.world = w;
    d.dt = dt;
    d.step_out = g_step.out;
    d.effects = g_step.effects;
    // Each side whose World this is: single-player Cube.exe both, a joined client its client side, Server.exe its server side.
    for (KyubuSide s : {KY_SIDE_CLIENT, KY_SIDE_SERVER}) {
        if (!side_active(s) || side_world(s) != w) continue;
        Events::get().emit("sim.world_step", s, &d, sizeof d);
    }
}

}  // namespace

void step_on_world_site(KyubuCpuContext* ctx) {
    const uintptr_t ebp = ctx->ebp;
    cube_World* w = rd<cube_World*>(ebp - kFrameWorld);
    if (!w) return;
    g_step.world = w;
    g_step.out = rd<void*>(ebp - kFrameOut);
    g_step.effects = reinterpret_cast<void*>(ebp - kFrameEffects);
    emit_step(w, rd<int32_t>(ebp + kFrameDt));
}

void step_on_list_site(KyubuCpuContext* ctx) {
    if (!g_pending.on) return;
    const uintptr_t h = ctx->esp + kHitAtListSite;
    wr<int32_t>(h + offsetof(cube_Hit, stun_duration), g_pending.stun);
    for (int i = 0; i < 3; ++i) wr<float>(h + offsetof(cube_Hit, hit_dir) + 4 * i, g_pending.dir[i]);
    ++g_pending.seen;
}

cube_World* step_world() { return g_step.world; }

void world_step_end(cube_World* w) {
    if (g_step.world == w) g_step = Step{};
}

void install_world_step_hooks() {
    if (g_site_hook) return;
    std::string why;
    if (!available(Prim::world_step, &why)) {
        KINFO("world step: sim.world_step and creature_hit are off: %s", why.c_str());
        Events::get().set_unavailable("sim.world_step", "world_step is unavailable: " + why);
        return;
    }
    const KyubuSide side = L().host.host == KY_HOST_SERVER ? KY_SIDE_SERVER : KY_SIDE_CLIENT;
    const uintptr_t site = verified_symbol(CW_SYM_cube_World_worldStepSite, &why);
    const uintptr_t list = site ? verified_symbol(CW_SYM_cube_World_hitListSite, &why) : 0;
    if (!site || !list || Hooks::get().hook_mid("kyubu", site, side, step_on_world_site, &g_site_hook, &why) != KY_OK ||
        Hooks::get().hook_mid("kyubu", list, side, step_on_list_site, &g_list_hook, &why) != KY_OK) {
        KERROR("world step: sim.world_step and creature_hit are off: %s", why.c_str());
        Events::get().set_unavailable("sim.world_step", "the world step hooks failed: " + why);
        remove_world_step_hooks();
        return;
    }
    KINFO("world step: installed at %s and %s", CW_SYM_cube_World_worldStepSite, CW_SYM_cube_World_hitListSite);
}

void remove_world_step_hooks() {
    if (g_site_hook) Hooks::get().remove("kyubu", g_site_hook);
    if (g_list_hook) Hooks::get().remove("kyubu", g_list_hook);
    g_site_hook = g_list_hook = 0;
    g_step = Step{};
    g_pending = Pending{};
}

void fill_world_step(KyubuApi& a) { a.creature_hit = k_creature_hit; }

}  // namespace kyubu::game
