// The World of each side, the game thread, the World's lock and the safe points (design D2, D4 and D7).
#include "world.h"

#include <atomic>
#include <mutex>

#include "ability_rank_bind.h"
#include "ability_step.h"
#include "creature_body.h"
#include "creature_flags.h"
#include "events.h"
#include "handles.h"
#include "hooks.h"
#include "lights.h"
#include "loader.h"
#include "particles.h"
#include "log.h"
#include "runtime.h"
#include "symbols.h"
#include "world_step.h"

namespace kyubu::game {

namespace {

// void __thiscall cube::World::update(cube::World* this, int32_t dt_ms, void* step_out, void* step_in), the same in
// both builds; __fastcall with an unused EDX has the same ABI.
using UpdateFn = void(__fastcall*)(cube_World*, void*, int32_t, void*, void*);
UpdateFn g_update_next = nullptr;
KyubuId g_update_hook = 0;

std::atomic<DWORD> g_thread{0};                  // the thread that steps the World
std::atomic<int> g_step_depth{0};                // > 0 while the original cube::World::update runs
std::atomic<cube_World*> g_server_world{nullptr};   // Server.exe: the World its main loop steps
std::atomic<bool> g_loaded{false};               // Cube.exe: between lifecycle.world_enter and world_exit

std::mutex g_queue_mu;
std::vector<std::function<void()>> g_before, g_after;

CRITICAL_SECTION* lock_of(cube_World* w) { return (CRITICAL_SECTION*)&w->lock; }

void run_queue(std::vector<std::function<void()>>& q) {
    std::vector<std::function<void()>> now;
    {
        std::lock_guard<std::mutex> g(g_queue_mu);
        now.swap(q);   // what these queue in turn waits for the next step
    }
    for (auto& fn : now) fn();
}

size_t queued(const std::vector<std::function<void()>>& q) {
    std::lock_guard<std::mutex> g(g_queue_mu);
    return q.size();
}

void __fastcall update_detour(cube_World* self, void*, int32_t dt, void* out, void* in) {
    const DWORD t = GetCurrentThreadId();
    const DWORD was = g_thread.exchange(t);
    if (was != t) {
        if (was) KWARN("game API: cube::World::update now runs on thread %lu (was %lu)", t, was);
        else KINFO("game API: the game thread is %lu (the first World step)", t);
    }
    bool captured = false;
    if (L().host.host == KY_HOST_SERVER) {
        cube_World* old = g_server_world.exchange(self);
        if (old != self) {
            if (old) KWARN("game API: Server.exe steps another World now (0x%08X, was 0x%08X)", (unsigned)(uintptr_t)self, (unsigned)(uintptr_t)old);
            captured = true;
        }
    }
    // The game holds the lock around each step already; taking it again is cheap and keeps the safe
    // points under it wherever the detour is called from.
    EnterCriticalSection(lock_of(self));
    if (captured) {
        KINFO("game API: the server side uses the World at 0x%08X (captured from cube::World::update)", (unsigned)(uintptr_t)self);
        for (auto& line : report()) KINFO("game: %s", line.c_str());
    }
    run_queue(g_before);
    ++g_step_depth;
    g_update_next(self, nullptr, dt, out, in);
    --g_step_depth;
    world_step_end(self);   // the step's output and changed-zone set end here (world_step.cpp)
    flush_particles(out);   // the effects mods asked for (particles.cpp): on this step's output, after the game's own
    run_queue(g_after);
    ability_server_after_step(self);   // Server.exe: the players' mp before a start (ability_step.cpp)
    LeaveCriticalSection(lock_of(self));
    // Server.exe's tick is this step: main calls cube::World::update exactly once per loop pass.
    // Cube.exe emits sim.tick after GameController::update instead (events/game_hooks.cpp).
    if (L().host.host == KY_HOST_SERVER) runtime_on_server_tick((double)dt);
}

// Server.exe has no other tick site: without the detour, sim.tick would silently never fire there.
void no_server_tick(const std::string& why) {
    if (L().host.host == KY_HOST_SERVER)
        Events::get().set_unavailable("sim.tick", "Server.exe's tick, cube::World::update, is not detoured: " + why);
}

// The creatures in a VC110 std::map, counted by walking its tree (the map's size is the game's own count).
uint32_t count_nodes(const std_map& m, bool* ok) {
    *ok = true;
    const std_tree_node* head = m.head;
    if (!head) return 0;
    std::vector<const std_tree_node*> todo;
    if (!head->parent->isnil) todo.push_back(head->parent);   // the head's parent is the root
    uint32_t n = 0;
    while (!todo.empty()) {
        const std_tree_node* node = todo.back();
        todo.pop_back();
        if (++n > m.size + 1024) { *ok = false; break; }   // not a tree: stop
        if (!node->left->isnil) todo.push_back(node->left);
        if (!node->right->isnil) todo.push_back(node->right);
    }
    return n;
}

std::string describe(cube_World* w) {
    EnterCriticalSection(lock_of(w));
    bool ok = true;
    const uint32_t n = count_nodes(w->creatures, &ok);
    const cube_Creature* lp = w->local_player;
    std::string s = fmt("0x%08X, joined %d, local player %s, %u creature(s)%s (map size %u)", (unsigned)(uintptr_t)w,
                        w->joined ? 1 : 0, lp ? fmt("id %llu", (unsigned long long)lp->id).c_str() : "none", n,
                        ok ? "" : " (walk stopped: not a tree)", w->creatures.size);
    LeaveCriticalSection(lock_of(w));
    return s;
}

// Who holds the lock (read before taking it): OwningThread is the owner's thread id, RecursionCount how often.
std::string lock_state(cube_World* w) {
    const CRITICAL_SECTION* cs = lock_of(w);
    const DWORD owner = (DWORD)(uintptr_t)cs->OwningThread;
    if (!owner) return "free";
    return fmt("held by thread %lu%s (recursion %ld)", owner, owner == g_thread.load() ? ", the game thread" : "",
               cs->RecursionCount);
}

}  // namespace

cube_World* side_world(KyubuSide side, KyubuResult* r, std::string* why) {
    auto no = [&](KyubuResult code, const std::string& reason) -> cube_World* {
        if (r) *r = code;
        if (why) *why = reason;
        return nullptr;
    };
    if (side != KY_SIDE_CLIENT && side != KY_SIDE_SERVER) return no(KY_E_INVALID_ARG, "not a side");
    std::string w;
    if (!available(Prim::world, &w)) return no(KY_E_UNAVAILABLE, "world is unavailable: " + w);
    const Loader& l = L();
    if (l.host.host == KY_HOST_SERVER) {
        if (side == KY_SIDE_CLIENT) return no(KY_E_SIDE, "Server.exe has no client side");
        cube_World* sw = g_server_world.load();
        if (!sw) return no(KY_E_PHASE, "no world is loaded (Server.exe's World has not stepped yet)");
        if (r) *r = KY_OK;
        return sw;
    }
    if (side == KY_SIDE_SERVER && l.mode == KY_MODE_REMOTE_CLIENT) return no(KY_E_SIDE, "a joined client has no server side");
    if (!g_loaded) return no(KY_E_PHASE, "no world is loaded");
    const uintptr_t global = verified_symbol("cube::game_controller");
    cube_GameController* gc = global ? *(cube_GameController**)global : nullptr;
    if (!gc) return no(KY_E_PHASE, "no world is loaded (no game controller)");
    if (r) *r = KY_OK;
    return &gc->world;
}

DWORD game_thread() { return g_thread.load(); }
bool on_game_thread() { return g_thread.load() == GetCurrentThreadId(); }
bool in_step() { return g_step_depth.load() > 0 && on_game_thread(); }

void before_step(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(g_queue_mu);
    g_before.push_back(std::move(fn));
}

void after_step(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(g_queue_mu);
    g_after.push_back(std::move(fn));
}

void on_world_enter() { g_loaded = true; }

void on_world_exit() {
    ability_end_all(EndReason::Gone);   // before the handles go: on_end still sees its creature's handle
    ability_rank_world_exit();
    lights_world_exit();
    creature_flags_world_exit();
    creature_body_world_exit();
    g_loaded = false;
    invalidate_handles();
    size_t dropped;
    {
        std::lock_guard<std::mutex> g(g_queue_mu);
        dropped = g_before.size() + g_after.size();
        g_before.clear();
        g_after.clear();
    }
    if (dropped) KINFO("game API: the world was left; %u queued safe-point task(s) dropped", (unsigned)dropped);
}

void install_world_hooks() {
    if (g_update_hook) return;
    std::string err;
    if (!available(Prim::world, &err)) return no_server_tick("world is unavailable: " + err);
    const uintptr_t a = verified_symbol("cube::World::update");
    if (!a || Hooks::get().hook_function("kyubu", a, (void*)&update_detour, kLoaderPriority, (void**)&g_update_next,
                                         &g_update_hook, &err) != KY_OK) {
        if (!a) err = "not a verified symbol";
        KERROR("game API: cannot detour cube::World::update: %s", err.c_str());
        no_server_tick(err);
    }
}

void remove_world_hooks() {
    if (g_update_hook) Hooks::get().remove("kyubu", g_update_hook);
    g_update_hook = 0;
    g_update_next = nullptr;
    g_thread = 0;
    g_server_world = nullptr;
    g_loaded = false;
    std::lock_guard<std::mutex> g(g_queue_mu);
    g_before.clear();
    g_after.clear();
}

WorldAccess::WorldAccess(Prim p, KyubuSide side) {
    std::string w;
    if (!available(p, &w)) {
        r_ = KY_E_UNAVAILABLE;
        why_ = std::string(prim_name(p)) + " is unavailable: " + w;
        return;
    }
    cube_World* world = side_world(side, &r_, &why_);
    if (!world) return;
    if (!g_thread.load()) {
        r_ = KY_E_PHASE;
        why_ = "no world is loaded (the World has not stepped yet)";
        return;
    }
    if (!on_game_thread()) {
        r_ = KY_E_UNSAFE;
        why_ = "not on the game thread (use run_on_main_thread)";
        return;
    }
    EnterCriticalSection(lock_of(world));
    world_ = world;
}

WorldAccess::~WorldAccess() {
    if (world_) LeaveCriticalSection(lock_of(world_));
}

std::vector<std::string> report() {
    std::vector<std::string> out;
    const Loader& l = L();
    const char* host = l.host.host == KY_HOST_SERVER         ? "Server.exe, dedicated"
                       : l.mode == KY_MODE_REMOTE_CLIENT ? "Cube.exe, joined"
                                                         : "Cube.exe, single-player";
    out.push_back(fmt("host: %s (%s)", host, l.host.build_id.c_str()));
    std::string why;
    KyubuResult r = KY_OK;
    cube_World* cw = side_world(KY_SIDE_CLIENT, &r, &why);
    out.push_back("client side: " + (cw ? "World " + describe(cw) : "no World (" + why + ")"));
    cube_World* sw = side_world(KY_SIDE_SERVER, &r, &why);
    if (sw && sw == cw) out.push_back(fmt("server side: the same World (0x%08X)", (unsigned)(uintptr_t)sw));
    else out.push_back("server side: " + (sw ? "World " + describe(sw) : "no World (" + why + ")"));
    const DWORD t = g_thread.load();
    out.push_back(t ? fmt("game thread: %lu (this is thread %lu)", t, GetCurrentThreadId())
                    : std::string("game thread: not known yet (the World has not stepped)"));
    if (cube_World* w = cw ? cw : sw) out.push_back("World lock: " + lock_state(w));
    out.push_back(fmt("in a step: %s; safe points queued: %u before, %u after", g_step_depth.load() > 0 ? "yes" : "no",
                      (unsigned)queued(g_before), (unsigned)queued(g_after)));
    for (auto& line : handle_report()) out.push_back(line);
    for (int p = 0; p < (int)Prim::count; ++p) {
        std::string w;
        bool ok = available((Prim)p, &w);
        out.push_back(fmt("primitive %s: %s", prim_name((Prim)p), ok ? "available" : ("unavailable: " + w).c_str()));
    }
    std::string slot_why;
    out.push_back(ability_slot_available(&slot_why) ? std::string("fourth ability slot: available")
                                                    : "fourth ability slot: " + slot_why);
    return out;
}

}  // namespace kyubu::game
