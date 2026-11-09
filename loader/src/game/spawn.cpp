// The spawn primitive: building a creature with the game's own recipe (openspec add-entity-spawner, design D3;
// mapping/atlas/primitives.md, Spawn, steps 1 to 7, and Behavior, step 8).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "call.h"
#include "cubeworld/functions.h"
#include "game.h"
#include "log.h"
#include "symbols.h"
#include "world.h"

namespace kyubu::game {

namespace {

static_assert(sizeof(KyubuSpawn) == 64 && offsetof(KyubuSpawn, level) == 12 && offsetof(KyubuSpawn, pos) == 16 &&
                  offsetof(KyubuSpawn, body_yaw) == 40 && offsetof(KyubuSpawn, path_count) == 44 &&
                  offsetof(KyubuSpawn, owner) == 48 && offsetof(KyubuSpawn, path) == 56,
              "KyubuSpawn's layout is part of the ABI");

// The ids of creatures built inside a World step whose insert waits for the after_step safe point. The id rule
// counts them, so two spawns in one step get different ids.
std::mutex g_pending_mu;
std::vector<int64_t> g_pending;

// One waiting insert: its id is on the pending list while the queued task lives. A task dropped unrun (the world was
// left first) still takes the id off; that creature never reaches the map, and leaving the world already invalidated
// its handle.
struct Pending {
    const int64_t id;
    bool inserted = false;
    explicit Pending(int64_t i) : id(i) {
        std::lock_guard<std::mutex> g(g_pending_mu);
        g_pending.push_back(id);
    }
    ~Pending() {
        {
            std::lock_guard<std::mutex> g(g_pending_mu);
            auto it = std::find(g_pending.begin(), g_pending.end(), id);
            if (it != g_pending.end()) g_pending.erase(it);
        }
        if (!inserted) KWARN("game API: creature_spawn: the creature with id %lld, built during a World step, was dropped "
                             "with the world before its insert", (long long)id);
    }
    Pending(const Pending&) = delete;
    Pending& operator=(const Pending&) = delete;
};

// The game's id rule (step 1): the lowest id, but not above 0, minus 1. The map compares ids as signed 64-bit, so
// its leftmost node (the head's left) holds the lowest; its key is at the node's +0x10.
int64_t next_id(const cube_World* w) {
    int64_t lowest = 0;
    const std_tree_node* head = w->creatures.head;
    if (head && w->creatures.size > 0 && !head->left->isnil)
        lowest = std::min(lowest, *(const int64_t*)((const uint8_t*)head->left + sizeof(std_tree_node)));
    std::lock_guard<std::mutex> g(g_pending_mu);
    for (int64_t id : g_pending) lowest = std::min(lowest, id);
    return lowest - 1;
}

// The recipe's game functions, from the build's symbol map (the primitive is available, so each is verified).
struct Recipe {
    cw_operator_new_fn alloc = (cw_operator_new_fn)verified_symbol(CW_SYM_operator_new);
    cw_cube_Creature_ctor_fn construct = (cw_cube_Creature_ctor_fn)verified_symbol(CW_SYM_cube_Creature_ctor);
    cw_cube_Creature_maxHp_fn max_hp = (cw_cube_Creature_maxHp_fn)verified_symbol(CW_SYM_cube_Creature_maxHp);
    cw_creature_appearance_for_type_fn look =
        (cw_creature_appearance_for_type_fn)verified_symbol(CW_SYM_creature_appearance_for_type);
    cw_creature_map_slot_fn slot = (cw_creature_map_slot_fn)verified_symbol(CW_SYM_creature_map_slot);
    bool complete() const { return alloc && construct && max_hp && look && slot; }
};

// Step 8 (behaviour primitives): the game builds a cube::SequentialBehavior, constructs each child and appends it to
// the sequence's list (at +4) with the folded std::list<T*>::push_back, then stores the sequence in the creature
// (0x60EBAD for the zone default, 0x5ED1D0 for a pet, 0x61423D for a summon). Every object comes from the game's
// allocator, since ~Creature frees the sequence with operator_delete. The sizes are the constructors' (probes
// spawn-behavior and spawn-behavior-all).
struct BehaviorRecipe {
    cw_cube_SequentialBehavior_ctor_fn sequence =
        (cw_cube_SequentialBehavior_ctor_fn)verified_symbol(CW_SYM_cube_SequentialBehavior_ctor);
    cw_cube_CombatBehavior_ctor_fn combat = (cw_cube_CombatBehavior_ctor_fn)verified_symbol(CW_SYM_cube_CombatBehavior_ctor);
    cw_cube_LookAtPlayerBehavior_ctor_fn look_at =
        (cw_cube_LookAtPlayerBehavior_ctor_fn)verified_symbol(CW_SYM_cube_LookAtPlayerBehavior_ctor);
    cw_cube_RandomWalkBehavior_ctor_fn walk =
        (cw_cube_RandomWalkBehavior_ctor_fn)verified_symbol(CW_SYM_cube_RandomWalkBehavior_ctor);
    cw_std_list_push_back_fn append = (cw_std_list_push_back_fn)verified_symbol(CW_SYM_std_list_push_back);
    // `behavior_companion` and `behavior_path` (checked only for the kinds that use them)
    cw_cube_CompanionBehavior_ctor_fn companion =
        (cw_cube_CompanionBehavior_ctor_fn)verified_symbol(CW_SYM_cube_CompanionBehavior_ctor);
    cw_cube_WalkPathBehavior_ctor_fn path = (cw_cube_WalkPathBehavior_ctor_fn)verified_symbol(CW_SYM_cube_WalkPathBehavior_ctor);
    cw_WaypointVector_push_back_fn waypoint = (cw_WaypointVector_push_back_fn)verified_symbol(CW_SYM_WaypointVector_push_back);
    bool complete() const { return sequence && combat && look_at && walk && append; }
    bool has_companion() const { return companion != nullptr; }
    bool has_path() const { return path && waypoint; }
};

constexpr uint32_t kSequenceSize = 12, kCombatSize = 20, kLookAtSize = 4, kWalkSize = 8, kCompanionSize = 16, kPathSize = 28;
constexpr uint32_t kSequenceListOffset = 4;    // the std::list of children inside a cube::SequentialBehavior
constexpr uint32_t kCompanionOwnerOffset = 8;  // the owner's creature id (uint64) inside a cube::CompanionBehavior
constexpr uint32_t kPathVectorOffset = 4;      // the waypoint vector inside a cube::WalkPathBehavior
constexpr float kCombatValue = 20.0f;          // what every combat behaviour the game builds gets
constexpr float kPathValue = 2.0f;             // what every walk path but the leader's gets

enum class Child { combat, look_at, walk, companion, path };

// The kind's children in the game's order: the zone default (monster), a creature that looks at the player, wanders and
// fights what is hostile to it when that comes close (friendly: the same sequence as the monster, so what it fights is
// the hostile type's doing, as for the game's own villagers), a pet or summon (companion: it fights beside its owner),
// a guard (it fights and walks its path).
std::vector<Child> children_of(uint8_t kind) {
    switch (kind) {
        case KY_SPAWN_MONSTER: return {Child::combat, Child::look_at, Child::walk};
        case KY_SPAWN_FRIENDLY: return {Child::combat, Child::look_at, Child::walk};
        case KY_SPAWN_COMPANION: return {Child::combat, Child::companion};
        case KY_SPAWN_GUARD: return {Child::combat, Child::path};
        default: return {};
    }
}

// What a kind's behaviour needs from the spawn: the owner's creature id for a companion, the waypoints for a guard.
struct BehaviorArgs {
    int64_t owner_id = 0;
    const int64_t (*path)[3] = nullptr;
    uint32_t path_count = 0;
};

// The game's allocator hands out uninitialised blocks, and the game reaches its creatures' behaviours through a clone
// (the copy constructors write every field), so the constructors of WalkPathBehavior (+0x14) and CompanionBehavior
// (+4) leave fields unwritten: the block is zeroed first, or a path creature does not walk (probe spawn-behavior-all).
void* alloc_zeroed(const Recipe& r, uint32_t size) {
    void* block = r.alloc(size);
    memset(block, 0, size);
    return block;
}

void* make_child(const Recipe& r, const BehaviorRecipe& b, Child which, const BehaviorArgs& a) {
    switch (which) {
        case Child::combat: return b.combat((cube_CombatBehavior*)alloc_zeroed(r, kCombatSize), nullptr, kCombatValue);
        case Child::look_at: return b.look_at((cube_LookAtPlayerBehavior*)alloc_zeroed(r, kLookAtSize), nullptr);
        case Child::walk: return b.walk((cube_RandomWalkBehavior*)alloc_zeroed(r, kWalkSize), nullptr);
        case Child::companion: {
            void* c = b.companion((cube_CompanionBehavior*)alloc_zeroed(r, kCompanionSize), nullptr);
            *(int64_t*)((uint8_t*)c + kCompanionOwnerOffset) = a.owner_id;   // as the pet and the summon write it
            return c;
        }
        case Child::path: {
            void* p = b.path((cube_WalkPathBehavior*)alloc_zeroed(r, kPathSize), nullptr, kPathValue);
            for (uint32_t i = 0; i < a.path_count; ++i) {
                cube_LongVector3 v;
                v.x = a.path[i][0];
                v.y = a.path[i][1];
                v.z = a.path[i][2];
                b.waypoint((uint8_t*)p + kPathVectorOffset, nullptr, &v);
            }
            return p;
        }
    }
    return nullptr;
}

void give_behavior(const Recipe& r, const BehaviorRecipe& b, cube_Creature* c, uint8_t kind, const BehaviorArgs& a) {
    auto* sequence = b.sequence((cube_SequentialBehavior*)alloc_zeroed(r, kSequenceSize), nullptr);
    void* list = (uint8_t*)sequence + kSequenceListOffset;
    // each child is constructed, then appended (the append takes the address of the child pointer), as in the game
    for (Child which : children_of(kind)) {
        void* child = make_child(r, b, which, a);
        b.append(list, nullptr, &child);
    }
    c->behavior = (cube_Behavior*)sequence;
}

// Step 7: the map's operator[] finds or makes the id's slot, and the creature goes in it.
void insert(cw_creature_map_slot_fn slot, cube_World* w, int64_t id, cube_Creature* c) {
    *slot(&w->creatures, nullptr, (const uint64_t*)&id) = c;
}

// Why a spawn request is refused, naming the field; empty when every field is in range.
std::string invalid_field(const KyubuSpawn& s) {
    if (s.size < sizeof(KyubuSpawn)) return fmt("size is %u, below %u (sizeof(KyubuSpawn))", s.size, (unsigned)sizeof(KyubuSpawn));
    if (!enum_value_name("cube::EntityType", s.entity_type))
        return fmt("entity_type is %u, not a value of cube::EntityType (game_enum lists them)", s.entity_type);
    if (s.hostile_type == 0) return "hostile_type is 0 (a player); a spawn takes another cube::HostileType value";
    if (!enum_value_name("cube::HostileType", s.hostile_type))
        return fmt("hostile_type is %u, not a value of cube::HostileType", (unsigned)s.hostile_type);
    if (s.behavior > KY_SPAWN_DEFAULT)
        return fmt("behavior is %u; this loader knows KY_SPAWN_STILL (0) to KY_SPAWN_DEFAULT (%u)", (unsigned)s.behavior,
                   KY_SPAWN_DEFAULT);
    if (s.behavior == KY_SPAWN_COMPANION && s.owner == 0)
        return "owner required: KY_SPAWN_COMPANION needs the handle of the creature it follows in owner";
    if (s.behavior == KY_SPAWN_GUARD) {
        if (s.path_count == 0 || !s.path)
            return fmt("path required: KY_SPAWN_GUARD needs 1 to %u waypoints in path (path_count is %u)", KY_SPAWN_PATH_MAX,
                       s.path_count);
        if (s.path_count > KY_SPAWN_PATH_MAX)
            return fmt("path_count is %u, above %u", s.path_count, KY_SPAWN_PATH_MAX);
    }
    if (s.reserved != 0) return fmt("reserved is %u, must be 0", (unsigned)s.reserved);
    if (s.level < 1) return fmt("level is %d, must be 1 or more", s.level);
    if (!std::isfinite(s.body_yaw)) return "body_yaw is not a finite number";
    return "";
}

KyubuResult KYUBU_CALL k_creature_spawn(KyubuModHandle self, const KyubuSpawn* spawn, KyubuCreature* out) {
    if (out) *out = 0;
    GameCall g(self, Prim::spawn, "creature_spawn");   // server-only (kSideRules)
    if (!g) return g.result();
    // it writes Entity state's fields and hands out a handle: both are checked before anything is built
    if (!g.need(Prim::entity_state) || !g.need(Prim::handles)) return g.result();
    if (!spawn || !out) return g.refuse(KY_E_INVALID_ARG, spawn ? "out is NULL" : "spawn is NULL");
    const std::string bad = invalid_field(*spawn);
    if (!bad.empty()) return g.refuse(KY_E_INVALID_ARG, bad);
    // the game's default for a target (hostile type 6) is no behaviour at all (0x60EBAD); any other creature gets the
    // zone default, which is what KY_SPAWN_MONSTER builds
    uint8_t kind = spawn->behavior;
    if (kind == KY_SPAWN_DEFAULT) kind = spawn->hostile_type == 6 ? KY_SPAWN_STILL : KY_SPAWN_MONSTER;
    // a behaviour is built with the `behavior` primitive's entries, and the kinds with a class of their own also need
    // theirs; only what the kind uses is checked
    if (kind != KY_SPAWN_STILL && !g.need(Prim::behavior)) return g.result();
    if (kind == KY_SPAWN_COMPANION && !g.need(Prim::behavior_companion)) return g.result();
    if (kind == KY_SPAWN_GUARD && !g.need(Prim::behavior_path)) return g.result();
    const Recipe r;
    if (!r.complete()) return g.refuse(KY_E_INTERNAL, "a function of the spawn recipe did not resolve");
    const BehaviorRecipe b;
    if ((kind != KY_SPAWN_STILL && !b.complete()) || (kind == KY_SPAWN_COMPANION && !b.has_companion()) ||
        (kind == KY_SPAWN_GUARD && !b.has_path()))
        return g.refuse(KY_E_INTERNAL, "a function of the behaviour recipe did not resolve");
    // a companion's owner is a handle on this side; the behaviour keeps its id, not the pointer
    BehaviorArgs args;
    if (kind == KY_SPAWN_COMPANION) {
        const cube_Creature* owner = g.creature(spawn->owner);
        if (!owner) return g.result();
        args.owner_id = (int64_t)owner->id;
    }
    if (kind == KY_SPAWN_GUARD) {
        args.path = spawn->path;
        args.path_count = spawn->path_count;
    }

    cube_World* w = g.world();
    const int64_t id = next_id(w);                                        // 1. the id
    auto* c = (cube_Creature*)r.alloc(sizeof(cube_Creature));             // 2. the game's allocator, 0x1E60 bytes
    r.construct(c, nullptr, (const uint64_t*)&id);                        // 3. the constructor, with the id
    cube_CreatureData& d = c->data;                                       // 4. the fields
    d.hostile_type = spawn->hostile_type;
    d.entity_type = spawn->entity_type;
    d.level = spawn->level;
    d.pos.x = spawn->pos[0];
    d.pos.y = spawn->pos[1];
    d.pos.z = spawn->pos[2];
    d.body_yaw = spawn->body_yaw;
    // the game's pet (0x60D1D3) and summon (0x61423D) paths store their owner's id in the creature as well as in the
    // behaviour, and the combat step reads it: a pet (hostile type 5) with an owner takes its owner's victims as targets
    if (kind == KY_SPAWN_COMPANION) d.parent_owner = (uint64_t)args.owner_id;
    d.hp = r.max_hp(c, nullptr);                                          // 5. hp from max hp
    r.look(&d.entity_type, &d.appearance, nullptr);                       // 6. the look (style NULL: a random variation)
    if (kind != KY_SPAWN_STILL) give_behavior(r, b, c, kind, args);       // 8. the behaviour, before the insert
    if (in_step()) {                                                      // 7. the insert: after the step inside one
        auto pending = std::make_shared<Pending>(id);
        after_step([slot = r.slot, w, id, c, pending] {
            insert(slot, w, id, c);
            pending->inserted = true;
        });
    } else {
        insert(r.slot, w, id, c);
    }
    *out = g.handle(c);
    return g.result();
}

}  // namespace

void fill_spawn(KyubuApi& a) { a.creature_spawn = k_creature_spawn; }

}  // namespace kyubu::game
