// The server_hits primitive: a fix of Server.exe's own code, on by itself (no API function).
//
// cube::World::hitCreature pushes its finished Hit on the step output's hit list (at cube::World::hitListSite) and then, on a World
// that is not a client's, calls cube::World::applyHit on the same Hit. Server.exe's World has no local player, so applyHit, for a
// target that is a player, does nothing but push the Hit on the same list again (cube::World::hitForwardSite) and return. Both copies
// go to the clients, and a joined client takes each hit on its player from the hp: a joined player loses twice the damage of every
// such hit (replay server-guardian-0720, run 5: 2.000 times, 24 hits of 24).
//
// The mid hook at hitListSite notes the Hit's address; the applyHit detour, when it is called on that same Hit and the target is a
// player, returns without calling the game's applyHit, so the hit is sent once. The game's applyHit would have done nothing else for
// that target. Every other call (the clients' hits, from the step's received list; hits on monsters) is passed on unchanged.
#include "server_hits.h"

#include <atomic>

#include "cubeworld/functions.h"
#include "game.h"
#include "handles.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

constexpr uintptr_t kHitAtListSite = 0xC4;   // from ESP at hitListSite
constexpr uint8_t kHostilePlayer = 0;        // cube::CreatureData.hostile_type of a player

ApplyHitFn g_next = nullptr;
KyubuId g_list_hook = 0, g_apply_hook = 0;
const cube_Hit* g_listed = nullptr;   // the Hit hitCreature pushed last and is about to apply (the game thread only)
std::atomic<uint64_t> g_dropped{0};

}  // namespace

void server_hits_on_list_site(KyubuCpuContext* ctx) { g_listed = reinterpret_cast<const cube_Hit*>(ctx->esp + kHitAtListSite); }

void __fastcall server_hits_apply_detour(cube_World* self, void* edx, const cube_Hit* hit, void* out, void* zones) {
    if (hit && hit == g_listed) {
        g_listed = nullptr;
        const cube_Creature* t = creature_by_id(self, hit->target_id);
        if (t && t->data.hostile_type == kHostilePlayer) {
            if (g_dropped.fetch_add(1) == 0)
                KINFO("server hits: a hit on player %llu was listed once, not twice (the first; the others are not logged)",
                      (unsigned long long)hit->target_id);
            return;
        }
    }
    if (g_next) g_next(self, edx, hit, out, zones);
}

void install_server_hit_hooks() {
    if (g_apply_hook) return;
    std::string why;
    if (!available(Prim::server_hits, &why)) {
        KINFO("server hits: off (each hit on a player is sent twice, as in the game): %s", why.c_str());
        return;
    }
    const uintptr_t list = verified_symbol(CW_SYM_cube_World_hitListSite, &why);
    const uintptr_t apply = list ? verified_symbol(CW_SYM_cube_World_applyHit, &why) : 0;
    if (!list || !apply ||
        Hooks::get().hook_mid("kyubu", list, KY_SIDE_SERVER, server_hits_on_list_site, &g_list_hook, &why) != KY_OK ||
        Hooks::get().hook_function("kyubu", apply, (void*)&server_hits_apply_detour, kLoaderPriority, (void**)&g_next,
                                   &g_apply_hook, &why) != KY_OK) {
        KERROR("server hits: off (each hit on a player is sent twice, as in the game): %s", why.c_str());
        remove_server_hit_hooks();
        return;
    }
    KINFO("server hits: installed at %s and %s", CW_SYM_cube_World_hitListSite, CW_SYM_cube_World_applyHit);
}

void remove_server_hit_hooks() {
    if (g_list_hook) Hooks::get().remove("kyubu", g_list_hook);
    if (g_apply_hook) Hooks::get().remove("kyubu", g_apply_hook);
    g_list_hook = g_apply_hook = 0;
    g_next = nullptr;
    g_listed = nullptr;
    g_dropped = 0;
}

void set_server_hits_next_for_test(ApplyHitFn next) { g_next = next; }
uint64_t server_hits_dropped() { return g_dropped.load(); }

}  // namespace kyubu::game
