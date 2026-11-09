// The player_shots primitive (openspec add-skills-unleashed-multiplayer 10.9, design D16; docs/game-api.md, shot_take and
// projectile_push). Server.exe only.
//
// A joined client sends each projectile it shoots as packet 9: the 0x70-byte cube::Projectile, with the client's start, direction,
// speed and kind. Server.exe's receive thread queues it on cube::Server's list and the main loop hands that list to the broadcast;
// the server's World never flies it. Here a mid hook at Net::receiveProjectileSite (the connection's thread, the World lock held,
// before the game's push) copies the packet and its connection into a queue of the loader, and leaves the game's packet alone, so
// the relay is unchanged. A mod takes the copies with shot_take on the game thread, and may put a projectile of its own (a checked
// copy) into the World's list with projectile_push, inside a World step: the World's projectile step then flies it, stops it on the
// server's blocks and makes its hits there.
//
// Nothing is queued until a mod's first shot_take: with no mod that wants them, the hook copies nothing.
#include "shots.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>

#include "call.h"
#include "cubeworld/functions.h"
#include "cubeworld/types.h"
#include "game.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "net_transport.h"
#include "symbols.h"
#include "world.h"
#include "world_step.h"

namespace kyubu::game {

namespace {

// From the mapping note of Net::receiveProjectileSite: EBP-0x110 the projectile; EDI the closure (+4 the connections, +8 the slot).
constexpr uintptr_t kFrameProjectile = 0x110;
constexpr size_t kClosureConnections = 4, kClosureSlot = 8;
constexpr size_t kMaxQueued = 256;     // shots waiting for a take; past it the oldest goes (logged once)

static_assert(sizeof(cube_Projectile) == KY_SHOT_DATA_SIZE, "a shot is one cube::Projectile");

std::mutex g_mu;
std::deque<KyubuShot> g_queue;
std::atomic<bool> g_wanted{false};
bool g_dropped_logged = false;
KyubuId g_hook = 0;

template <class T> T rd(uintptr_t a) { T v{}; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v); return v; }

// The closure's cube::Connection, or 0 (the same walk as the channel transport's).
uintptr_t closure_connection(uintptr_t closure) {
    const uintptr_t vec = rd<uintptr_t>(closure + kClosureConnections);
    if (!vec) return 0;
    const uintptr_t begin = rd<uintptr_t>(vec), end = rd<uintptr_t>(vec + 4);
    const uint32_t slot = rd<uint32_t>(closure + kClosureSlot);
    if (!begin || end < begin || slot >= (end - begin) / 4) return 0;
    return rd<uintptr_t>(begin + 4 * slot);
}

void enqueue(uint32_t connection, const void* data) {
    KyubuShot s = {};
    s.size = sizeof s;
    s.connection = connection;
    std::memcpy(s.data, data, KY_SHOT_DATA_SIZE);
    std::lock_guard<std::mutex> g(g_mu);
    if (g_queue.size() >= kMaxQueued) {
        g_queue.pop_front();
        if (!g_dropped_logged) {
            g_dropped_logged = true;
            KWARN("player shots: more than %u shots were not taken: the oldest are dropped (logged once)", (unsigned)kMaxQueued);
        }
    }
    g_queue.push_back(s);
}

KyubuResult KYUBU_CALL k_shot_take(KyubuModHandle self, KyubuShot* out, uint32_t max, uint32_t* count) {
    if (count) *count = 0;
    GameCall g(self, Prim::player_shots, "shot_take");
    if (!g) return g.result();
    if (!count) return g.refuse(KY_E_INVALID_ARG, "count is NULL");
    if (max && !out) return g.refuse(KY_E_INVALID_ARG, "out is NULL");
    for (uint32_t i = 0; i < max; ++i)
        if (out[i].size < sizeof(KyubuShot))
            return g.refuse(KY_E_INVALID_ARG, fmt("out[%u].size is %u, below %u", i, out[i].size, (unsigned)sizeof(KyubuShot)));
    g_wanted.store(true);
    std::lock_guard<std::mutex> lock(g_mu);
    uint32_t n = 0;
    while (n < max && !g_queue.empty()) {
        out[n] = g_queue.front();
        g_queue.pop_front();
        ++n;
    }
    *count = n;
    return KY_OK;
}

KyubuResult KYUBU_CALL k_projectile_push(KyubuModHandle self, const uint8_t* data, uint32_t len) {
    GameCall g(self, Prim::player_shots, "projectile_push");
    if (!g) return g.result();
    if (!data) return g.refuse(KY_E_INVALID_ARG, "data is NULL");
    if (len != KY_SHOT_DATA_SIZE) return g.refuse(KY_E_INVALID_ARG, fmt("len is %u, not %u", len, (unsigned)KY_SHOT_DATA_SIZE));
    if (!in_step() || step_world() != g.world())
        return g.refuse(KY_E_PHASE, "not inside a World step: call it from sim.world_step, or later in the same step");
    std::string why;
    const auto push = (cw_std_list_projectile_push_back_fn)verified_symbol(CW_SYM_std_list_projectile_push_back, &why);
    if (!push) return g.refuse(KY_E_UNAVAILABLE, "std_list_projectile_push_back: " + why);
    cube_Projectile p;
    std::memcpy(&p, data, sizeof p);
    push(reinterpret_cast<char*>(g.world()) + offsetof(cube_World, projectiles), nullptr, &p);
    return KY_OK;
}

}  // namespace

void shots_on_receive_site(KyubuCpuContext* ctx) {
    if (!g_wanted.load()) return;
    const uintptr_t c = closure_connection(ctx->edi);
    const uint32_t conn = c ? conn_for_creature(rd<int64_t>(c + offsetof(cube_Connection, creature_id))) : 0;
    enqueue(conn, reinterpret_cast<const void*>(ctx->ebp - kFrameProjectile));
}

void install_shot_hooks() {
    if (g_hook) return;
    std::string why;
    if (!available(Prim::player_shots, &why)) {
        KINFO("player shots: shot_take and projectile_push are off: %s", why.c_str());
        return;
    }
    const uintptr_t site = verified_symbol(CW_SYM_Net_receiveProjectileSite, &why);
    if (!site || Hooks::get().hook_mid("kyubu", site, KY_SIDE_SERVER, shots_on_receive_site, &g_hook, &why) != KY_OK) {
        KERROR("player shots: shot_take and projectile_push are off: %s", why.c_str());
        remove_shot_hooks();
        return;
    }
    KINFO("player shots: installed at %s", CW_SYM_Net_receiveProjectileSite);
}

void remove_shot_hooks() {
    if (g_hook) Hooks::get().remove("kyubu", g_hook);
    g_hook = 0;
    reset_shots_for_test();
}

void shots_enqueue_for_test(uint32_t connection, const void* data) { enqueue(connection, data); }

void reset_shots_for_test() {
    std::lock_guard<std::mutex> g(g_mu);
    g_queue.clear();
    g_wanted.store(false);
    g_dropped_logged = false;
}

void fill_shots(KyubuApi& a) {
    a.shot_take = k_shot_take;
    a.projectile_push = k_projectile_push;
}

}  // namespace kyubu::game
