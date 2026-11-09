// Creature handles and ownership (design D2 and D3).
#include "handles.h"

#include <mutex>
#include <unordered_map>

#include "animations.h"
#include "creature_body.h"
#include "creature_flags.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "symbols.h"
#include "world.h"

namespace kyubu::game {

namespace {

// void __thiscall cube::Creature::~Creature(cube::Creature* this); __fastcall with an unused EDX has the same ABI.
using DtorFn = void(__fastcall*)(cube_Creature*, void*);
DtorFn g_dtor_next = nullptr;
KyubuId g_dtor_hook = 0;

constexpr uint32_t kSlotBits = 24;
constexpr uint32_t kSlotMask = (1u << kSlotBits) - 1;

struct Slot {
    cube_Creature* creature = nullptr;   // null while the slot is free
    uint32_t generation = 1;             // bumped when the slot is freed; never 0
};

struct SideTable {
    std::vector<Slot> slots;
    std::vector<uint32_t> free;
    std::unordered_map<const cube_Creature*, uint32_t> by_ptr;
};

// The destructor runs on the game thread and, for the game's temporary creature, on a worker thread: the tables have
// their own mutex, taken last (never while calling into the game or the World).
std::mutex g_mu;
SideTable g_tables[3];   // [KY_SIDE_CLIENT], [KY_SIDE_SERVER]
uint64_t g_invalidated = 0;
std::vector<std::pair<KyubuCreature, uint64_t>> g_held;   // `game hold`: handle, game id

bool valid_side(KyubuSide s) { return s == KY_SIDE_CLIENT || s == KY_SIDE_SERVER; }

KyubuCreature pack(uint32_t gen, KyubuSide side, uint32_t slot) {
    return ((uint64_t)gen << 32) | ((uint64_t)(uint8_t)side << kSlotBits) | slot;
}

void free_slot(SideTable& t, uint32_t i) {
    Slot& s = t.slots[i];
    t.by_ptr.erase(s.creature);
    s.creature = nullptr;
    if (++s.generation == 0) s.generation = 1;
    t.free.push_back(i);
    ++g_invalidated;
}

void __fastcall dtor_detour(cube_Creature* self, void*) {
    on_creature_destroyed(self);
    g_dtor_next(self, nullptr);
}

// The creature with a game id, found by walking the World's map (std::map<uint64, cube::Creature*>: the key at the
// node's +0x10, the value after it).
cube_Creature* find_by_id(cube_World* w, uint64_t id) {
    const std_tree_node* head = w->creatures.head;
    if (!head) return nullptr;
    std::vector<const std_tree_node*> todo;
    if (!head->parent->isnil) todo.push_back(head->parent);
    uint32_t n = 0;
    while (!todo.empty()) {
        const std_tree_node* node = todo.back();
        todo.pop_back();
        if (++n > w->creatures.size + 1024) return nullptr;   // not a tree: stop
        const uint8_t* e = (const uint8_t*)node + sizeof(std_tree_node);
        if (*(const uint64_t*)e == id) return *(cube_Creature* const*)(e + 8);
        if (!node->left->isnil) todo.push_back(node->left);
        if (!node->right->isnil) todo.push_back(node->right);
    }
    return nullptr;
}

// Every creature of the World's map, as (id, creature).
std::vector<std::pair<uint64_t, cube_Creature*>> all_creatures(cube_World* w) {
    std::vector<std::pair<uint64_t, cube_Creature*>> out;
    const std_tree_node* head = w->creatures.head;
    if (!head) return out;
    std::vector<const std_tree_node*> todo;
    if (!head->parent->isnil) todo.push_back(head->parent);
    while (!todo.empty() && out.size() <= w->creatures.size + 1024) {
        const std_tree_node* node = todo.back();
        todo.pop_back();
        const uint8_t* e = (const uint8_t*)node + sizeof(std_tree_node);
        out.emplace_back(*(const uint64_t*)e, *(cube_Creature* const*)(e + 8));
        if (!node->left->isnil) todo.push_back(node->left);
        if (!node->right->isnil) todo.push_back(node->right);
    }
    return out;
}

}  // namespace

std::vector<std::pair<uint64_t, cube_Creature*>> world_creatures(cube_World* w) { return all_creatures(w); }

cube_Creature* creature_by_id(cube_World* w, uint64_t id) { return find_by_id(w, id); }

KyubuCreature handle_for(KyubuSide side, cube_Creature* c) {
    if (!c || !valid_side(side)) return 0;
    std::lock_guard<std::mutex> g(g_mu);
    SideTable& t = g_tables[side];
    auto it = t.by_ptr.find(c);
    if (it != t.by_ptr.end()) return pack(t.slots[it->second].generation, side, it->second);
    uint32_t i;
    if (!t.free.empty()) {
        i = t.free.back();
        t.free.pop_back();
    } else {
        if (t.slots.size() > kSlotMask) return 0;
        i = (uint32_t)t.slots.size();
        t.slots.emplace_back();
    }
    t.slots[i].creature = c;
    t.by_ptr[c] = i;
    return pack(t.slots[i].generation, side, i);
}

cube_Creature* resolve_handle(KyubuSide side, KyubuCreature h, KyubuResult* r, std::string* why) {
    auto no = [&](KyubuResult code, const std::string& reason) -> cube_Creature* {
        if (r) *r = code;
        if (why) *why = reason;
        return nullptr;
    };
    if (!valid_side(side)) return no(KY_E_INVALID_ARG, "not a side");
    if (!h) return no(KY_E_INVALID_ARG, "no creature (handle 0)");
    const KyubuSide hs = (KyubuSide)((h >> kSlotBits) & 0xFF);
    if (!valid_side(hs)) return no(KY_E_INVALID_ARG, "not a creature handle");
    if (hs != side)
        return no(KY_E_SIDE, std::string("the handle was issued on the ") + (hs == KY_SIDE_CLIENT ? "client" : "server") +
                                 " side and cannot be used on the " + (side == KY_SIDE_CLIENT ? "client" : "server") +
                                 " side (game ids cross sides, handles do not)");
    std::lock_guard<std::mutex> g(g_mu);
    const SideTable& t = g_tables[side];
    const uint32_t i = (uint32_t)(h & kSlotMask);
    if (i >= t.slots.size() || !t.slots[i].creature || t.slots[i].generation != (uint32_t)(h >> 32))
        return no(KY_E_GONE, "the creature no longer exists");
    if (r) *r = KY_OK;
    return t.slots[i].creature;
}

KyubuSide owner_of(const cube_World* w, const cube_Creature* c) {
    if (L().host.host == KY_HOST_SERVER) return KY_SIDE_SERVER;
    return w && c == w->local_player ? KY_SIDE_CLIENT : KY_SIDE_SERVER;
}

void on_creature_destroyed(const cube_Creature* c) {
    creature_flags_drop(c->id);
    creature_body_drop(c->id);
    Animations::get().drop_creature(c->id);
    std::lock_guard<std::mutex> g(g_mu);
    for (SideTable& t : g_tables) {
        auto it = t.by_ptr.find(c);
        if (it != t.by_ptr.end()) free_slot(t, it->second);
    }
}

void invalidate_handles() {
    Animations::get().clear_creature_bindings();   // creature ids are a world's
    std::lock_guard<std::mutex> g(g_mu);
    for (SideTable& t : g_tables)
        for (uint32_t i = 0; i < t.slots.size(); ++i)
            if (t.slots[i].creature) free_slot(t, i);
}

void install_handle_hooks() {
    if (g_dtor_hook || !available(Prim::handles)) return;
    const uintptr_t a = verified_symbol("cube::Creature::~Creature");
    std::string err;
    if (!a || Hooks::get().hook_function("kyubu", a, (void*)&dtor_detour, kLoaderPriority, (void**)&g_dtor_next,
                                         &g_dtor_hook, &err) != KY_OK)
        KERROR("game API: cannot detour cube::Creature::~Creature: %s", a ? err.c_str() : "not a verified symbol");
}

void remove_handle_hooks() {
    if (g_dtor_hook) Hooks::get().remove("kyubu", g_dtor_hook);
    g_dtor_hook = 0;
    g_dtor_next = nullptr;
    std::lock_guard<std::mutex> g(g_mu);
    for (SideTable& t : g_tables) t = SideTable{};
    g_held.clear();
    g_invalidated = 0;
}

HandleStats handle_stats(bool reset_invalidated) {
    std::lock_guard<std::mutex> g(g_mu);
    HandleStats s;
    for (int side = KY_SIDE_CLIENT; side <= KY_SIDE_SERVER; ++side) s.live[side] = (uint32_t)g_tables[side].by_ptr.size();
    s.invalidated = g_invalidated;
    if (reset_invalidated) g_invalidated = 0;
    return s;
}

namespace {
// Adds a handle to the `game hold` list (once); 1 when it was added.
int hold_creature_locked(uint64_t id, KyubuCreature h) {
    if (!h) return 0;
    std::lock_guard<std::mutex> g(g_mu);
    for (auto& p : g_held)
        if (p.first == h) return 0;
    g_held.emplace_back(h, id);
    return 1;
}
}  // namespace

std::string hold_creature(uint64_t id, bool all) {
    WorldAccess w(Prim::handles, KY_SIDE_CLIENT);
    if (!w) return "cannot hold: " + w.why();
    if (all) {
        size_t n = 0;
        for (auto& [cid, c] : all_creatures(w.world())) {
            const KyubuCreature h = handle_for(KY_SIDE_CLIENT, c);
            hold_creature_locked(cid, h);
            n += h != 0;
        }
        return fmt("holding %u creature(s)", (unsigned)n);
    }
    cube_Creature* c = find_by_id(w.world(), id);
    if (!c) return fmt("no creature with id %llu in the client World", (unsigned long long)id);
    const KyubuCreature h = handle_for(KY_SIDE_CLIENT, c);
    if (!h) return "no handle could be made";
    hold_creature_locked(id, h);
    return fmt("holding creature %llu as handle 0x%016llX", (unsigned long long)id, (unsigned long long)h);
}

std::vector<std::string> handle_report() {
    std::vector<std::string> out;
    const HandleStats s = handle_stats(true);
    out.push_back(fmt("handles: %u live on the client side, %u on the server side; %llu invalidated since the last report",
                      s.live[KY_SIDE_CLIENT], s.live[KY_SIDE_SERVER], (unsigned long long)s.invalidated));
    std::vector<std::pair<KyubuCreature, uint64_t>> held;
    {
        std::lock_guard<std::mutex> g(g_mu);
        held = g_held;
    }
    for (auto& [h, id] : held) {
        KyubuResult r = KY_OK;
        std::string why;
        resolve_handle(KY_SIDE_CLIENT, h, &r, &why);
        out.push_back(fmt("held creature %llu: handle 0x%016llX is %s", (unsigned long long)id, (unsigned long long)h,
                          r == KY_OK ? "live" : ("not live (" + why + ")").c_str()));
    }
    return out;
}

}  // namespace kyubu::game
