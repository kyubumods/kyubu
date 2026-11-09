#include "doctest.h"

#include "abilities.h"
#include "ability_bar.h"
#include "ability_rank_bind.h"
#include "ability_step.h"
#include "api.h"
#include "call.h"
#include "cubeworld/layout.h"
#include "events.h"
#include "game.h"
#include "handles.h"
#include "loader.h"
#include "log.h"
#include "luart.h"
#include "modmanager.h"
#include "creature_flags.h"
#include "lights.h"
#include "terrain.h"
#include "game/particles.h"   // not "particles.h": tools/animator has one too, and comes first on the include path
#include "registry.h"
#include "symbols.h"
#include "world.h"
#include "world_step.h"
#include "shots.h"
#include "server_hits.h"

#include <limits>
#include <cmath>
#include <functional>
#include <filesystem>
#include <thread>

using namespace kyubu;

#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

// Stand-ins for the game functions the base detours, and for Cube.exe's controller global. The World step has
// cube::World::update's ABI (__thiscall, three stack arguments) and runs g_inside_step as the step's own work.
static void (*volatile g_inside_step)() = nullptr;
extern "C" NOINLINE void __fastcall game_test_world_update(cube_World* self, void*, int32_t dt, void* out, void* in) {
    volatile int x = dt;
    (void)x; (void)self; (void)out; (void)in;
    if (g_inside_step) g_inside_step();
}
static void (*volatile g_dtor_ran)(cube_Creature*) = nullptr;
extern "C" NOINLINE void __fastcall game_test_creature_dtor(cube_Creature* self, void*) {
    if (g_dtor_ran) g_dtor_ran(self);
}
static cube_GameController* game_test_controller = nullptr;

// Stand-ins for the spawn recipe's five game functions (primitives.md, Spawn), with the game's ABIs. Each writes its
// call to g_recipe (when set), so a test sees the order, the arguments, and whether the insert ran inside a step. The
// allocator keeps what it gave out in g_allocated; the map's operator[] hands out slots of g_slots and remembers the
// map it was called on.
static std::vector<std::string>* g_recipe = nullptr;
static std::vector<void*> g_allocated;
static cube_Creature* g_slots[256] = {};
static int g_slot_count = 0;
static std_map* g_slot_map = nullptr;
extern "C" NOINLINE void* __cdecl game_test_operator_new(uint32_t size) {
    if (g_recipe) g_recipe->push_back(fmt("new 0x%X", size));
    void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (size < 0x100) memset(p, 0xCD, size);   // a behaviour object: the game's allocator does not clear it either
    g_allocated.push_back(p);
    return p;
}
extern "C" NOINLINE cube_Creature* __fastcall game_test_creature_ctor(cube_Creature* self, void*, const uint64_t* id) {
    self->id = *id;
    if (g_recipe) g_recipe->push_back(fmt("construct %lld", (long long)*id));
    return self;
}
extern "C" NOINLINE float __fastcall game_test_max_hp(cube_Creature* self, void*) {
    if (g_recipe) g_recipe->push_back(fmt("max hp: type %u, level %d", self->data.entity_type, self->data.level));
    return 100.0f + (float)self->data.level;
}
extern "C" NOINLINE void __cdecl game_test_appearance(uint32_t* type, cube_AppearanceData* look, const void* style) {
    if (g_recipe) g_recipe->push_back(fmt("look %u%s", *type, style ? ", with a style" : ""));
    *(uint8_t*)look = 0xA5;
}
extern "C" NOINLINE cube_Creature** __fastcall game_test_map_slot(std_map* self, void*, const uint64_t* id) {
    g_slot_map = self;
    if (g_recipe) g_recipe->push_back(fmt("insert %lld, in a step: %d", (long long)*id, kyubu::game::in_step() ? 1 : 0));
    return &g_slots[g_slot_count++ % 256];
}

// Stand-ins for the behaviour recipe's six functions (primitives.md, Behavior): the constructors log their call, and
// the append logs which allocation it appends to which (by the order of the allocator's blocks: 0 is the creature).
static int alloc_index(const void* p) {
    for (size_t i = 0; i < g_allocated.size(); ++i)
        if (g_allocated[i] == p) return (int)i;
    return -1;
}
extern "C" NOINLINE cube_SequentialBehavior* __fastcall game_test_sequence_ctor(cube_SequentialBehavior* self, void*) {
    if (g_recipe) g_recipe->push_back(fmt("sequence %d", alloc_index(self)));
    return self;
}
extern "C" NOINLINE cube_CombatBehavior* __fastcall game_test_combat_ctor(cube_CombatBehavior* self, void*, float value) {
    if (g_recipe) g_recipe->push_back(fmt("combat %d, value %.1f", alloc_index(self), value));
    return self;
}
extern "C" NOINLINE cube_LookAtPlayerBehavior* __fastcall game_test_look_at_ctor(cube_LookAtPlayerBehavior* self, void*) {
    if (g_recipe) g_recipe->push_back(fmt("look at player %d", alloc_index(self)));
    return self;
}
extern "C" NOINLINE cube_RandomWalkBehavior* __fastcall game_test_walk_ctor(cube_RandomWalkBehavior* self, void*) {
    if (g_recipe) g_recipe->push_back(fmt("random walk %d", alloc_index(self)));
    return self;
}
extern "C" NOINLINE cube_CompanionBehavior* __fastcall game_test_companion_ctor(cube_CompanionBehavior* self, void*) {
    if (g_recipe) g_recipe->push_back(fmt("companion %d, +4 %08X", alloc_index(self), *(const uint32_t*)((const uint8_t*)self + 4)));
    return self;
}
extern "C" NOINLINE cube_WalkPathBehavior* __fastcall game_test_path_ctor(cube_WalkPathBehavior* self, void*, float value) {
    if (g_recipe)
        g_recipe->push_back(fmt("walk path %d, value %.1f, +0x14 %08X", alloc_index(self), value,
                                *(const uint32_t*)((const uint8_t*)self + 0x14)));
    return self;
}
extern "C" NOINLINE void __fastcall game_test_waypoint(void* vec, void*, const cube_LongVector3* v) {
    if (g_recipe)
        g_recipe->push_back(fmt("waypoint to %d: %lld, %lld, %lld", alloc_index((uint8_t*)vec - 4), (long long)v->x,
                                (long long)v->y, (long long)v->z));
}
extern "C" NOINLINE void __fastcall game_test_push_back(void* list, void*, void** value) {
    if (!g_recipe) return;
    const int seq = alloc_index((uint8_t*)list - 4);
    g_recipe->push_back(fmt("append %d to the list of %d", alloc_index(*value), seq));
}

// Stand-in for the game's list push of the World step output's particle list (primitives.md, Particles): thiscall(list, particle).
struct ParticlePush {
    void* list;
    cube_ParticleData data;
};
static std::vector<ParticlePush>* g_particle_pushes = nullptr;
extern "C" NOINLINE void __fastcall game_test_push_particle(void* list, void*, const cube_ParticleData* p) {
    if (g_particle_pushes) g_particle_pushes->push_back({list, *p});
}

// Stand-in for std::list<cube::Projectile>::push_back (primitives.md, Player shots): thiscall(list, projectile).
struct ProjectilePush {
    void* list;
    cube_Projectile p;
};
static std::vector<ProjectilePush>* g_projectile_pushes = nullptr;
extern "C" NOINLINE void __fastcall game_test_push_projectile(void* list, void*, const cube_Projectile* p) {
    if (g_projectile_pushes) g_projectile_pushes->push_back({list, *p});
}

// Stand-in for the push_back of the frame's light vector (primitives.md, Lights): thiscall(vector, light).
struct LightPush {
    void* vector;
    cube_Light light;
};
static std::vector<LightPush>* g_light_pushes = nullptr;
extern "C" NOINLINE void __fastcall game_test_push_light(void* vector, void*, const cube_Light* l) {
    if (g_light_pushes) g_light_pushes->push_back({vector, *l});
}

// Stand-in for cube::World::hitCreature (primitives.md, World step): records its fourteen arguments and, as the game's normal path
// does, passes hitListSite with its cube::Hit at ESP+0xC4 (a stun chance that came up already in it), then returns g_hit_result.
struct HitCall {
    cube_World* world;
    cube_Creature* target;
    cube_Creature* attacker;
    float damage, chance;
    int32_t critical, powered, magic, ability, allow_block;
    const cube_LongVector3* position;
    cube_FloatVector3 direction;
    void* out;
    void* zones;
    void* projectile;
    cube_Hit hit;   // the Hit as the list site left it
};
static std::vector<HitCall>* g_hit_calls = nullptr;
static bool g_hit_result = true;
extern "C" NOINLINE bool __fastcall game_test_hit_creature(cube_World* self, void*, cube_Creature* target, cube_Creature* attacker,
                                                          float damage, int32_t critical, int32_t powered, float chance,
                                                          const cube_LongVector3* position, const cube_FloatVector3* direction,
                                                          void* out, void* zones, int32_t magic, int32_t ability, void* projectile,
                                                          int32_t allow_block) {
    alignas(8) uint8_t stack[0xC4 + sizeof(cube_Hit)] = {};
    cube_Hit* hit = (cube_Hit*)(stack + 0xC4);
    hit->stun_duration = 1234;   // what a stun chance that came up leaves: the target's maximum, and z 10
    hit->hit_dir.z = 10.0f;
    hit->damage = damage;
    KyubuCpuContext ctx = {};
    ctx.esp = (uint32_t)(uintptr_t)stack;
    kyubu::game::step_on_list_site(&ctx);
    if (g_hit_calls)
        g_hit_calls->push_back({self, target, attacker, damage, chance, critical, powered, magic, ability, allow_block, position,
                                *direction, out, zones, projectile, *hit});
    return g_hit_result;
}

namespace {

std::string code_bytes(const void* f, int n = 24) {
    std::string s;
    for (int i = 0; i < n; ++i) s += fmt("%02X ", ((const uint8_t*)f)[i]);
    return s;
}

// A symbol map of the test build with the base's two functions, Cube.exe's controller global and the spawn recipe's
// five functions.
void load_base_map(const char* update_status, bool with_controller = true, const char* dtor_status = "verified",
                   const char* spawn_status = "verified", const char* behavior_status = "verified",
                   const char* extra_status = "verified") {
    auto& t = SymbolTable::get();
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    t.clear();
    t.set_image(base, base);
    auto rva = [&](const void* p) { return (uint32_t)((uintptr_t)p - base); };
    std::string text = fmt(R"(build = "test-build"
[[symbol]]
name = "cube::World::update"
rva = %u
signature = "%s"
convention = "thiscall"
status = "%s"

[[symbol]]
name = "cube::Creature::~Creature"
rva = %u
signature = "%s"
convention = "thiscall"
status = "%s"
)", rva((const void*)&game_test_world_update), code_bytes((const void*)&game_test_world_update).c_str(), update_status,
                           rva((const void*)&game_test_creature_dtor), code_bytes((const void*)&game_test_creature_dtor).c_str(),
                           dtor_status);
    if (with_controller)
        text += fmt("\n[[global]]\nname = \"cube::game_controller\"\nrva = %u\nstatus = \"verified\"\n", rva(&game_test_controller));
    const struct { const char* name; const void* fn; const char* convention; } recipe[] = {
        {"operator_new", (const void*)&game_test_operator_new, "cdecl"},
        {"cube::Creature::Creature", (const void*)&game_test_creature_ctor, "thiscall"},
        {"cube::Creature::maxHp", (const void*)&game_test_max_hp, "thiscall"},
        {"creature_appearance_for_type", (const void*)&game_test_appearance, "cdecl"},
        {"creature_map_slot", (const void*)&game_test_map_slot, "thiscall"},
    };
    for (auto& f : recipe)
        text += fmt("\n[[symbol]]\nname = \"%s\"\nrva = %u\nsignature = \"%s\"\nconvention = \"%s\"\nstatus = \"%s\"\n",
                    f.name, rva(f.fn), code_bytes(f.fn).c_str(), f.convention, spawn_status);
    const struct { const char* name; const void* fn; } behavior[] = {
        {"cube::SequentialBehavior::SequentialBehavior", (const void*)&game_test_sequence_ctor},
        {"cube::CombatBehavior::CombatBehavior", (const void*)&game_test_combat_ctor},
        {"cube::LookAtPlayerBehavior::LookAtPlayerBehavior", (const void*)&game_test_look_at_ctor},
        {"cube::RandomWalkBehavior::RandomWalkBehavior", (const void*)&game_test_walk_ctor},
        {"std::list::push_back", (const void*)&game_test_push_back},
    };
    for (auto& f : behavior)
        text += fmt("\n[[symbol]]\nname = \"%s\"\nrva = %u\nsignature = \"%s\"\nconvention = \"thiscall\"\nstatus = \"%s\"\n",
                    f.name, rva(f.fn), code_bytes(f.fn).c_str(), behavior_status);
    // the companion and walk-path classes (their own primitives, behavior_companion and behavior_path)
    const struct { const char* name; const void* fn; } extras[] = {
        {"cube::CompanionBehavior::CompanionBehavior", (const void*)&game_test_companion_ctor},
        {"cube::WalkPathBehavior::WalkPathBehavior", (const void*)&game_test_path_ctor},
        {"WaypointVector::push_back", (const void*)&game_test_waypoint},
        {"step_output_push_particle", (const void*)&game_test_push_particle},
        {"light_vector_push", (const void*)&game_test_push_light},
        {"cube::World::hitCreature", (const void*)&game_test_hit_creature},
        {"std_list_projectile_push_back", (const void*)&game_test_push_projectile},
    };
    for (auto& f : extras)
        text += fmt("\n[[symbol]]\nname = \"%s\"\nrva = %u\nsignature = \"%s\"\nconvention = \"thiscall\"\nstatus = \"%s\"\n",
                    f.name, rva(f.fn), code_bytes(f.fn).c_str(), extra_status);
    std::vector<std::string> errors;
    t.load_text(text, "game-test.toml", "test-build", "kyubu", errors);
    REQUIRE(errors.empty());
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("game-api: entry not verified on this build") {
    // covers: game-api/Entry not verified on this build
    Loader& l = L();
    const HostInfo saved = l.host;
    l.host.host = KY_HOST_CLIENT;
    l.host.build_id = "test-build";
    const KyubuApi* api = api_table();
    char why[256] = {};

    load_base_map("candidate");
    const std::string before = code_bytes((const void*)&game_test_world_update);
    const uint64_t seq = Log::get().sequence();
    game::boot();
    CHECK(api->game_available("world", why, sizeof why) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(contains(why, "cube::World::update: candidate, not verified (test-build)"), why);
    CHECK(api->game_available("handles", why, sizeof why) == KY_E_UNAVAILABLE);   // every primitive needs world
    CHECK_MESSAGE(contains(why, "needs world"), why);
    CHECK(api->game_available("world", nullptr, 0) == KY_E_UNAVAILABLE);
    CHECK(api->game_available("no_such_primitive", why, sizeof why) == KY_E_NOT_FOUND);
    CHECK(api->game_available(nullptr, why, sizeof why) == KY_E_INVALID_ARG);
    // the log names the primitive and the entry once
    int named = 0;
    for (auto& line : Log::get().since(seq))
        if (contains(line.text, "game API: world is unavailable") && contains(line.text, "cube::World::update")) ++named;
    CHECK(named == 1);
    // the game's memory is unchanged: nothing hooked the unverified function
    CHECK(code_bytes((const void*)&game_test_world_update) == before);

    // every entry verified: both primitives are available
    load_base_map("verified");
    game::check_entries();
    CHECK_MESSAGE(api->game_available("world", why, sizeof why) == KY_OK, why);
    CHECK(api->game_available("handles", why, sizeof why) == KY_OK);

    // Cube.exe's entries only block Cube.exe: without the controller global, Server.exe still has a World
    load_base_map("verified", false);
    game::check_entries();
    CHECK(api->game_available("world", why, sizeof why) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(contains(why, "cube::game_controller: not in the symbol map"), why);
    l.host.host = KY_HOST_SERVER;
    game::check_entries();
    CHECK(api->game_available("world", why, sizeof why) == KY_OK);

    SymbolTable::get().clear();
    l.host = saved;
    game::check_entries();
}

TEST_CASE("game-api: primitive limited to some builds") {
    // covers: game-api/Limited primitive on the other build (limit_builds_for_test stands in for a GAME_BUILDS line)
    Loader& l = L();
    const HostInfo saved = l.host;
    l.host.build_id = "test-build";
    const KyubuApi* api = api_table();
    char why[256] = {};
    load_base_map("verified", false);
    game::limit_builds_for_test(game::Prim::handles, 1u << KY_HOST_CLIENT);

    l.host.host = KY_HOST_SERVER;
    const uint64_t seq = Log::get().sequence();
    game::check_entries();
    CHECK(api->game_available("handles", why, sizeof why) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(std::string(why) == "not mapped for Server.exe yet", why);
    CHECK(api->game_available("world", why, sizeof why) == KY_OK);   // the other primitives are not limited
    int named = 0;
    for (auto& line : Log::get().since(seq))
        if (contains(line.text, "game API: handles is unavailable") && contains(line.text, "not mapped for Server.exe")) ++named;
    CHECK(named == 1);

    // on its own build it is available again; a limit to Server.exe blocks Cube.exe
    load_base_map("verified");
    l.host.host = KY_HOST_CLIENT;
    game::check_entries();
    CHECK_MESSAGE(api->game_available("handles", why, sizeof why) == KY_OK, why);
    game::limit_builds_for_test(game::Prim::handles, 1u << KY_HOST_SERVER);
    game::check_entries();
    CHECK(api->game_available("handles", why, sizeof why) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(std::string(why) == "not mapped for Cube.exe yet", why);

    // taking the limit back restores entries.inc's own rule (handles has no GAME_BUILDS line)
    game::limit_builds_for_test(game::Prim::handles, 0);
    game::check_entries();
    CHECK(api->game_available("handles", why, sizeof why) == KY_OK);

    SymbolTable::get().clear();
    l.host = saved;
    game::check_entries();
}

TEST_CASE("game-api: type entries and the loader's layouts") {
    // covers: game-api/Candidate enum (the boot check: a `type` entry naming a candidate enum blocks its primitive)
    // Whatever the mapping says today, each compiled enum and struct is judged by its own status.
    int verified = 0, candidate = 0;
    for (auto& en : cw_layout_enums) {
        if (!*en.name) continue;
        const std::string p = game::type_problem(en.name);
        if (en.verified) { CHECK_MESSAGE(p.empty(), en.name); ++verified; }
        else { CHECK_MESSAGE(p == "candidate in the loader's layouts", en.name); ++candidate; }
    }
    CHECK(verified + candidate == CW_LAYOUT_ENUM_COUNT);
    for (auto& t : cw_layout_types)
        if (*t.name) CHECK_MESSAGE(game::type_problem(t.name).empty() == (t.verified != 0), t.name);
    CHECK(game::type_problem("cube::NoSuchType") == "not in the loader's layouts");
}

TEST_CASE("game-api: a field entry is judged by the field's own status, not its struct's") {
    // cube::AppearanceData is a candidate struct with a verified `scale` (creature_state's box, 10.7) and candidate fields
    CHECK(game::type_problem("cube::AppearanceData") == "candidate in the loader's layouts");
    CHECK(game::field_problem("cube::AppearanceData.scale").empty());
    CHECK(game::field_problem("cube::AppearanceData.hair_red") == "candidate in the loader's layouts");
    CHECK(game::field_problem("cube::AppearanceData.no_such_field") == "not in the loader's layouts");
    CHECK(game::field_problem("cube::CreatureData.ray_hit").empty());
}

namespace {

const CwLayoutEnum* compiled_enum(const char* name) {
    for (auto& en : cw_layout_enums)
        if (std::string(en.name) == name) return &en;
    return nullptr;
}

using EnumValues = std::vector<std::pair<int64_t, std::string>>;

// Every value game_enum hands out for an enum, in its order.
EnumValues enum_values(const char* name) {
    EnumValues out;
    char buf[128];
    int64_t v = 0;
    for (uint32_t i = 0; api_table()->game_enum(name, i, &v, buf, sizeof buf) == KY_OK; ++i) out.push_back({v, buf});
    return out;
}

// The same enum's values from the loader's layouts, in value order.
EnumValues layout_values(const CwLayoutEnum& en) {
    EnumValues out;
    for (uint32_t i = 0; i < en.value_count; ++i) out.push_back({en.values[i].value, en.values[i].name});
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

// A fresh folder under %TEMP% for one test.
std::string temp_dir(const char* name) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::filesystem::path p = std::filesystem::path(tmp) / (std::string("kyubu-game-") + name + "-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p);
    return narrow(p.wstring());
}

}  // namespace

TEST_CASE("game-api: entity types") {
    // covers: game-api/Entity types (the list; that creature_spawn accepts each value is tested with creature_spawn)
    const CwLayoutEnum* types = compiled_enum("cube::EntityType");
    REQUIRE(types);
    REQUIRE(types->verified);   // raised by the add-entity-spawner mapping (task 3.3)
    const EnumValues got = enum_values("cube::EntityType");
    CHECK(got.size() == types->value_count);
    CHECK(got == layout_values(*types));   // every value with its name
    for (size_t i = 1; i < got.size(); ++i) CHECK(got[i - 1].first <= got[i].first);   // in value order
    // any thread, any phase: no world is loaded here, and another thread gets the same list
    EnumValues other;
    std::thread t([&] { other = enum_values("cube::EntityType"); });
    t.join();
    CHECK(other == got);

    const KyubuApi* api = api_table();
    char buf[64] = "x";
    int64_t v = -1;
    CHECK(api->game_enum("cube::EntityType", types->value_count, &v, buf, sizeof buf) == KY_E_NOT_FOUND);   // past the last
    CHECK(std::string(buf).empty());
    CHECK(api->game_enum("cube::EntityType", 0, nullptr, buf, sizeof buf) == KY_OK);   // the value may be NULL
    CHECK(std::string(buf) == got[0].second);
    CHECK(api->game_enum("cube::EntityType", 0, &v, buf, 2) == KY_E_BUFFER);   // the name does not fit
    CHECK(api->game_enum("cube::EntityType", 0, &v, nullptr, 0) == KY_E_BUFFER);
    CHECK(api->game_enum("cube::NoSuchEnum", 0, &v, buf, sizeof buf) == KY_E_NOT_FOUND);
    CHECK_MESSAGE(std::string(buf) == "no enum 'cube::NoSuchEnum' in the loader's layouts", buf);
    CHECK(api->game_enum("", 0, &v, buf, sizeof buf) == KY_E_NOT_FOUND);
    CHECK(api->game_enum(nullptr, 0, &v, buf, sizeof buf) == KY_E_INVALID_ARG);
}

TEST_CASE("game-api: candidate enum") {
    // covers: game-api/Candidate enum. Each compiled enum is judged by its own status (cube::ClassType was a candidate
    // when this was written); a verified one lists exactly its values.
    const KyubuApi* api = api_table();
    int candidates = 0;
    for (auto& en : cw_layout_enums) {
        if (!*en.name) continue;
        if (en.verified) {
            CHECK_MESSAGE(enum_values(en.name) == layout_values(en), en.name);
            continue;
        }
        ++candidates;
        char why[256] = {};
        int64_t v = 12345;
        CHECK(api->game_enum(en.name, 0, &v, why, sizeof why) == KY_E_UNAVAILABLE);
        CHECK_MESSAGE(std::string(why) == std::string(en.name) + " is candidate in the loader's layouts, not verified", why);
        CHECK(v == 12345);   // no value handed out
        CHECK(api->game_enum(en.name, 0, &v, nullptr, 0) == KY_E_UNAVAILABLE);
    }
    WARN_MESSAGE(candidates > 0, "no candidate enum is left in the layouts: only verified ones were checked");
}

TEST_CASE("game-api: kyubu.game.enum in Lua") {
    // covers: game-api/Entity types and game-api/Candidate enum through the Lua binding, in a full-trust mod
    const CwLayoutEnum* types = compiled_enum("cube::EntityType");
    REQUIRE(types);
    const EnumValues expected = layout_values(*types);
    REQUIRE(!expected.empty());
    const CwLayoutEnum* candidate = nullptr;
    for (auto& en : cw_layout_enums)
        if (*en.name && !en.verified && !candidate) candidate = &en;

    Loader& l = L();
    const std::string saved_kyubu = l.kyubu_dir, saved_mods = l.mods_dir;
    const auto saved_host = l.host.host;
    const auto saved_mode = l.mode;
    const auto saved_phase = l.phase.load();
    const std::string game_dir = temp_dir("lua-enum");
    l.kyubu_dir = game_dir + "/kyubu";
    l.mods_dir = game_dir + "/mods";
    l.host.host = KY_HOST_CLIENT;
    l.mode = KY_MODE_MENU_OR_SINGLEPLAYER;
    l.phase = KY_PHASE_PREINIT;
    const std::string mod = l.mods_dir + "/fx_game_enum";
    REQUIRE(make_dirs(mod + "/scripts"));
    REQUIRE(write_text(mod + "/mod.toml", "id = \"fx_game_enum\"\nname = \"Game API: enums from Lua\"\n"
                                          "version = \"1.0.0\"\nkyubu = \"^1.0\"\nsides = [\"client\"]\ntrust = \"full\"\n"
                                          "description = \"Unit test: kyubu.game.enum.\"\n\n[scripts]\nclient = \"scripts/client.lua\"\n"));
    REQUIRE(write_text(mod + "/scripts/client.lua", std::string(R"(
local r = {}
local types, why = kyubu.game.enum("cube::EntityType")
if types then
  local ordered = true
  for i = 2, #types do ordered = ordered and types[i - 1].value <= types[i].value end
  r[#r + 1] = "types=" .. #types .. " ordered=" .. tostring(ordered) .. " first=" .. types[1].value .. "=" .. types[1].name
    .. " last=" .. types[#types].value .. "=" .. types[#types].name
else
  r[#r + 1] = "types=nil/" .. tostring(why)
end
local none, none_why = kyubu.game.enum("cube::NoSuchEnum")
r[#r + 1] = "unknown=" .. tostring(none) .. "/" .. tostring(none_why)
local candidate = ")") + (candidate ? candidate->name : "") + R"("
if candidate ~= "" then
  local c, c_why = kyubu.game.enum(candidate)
  r[#r + 1] = "candidate=" .. tostring(c) .. "/" .. tostring(c_why)
end
kyubu.info("fx_game_enum: " .. table.concat(r, " | "))
)"));

    const uint64_t seq = Log::get().sequence();
    auto& mm = ModManager::get();
    mm.discover_and_resolve();
    mm.load_native();
    LuaRuntime::get().load_all();
    std::string line;
    for (auto& entry : Log::get().since(seq))
        if (contains(entry.text, "fx_game_enum: ")) line = entry.text;
    Mod* m = mm.find("fx_game_enum");
    REQUIRE(m);
    CHECK(m->state == ModState::Loaded);
    std::string want = fmt("fx_game_enum: types=%u ordered=true first=%lld=%s last=%lld=%s | unknown=nil/no enum "
                           "'cube::NoSuchEnum' in the loader's layouts", (unsigned)expected.size(),
                           (long long)expected.front().first, expected.front().second.c_str(),
                           (long long)expected.back().first, expected.back().second.c_str());
    if (candidate) want += fmt(" | candidate=nil/%s is candidate in the loader's layouts, not verified", candidate->name);
    CHECK_MESSAGE(contains(line, want.c_str()), line);

    // leave no mods behind for other tests
    l.mods_dir = temp_dir("lua-enum-empty");
    mm.discover_and_resolve();
    l.kyubu_dir = saved_kyubu;
    l.mods_dir = saved_mods;
    l.host.host = saved_host;
    l.mode = saved_mode;
    l.phase = saved_phase;
}

namespace {

// One World step, the way the game makes one: through the (detoured) cube::World::update.
void step(cube_World* w) {
    auto volatile update = &game_test_world_update;
    update(w, nullptr, 20, nullptr, nullptr);
}

// A game-API function for the tests: sets the World's `joined` through the checks every function makes.
KyubuResult test_set_joined(KyubuSide side, std::string* why = nullptr) {
    game::WorldAccess w(game::Prim::world, side);
    if (!w) {
        if (why) *why = w.why();
        return w.result();
    }
    w.world()->joined = true;
    return KY_OK;
}

template <typename T>
T* alloc_zeroed() { return (T*)VirtualAlloc(nullptr, sizeof(T), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }

// A World's lock set up, and a creature map of two nodes under its head (the root, id 1, and its left child, id 2),
// each with the creature it maps to (the key at the node's +0x10, the value after it). Creature 1 is the local player.
struct FakeWorld {
    struct Node {
        std_tree_node n;
        uint64_t key;
        cube_Creature* value;
    };
    Node nodes[3] = {};
    cube_Creature* creatures[2] = {};
    void init(cube_World* w) {
        InitializeCriticalSection((CRITICAL_SECTION*)&w->lock);
        std_tree_node *head = &nodes[0].n, *root = &nodes[1].n, *left = &nodes[2].n;
        head->isnil = 1;
        head->parent = root; head->left = left; head->right = root;
        root->parent = head; root->left = left; root->right = head;
        left->parent = root; left->left = head; left->right = head;
        for (int i = 0; i < 2; ++i) {
            creatures[i] = alloc_zeroed<cube_Creature>();
            creatures[i]->id = i + 1;
            nodes[i + 1].key = i + 1;
            nodes[i + 1].value = creatures[i];
        }
        w->creatures.head = head;
        w->creatures.size = 2;
        w->local_player = creatures[0];
    }
    ~FakeWorld() {
        for (auto* c : creatures)
            if (c) VirtualFree(c, 0, MEM_RELEASE);
    }
};

// Cube.exe (single-player) with every entry verified and the World detoured; puts the loader back afterwards.
struct ClientGame {
    HostInfo saved_host = L().host;
    KyubuHostMode saved_mode = L().mode;
    cube_GameController* gc = alloc_zeroed<cube_GameController>();
    FakeWorld fake;
    ClientGame() {
        L().host.host = KY_HOST_CLIENT;
        L().host.build_id = "test-build";
        L().mode = KY_MODE_MENU_OR_SINGLEPLAYER;
        fake.init(&gc->world);
        game_test_controller = gc;
        load_base_map("verified");
        game::boot();
    }
    ~ClientGame() {
        game::remove_world_hooks();
        game::remove_handle_hooks();
        g_dtor_ran = nullptr;
        g_inside_step = nullptr;
        game_test_controller = nullptr;
        DeleteCriticalSection((CRITICAL_SECTION*)&gc->world.lock);
        VirtualFree(gc, 0, MEM_RELEASE);
        SymbolTable::get().clear();
        L().host = saved_host;
        L().mode = saved_mode;
        game::check_entries();
    }
};

std::vector<std::string>* g_order = nullptr;

}  // namespace

TEST_CASE("game-api: main menu") {
    // covers: game-api/Main menu
    ClientGame g;
    std::string why;
    CHECK(test_set_joined(KY_SIDE_CLIENT, &why) == KY_E_PHASE);
    CHECK_MESSAGE(contains(why, "no world is loaded"), why);
    CHECK(test_set_joined(KY_SIDE_SERVER, &why) == KY_E_PHASE);
    CHECK_FALSE(g.gc->world.joined);   // the game's memory is unchanged

    // a world is entered: still refused until the World has stepped once (the game thread is not known)
    game::on_world_enter();
    CHECK(test_set_joined(KY_SIDE_CLIENT, &why) == KY_E_PHASE);
    CHECK_FALSE(g.gc->world.joined);
    step(&g.gc->world);
    CHECK(game::game_thread() == GetCurrentThreadId());
    CHECK(test_set_joined(KY_SIDE_CLIENT, &why) == KY_OK);
    CHECK(g.gc->world.joined);

    // single-player: both sides use the controller's World; the report counts the map's two creatures
    CHECK(game::side_world(KY_SIDE_CLIENT) == &g.gc->world);
    CHECK(game::side_world(KY_SIDE_SERVER) == &g.gc->world);
    std::string all;
    for (auto& line : game::report()) all += line + "\n";
    CHECK_MESSAGE(contains(all, "2 creature(s) (map size 2)"), all);
    CHECK_MESSAGE(contains(all, "server side: the same World"), all);
    CHECK_MESSAGE(contains(all, "World lock: free"), all);

    // a joined client has no server side
    L().mode = KY_MODE_REMOTE_CLIENT;
    CHECK(test_set_joined(KY_SIDE_SERVER, &why) == KY_E_SIDE);
    CHECK(test_set_joined(KY_SIDE_CLIENT) == KY_OK);
    L().mode = KY_MODE_MENU_OR_SINGLEPLAYER;

    // back at the menu
    g.gc->world.joined = false;
    game::on_world_exit();
    CHECK(test_set_joined(KY_SIDE_CLIENT, &why) == KY_E_PHASE);
    CHECK_FALSE(g.gc->world.joined);
}

TEST_CASE("game-api: network thread") {
    // covers: game-api/Network thread
    ClientGame g;
    game::on_world_enter();
    step(&g.gc->world);
    KyubuResult r = KY_OK;
    std::string why;
    std::thread net([&] { r = test_set_joined(KY_SIDE_CLIENT, &why); });
    net.join();
    CHECK(r == KY_E_UNSAFE);
    CHECK_MESSAGE(contains(why, "not on the game thread"), why);
    CHECK_FALSE(g.gc->world.joined);   // refused without touching the World
    // a call holds the World's lock, so another thread can't take it meanwhile
    {
        game::WorldAccess w(game::Prim::world, KY_SIDE_CLIENT);
        REQUIRE(w);
        bool entered = true;
        std::thread other([&] { entered = TryEnterCriticalSection((CRITICAL_SECTION*)&g.gc->world.lock) != 0; });
        other.join();
        CHECK_FALSE(entered);
    }
}

TEST_CASE("game-api: change during a step") {
    // covers: game-api/Change during a step
    ClientGame g;
    game::on_world_enter();
    std::vector<std::string> seen;
    g_order = &seen;
    game::before_step([] { g_order->push_back(fmt("before, in a step: %d", game::in_step() ? 1 : 0)); });
    g_inside_step = [] {
        g_order->push_back(fmt("step, in a step: %d", game::in_step() ? 1 : 0));
        game::after_step([] { g_order->push_back(fmt("change, in a step: %d", game::in_step() ? 1 : 0)); });
    };
    step(&g.gc->world);
    CHECK(seen == std::vector<std::string>{"before, in a step: 0", "step, in a step: 1", "change, in a step: 0"});
    // each queued task runs once
    g_inside_step = nullptr;
    step(&g.gc->world);
    CHECK(seen.size() == 3);
    // a task still queued when the world is left is dropped with it
    game::after_step([] { g_order->push_back("stale"); });
    game::on_world_exit();
    game::on_world_enter();
    step(&g.gc->world);
    CHECK(seen.size() == 3);
    g_order = nullptr;
}

TEST_CASE("game-api: Server.exe's World is the one its main loop steps") {
    Loader& l = L();
    const HostInfo saved = l.host;
    l.host.host = KY_HOST_SERVER;
    l.host.build_id = "test-build";
    cube_World* w = alloc_zeroed<cube_World>();
    FakeWorld fake;
    fake.init(w);
    load_base_map("verified", false);   // Server.exe has no controller global
    game::boot();
    KyubuResult r = KY_OK;
    std::string why;
    CHECK(game::side_world(KY_SIDE_SERVER, &r, &why) == nullptr);
    CHECK(r == KY_E_PHASE);
    CHECK(test_set_joined(KY_SIDE_SERVER, &why) == KY_E_PHASE);

    const uint64_t seq = Log::get().sequence();
    step(w);
    CHECK(game::side_world(KY_SIDE_SERVER) == w);
    CHECK(game::side_world(KY_SIDE_CLIENT, &r, &why) == nullptr);
    CHECK(r == KY_E_SIDE);
    CHECK(test_set_joined(KY_SIDE_SERVER) == KY_OK);
    // the log shows the World it captured, and the report
    const std::string captured = fmt("the server side uses the World at 0x%08X", (unsigned)(uintptr_t)w);
    int shown = 0;
    for (auto& line : Log::get().since(seq))
        if (contains(line.text, captured.c_str()) ||
            (contains(line.text, "game: server side: World") && contains(line.text, "2 creature(s)")))
            ++shown;
    CHECK(shown == 2);

    game::remove_world_hooks();
    game::remove_handle_hooks();
    DeleteCriticalSection((CRITICAL_SECTION*)&w->lock);
    VirtualFree(w, 0, MEM_RELEASE);
    SymbolTable::get().clear();
    l.host = saved;
    game::check_entries();
}

TEST_CASE("game-events: tick on a dedicated server") {
    // covers: game-events/Tick on a dedicated server (the in-game check is a Server.exe session under Wine)
    std::vector<KyubuFrameData> ticks;
    int inside = 0;
    const KyubuId id = Events::get().subscribe_internal("sim.tick", KY_SIDE_SERVER, 0, [&](KyubuEvent* ev) {
        ticks.push_back(*(const KyubuFrameData*)ev->data);
        if (game::in_step()) ++inside;
    });

    // Server.exe: one sim.tick per World step, after it, with the step's milliseconds
    Loader& l = L();
    const HostInfo saved = l.host;
    const KyubuHostMode saved_mode = l.mode;
    l.host.host = KY_HOST_SERVER;
    l.host.build_id = "test-build";
    l.mode = KY_MODE_DEDICATED;
    cube_World* w = alloc_zeroed<cube_World>();
    FakeWorld fake;
    fake.init(w);
    load_base_map("verified", false);
    game::boot();
    for (int i = 0; i < 3; ++i) step(w);
    REQUIRE(ticks.size() == 3);
    CHECK(inside == 0);   // after the step returned
    for (size_t i = 0; i < ticks.size(); ++i) {
        CHECK(ticks[i].dt == 20.0);
        CHECK(ticks[i].frame == ticks[0].frame + i);   // the tick numbers increase by one each call
    }
    game::remove_world_hooks();
    game::remove_handle_hooks();
    DeleteCriticalSection((CRITICAL_SECTION*)&w->lock);
    VirtualFree(w, 0, MEM_RELEASE);
    l.host = saved;
    l.mode = saved_mode;

    // single-player Cube.exe: the World step emits nothing (GameController::update's hook does)
    {
        ClientGame g;
        step(&g.gc->world);
    }
    CHECK(ticks.size() == 3);
    Events::get().unsubscribe(nullptr, id);
}

namespace {

// The game destroying a creature, through the (detoured) cube::Creature::~Creature.
void destroy(cube_Creature* c) {
    auto volatile dtor = &game_test_creature_dtor;
    dtor(c, nullptr);
}

// A mod with a handle on each side.
struct TestMod {
    Mod mod;
    TestMod() {
        mod.id = "game-test";
        mod.state = ModState::Loaded;
        mod.manifest.sides = KY_SIDE_BOTH;
        for (int s = 1; s <= 2; ++s) { mod.ctx[s].mod = &mod; mod.ctx[s].side = (KyubuSide)s; }
    }
    KyubuModHandle on(KyubuSide side) { return mod.handle(side); }
};

// Game-API functions for the tests: each starts with GameCall, as the checklist says.
KyubuResult test_hand_out(KyubuModHandle self, cube_Creature* c, KyubuCreature* out) {
    game::GameCall g(self, game::Prim::handles, "test_hand_out");
    if (!g) return g.result();
    *out = g.handle(c);
    return KY_OK;
}

KyubuResult test_read_id(KyubuModHandle self, KyubuCreature h, uint64_t* id) {
    game::GameCall g(self, game::Prim::handles, "test_read_id");
    if (!g) return g.result();
    cube_Creature* c = g.creature(h);
    if (!c) return g.result();
    *id = c->id;
    return KY_OK;
}

// A change: only the creature's owner may make it.
KyubuResult test_set_hp(KyubuModHandle self, KyubuCreature h, float hp) {
    game::GameCall g(self, game::Prim::handles, "test_set_hp");
    if (!g) return g.result();
    cube_Creature* c = g.creature(h);
    if (!c) return g.result();
    if (!g.owns(c)) return g.result();
    c->data.hp = hp;
    return KY_OK;
}

// A function that only runs its guard, under a name of the test's choosing (a real function's row is looked up by name).
KyubuResult test_guard_only(KyubuModHandle self, const char* fn) {
    game::GameCall g(self, game::Prim::handles, fn);
    return g.result();
}

std::string report_text() {
    std::string all;
    for (auto& line : game::report()) all += line + "\n";
    return all;
}

int logged_lines(uint64_t since, const char* a, const char* b) {
    int n = 0;
    for (auto& line : Log::get().since(since))
        if (contains(line.text, a) && contains(line.text, b)) ++n;
    return n;
}

}  // namespace

TEST_CASE("game-api: destroyed creature") {
    // covers: game-api/Destroyed creature
    ClientGame g;
    TestMod m;
    game::on_world_enter();
    step(&g.gc->world);
    cube_Creature* c = g.fake.creatures[1];
    KyubuCreature h1 = 0, h2 = 0, other = 0;
    uint64_t id = 0;
    REQUIRE(test_hand_out(m.on(KY_SIDE_CLIENT), c, &h1) == KY_OK);
    CHECK(h1 != 0);
    // the same creature keeps its handle while it lives, and a handle works
    REQUIRE(test_hand_out(m.on(KY_SIDE_CLIENT), c, &h2) == KY_OK);
    CHECK(h1 == h2);
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), h1, &id) == KY_OK);
    CHECK(id == 2);
    REQUIRE(test_hand_out(m.on(KY_SIDE_CLIENT), g.fake.creatures[0], &other) == KY_OK);
    CHECK(other != h1);

    // the game destroys the creature: its handle is gone, the other one stays valid
    destroy(c);
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), h1, &id) == KY_E_GONE);
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), other, &id) == KY_OK);
    CHECK(game::handle_stats().live[KY_SIDE_CLIENT] == 1);

    // a creature made in its place (the same memory, as the game may rebuild it) gets a new handle: the old one
    // stays gone
    REQUIRE(test_hand_out(m.on(KY_SIDE_CLIENT), c, &h2) == KY_OK);
    CHECK(h2 != h1);
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), h1, &id) == KY_E_GONE);
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), h2, &id) == KY_OK);

    // `game hold` keeps a handle, and the report says whether it is live
    CHECK(contains(game::hold_creature(1), "holding creature 1"));
    CHECK(contains(game::hold_creature(77), "no creature with id 77"));
    CHECK(contains(game::hold_creature(0, true), "holding 2 creature(s)"));
    CHECK_MESSAGE(contains(report_text(), "handle 0x"), report_text());
    CHECK_MESSAGE(contains(report_text(), "is live"), report_text());
    destroy(g.fake.creatures[0]);
    const std::string after = report_text();
    CHECK_MESSAGE(contains(after, "not live (the creature no longer exists)"), after);

    // leaving the world invalidates every handle
    game::on_world_exit();
    CHECK(game::handle_stats().live[KY_SIDE_CLIENT] == 0);
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), other, &id) == KY_E_PHASE);
    game::on_world_enter();
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), other, &id) == KY_E_GONE);
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), h2, &id) == KY_E_GONE);
}

TEST_CASE("game-api: handle from the other side") {
    // covers: game-api/Handle from the other side
    ClientGame g;
    TestMod m;
    game::on_world_enter();
    step(&g.gc->world);
    KyubuCreature client_h = 0, server_h = 0;
    uint64_t id = 0;
    REQUIRE(test_hand_out(m.on(KY_SIDE_CLIENT), g.fake.creatures[1], &client_h) == KY_OK);
    REQUIRE(test_hand_out(m.on(KY_SIDE_SERVER), g.fake.creatures[1], &server_h) == KY_OK);
    CHECK(client_h != server_h);   // one handle per side
    const uint64_t seq = Log::get().sequence();
    CHECK(test_read_id(m.on(KY_SIDE_SERVER), client_h, &id) == KY_E_SIDE);
    CHECK(test_read_id(m.on(KY_SIDE_SERVER), client_h, &id) == KY_E_SIDE);
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), server_h, &id) == KY_E_SIDE);
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), client_h, &id) == KY_OK);
    CHECK(test_read_id(m.on(KY_SIDE_SERVER), server_h, &id) == KY_OK);
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), 0, &id) == KY_E_INVALID_ARG);
    // the refusal is logged once per mod, function and reason
    CHECK(logged_lines(seq, "test_read_id refused", "issued on the client side") == 1);
    // a destroyed creature's handles are gone on both sides
    destroy(g.fake.creatures[1]);
    CHECK(test_read_id(m.on(KY_SIDE_CLIENT), client_h, &id) == KY_E_GONE);
    CHECK(test_read_id(m.on(KY_SIDE_SERVER), server_h, &id) == KY_E_GONE);
}

TEST_CASE("game-api: changing the player from the server side") {
    // covers: game-api/Changing the player from the server side
    ClientGame g;
    TestMod m;
    game::on_world_enter();
    step(&g.gc->world);
    cube_Creature* player = g.fake.creatures[0];   // the local player
    cube_Creature* monster = g.fake.creatures[1];
    KyubuCreature ph_client = 0, ph_server = 0, mh_client = 0, mh_server = 0;
    REQUIRE(test_hand_out(m.on(KY_SIDE_CLIENT), player, &ph_client) == KY_OK);
    REQUIRE(test_hand_out(m.on(KY_SIDE_SERVER), player, &ph_server) == KY_OK);
    REQUIRE(test_hand_out(m.on(KY_SIDE_CLIENT), monster, &mh_client) == KY_OK);
    REQUIRE(test_hand_out(m.on(KY_SIDE_SERVER), monster, &mh_server) == KY_OK);

    // single-player: the client side owns the player, the server side every other creature
    CHECK(game::owner_of(&g.gc->world, player) == KY_SIDE_CLIENT);
    CHECK(game::owner_of(&g.gc->world, monster) == KY_SIDE_SERVER);
    const uint64_t seq = Log::get().sequence();
    CHECK(test_set_hp(m.on(KY_SIDE_SERVER), ph_server, 5.0f) == KY_E_SIDE);
    CHECK(player->data.hp == 0.0f);   // the game's memory is unchanged
    CHECK(logged_lines(seq, "test_set_hp refused", "the client side owns this creature") == 1);
    CHECK(test_set_hp(m.on(KY_SIDE_CLIENT), ph_client, 5.0f) == KY_OK);
    CHECK(player->data.hp == 5.0f);
    CHECK(test_set_hp(m.on(KY_SIDE_CLIENT), mh_client, 7.0f) == KY_E_SIDE);
    CHECK(monster->data.hp == 0.0f);
    CHECK(test_set_hp(m.on(KY_SIDE_SERVER), mh_server, 7.0f) == KY_OK);
    CHECK(monster->data.hp == 7.0f);

    // a faulted mod is refused, and so is a call from another thread or with a bad mod handle
    m.mod.state = ModState::Faulted;
    CHECK(test_set_hp(m.on(KY_SIDE_CLIENT), ph_client, 9.0f) == KY_E_FAULTED);
    m.mod.state = ModState::Loaded;
    KyubuResult r = KY_OK;
    std::thread net([&] { r = test_set_hp(m.on(KY_SIDE_CLIENT), ph_client, 9.0f); });
    net.join();
    CHECK(r == KY_E_UNSAFE);
    CHECK(player->data.hp == 5.0f);
    CHECK(test_set_hp(nullptr, ph_client, 9.0f) == KY_E_INVALID_ARG);
}

TEST_CASE("game-api: Server.exe owns every creature") {
    Loader& l = L();
    const HostInfo saved = l.host;
    l.host.host = KY_HOST_SERVER;
    l.host.build_id = "test-build";
    cube_World* w = alloc_zeroed<cube_World>();
    FakeWorld fake;
    fake.init(w);
    load_base_map("verified", false);
    game::boot();
    TestMod m;
    step(w);
    KyubuCreature h = 0;
    REQUIRE(test_hand_out(m.on(KY_SIDE_SERVER), fake.creatures[0], &h) == KY_OK);
    CHECK(test_set_hp(m.on(KY_SIDE_SERVER), h, 3.0f) == KY_OK);   // no player is told apart from a monster yet
    CHECK(fake.creatures[0]->data.hp == 3.0f);
    CHECK(test_hand_out(m.on(KY_SIDE_CLIENT), fake.creatures[0], &h) == KY_E_SIDE);   // Server.exe has no client side

    game::remove_world_hooks();
    game::remove_handle_hooks();
    DeleteCriticalSection((CRITICAL_SECTION*)&w->lock);
    VirtualFree(w, 0, MEM_RELEASE);
    SymbolTable::get().clear();
    l.host = saved;
    game::check_entries();
}

TEST_CASE("game-api: a one-side function refuses the other side") {
    // covers: game-api/Adding a game-API function
    ClientGame g;
    TestMod m;
    game::on_world_enter();
    step(&g.gc->world);
    // single-player: "ui_begin" is client-only in kSideRules, "game_available" is for both sides
    const uint64_t seq = Log::get().sequence();
    CHECK(test_guard_only(m.on(KY_SIDE_SERVER), "ui_begin") == KY_E_SIDE);
    CHECK(test_guard_only(m.on(KY_SIDE_SERVER), "ui_begin") == KY_E_SIDE);
    CHECK(logged_lines(seq, "ui_begin refused", "client-only and was called from the server side") == 1);
    CHECK(test_guard_only(m.on(KY_SIDE_CLIENT), "ui_begin") == KY_OK);
    CHECK(test_guard_only(m.on(KY_SIDE_CLIENT), "game_available") == KY_OK);
    CHECK(test_guard_only(m.on(KY_SIDE_SERVER), "game_available") == KY_OK);
    CHECK(logged_lines(seq, "refused", "-only") == 1);
    // a name with no row passes this check (the other tests rely on it)
    CHECK(test_guard_only(m.on(KY_SIDE_SERVER), "test_no_row") == KY_OK);
}

namespace {

KyubuCreatureInfo read_info(KyubuModHandle self, KyubuCreature h, KyubuResult* r) {
    KyubuCreatureInfo info = {};
    info.size = sizeof info;
    *r = api_table()->creature_get(self, h, &info);
    return info;
}

// The player's state for the tests: far from the origin (z above 2^32 units), facing west, and an id below zero, like
// a creature made at run time.
void set_player_state(cube_Creature* p) {
    p->id = (uint64_t)-5;
    p->data.pos.x = 65536LL * 100 + 5;
    p->data.pos.y = -3 * 65536LL;
    p->data.pos.z = (1LL << 39) + 7;
    p->data.body_yaw = 90.0f;
    p->data.hp = 123.5f;
    p->data.level = 7;
    p->data.entity_type = 2;
    p->data.hostile_type = 0;
}

}  // namespace

TEST_CASE("game-api: the player and its state") {
    // covers: game-api/No world yet, and the reading behind game-api/The player's position (its in-game check is a
    // replay)
    ClientGame g;
    TestMod m;
    game::on_world_exit();   // the start menu, whatever an earlier test left
    const KyubuApi* api = api_table();
    const KyubuModHandle client = m.on(KY_SIDE_CLIENT), server = m.on(KY_SIDE_SERVER);
    KyubuCreature h = 77;
    const uint64_t seq = Log::get().sequence();
    CHECK(api->creature_player(client, &h) == KY_E_PHASE);
    CHECK(h == 0);
    CHECK(game::last_refusal() == "no world is loaded");
    CHECK(logged_lines(seq, "creature_player refused", "no world is loaded") == 1);
    game::on_world_enter();
    CHECK(api->creature_player(client, &h) == KY_E_PHASE);   // the World has not stepped yet
    step(&g.gc->world);

    // the player: the same handle each time, on the client side only
    REQUIRE(api->creature_player(client, &h) == KY_OK);
    CHECK(h != 0);
    KyubuCreature again = 0;
    REQUIRE(api->creature_player(client, &again) == KY_OK);
    CHECK(again == h);
    CHECK(api->creature_player(server, &again) == KY_E_SIDE);
    CHECK(again == 0);
    CHECK(game::last_refusal() == "creature_player is client-only and was called from the server side");
    CHECK(api->creature_player(client, nullptr) == KY_E_INVALID_ARG);

    // its state, from the verified fields
    cube_Creature* p = g.fake.creatures[0];
    set_player_state(p);
    KyubuResult r = KY_OK;
    KyubuCreatureInfo info = read_info(client, h, &r);
    REQUIRE(r == KY_OK);
    CHECK(info.size == sizeof info);
    CHECK(info.entity_type == 2);
    CHECK(info.id == -5);
    CHECK(info.pos[0] == 65536LL * 100 + 5);
    CHECK(info.pos[1] == -3 * 65536LL);
    CHECK(info.pos[2] == (1LL << 39) + 7);
    CHECK(info.body_yaw == 90.0f);
    CHECK(info.hp == 123.5f);
    CHECK(info.level == 7);
    CHECK(info.hostile_type == 0);
    CHECK((info.reserved[0] | info.reserved[1] | info.reserved[2]) == 0);
    // the player walks: the next read follows
    p->data.pos.x += 65536;
    CHECK(read_info(client, h, &r).pos[0] == 65536LL * 101 + 5);

    // an older, smaller struct gets only what fits; a size that holds nothing is refused
    KyubuCreatureInfo small;
    memset(&small, 0xAB, sizeof small);
    small.size = offsetof(KyubuCreatureInfo, pos);
    REQUIRE(api->creature_get(client, h, &small) == KY_OK);
    CHECK(small.size == offsetof(KyubuCreatureInfo, pos));
    CHECK(small.entity_type == 2);
    CHECK(small.id == -5);
    CHECK(small.pos[0] == (int64_t)0xABABABABABABABABull);   // untouched
    small.size = 4;
    CHECK(api->creature_get(client, h, &small) == KY_E_INVALID_ARG);
    CHECK_MESSAGE(game::last_refusal() == "out->size is 4, below 8", game::last_refusal());
    CHECK(api->creature_get(client, h, nullptr) == KY_E_INVALID_ARG);
    CHECK(api->creature_get(client, 0, &info) == KY_E_INVALID_ARG);

    // reading works on both sides, each with its own handle
    CHECK(api->creature_get(server, h, &info) == KY_E_SIDE);
    KyubuCreature sh = 0;
    REQUIRE(test_hand_out(server, p, &sh) == KY_OK);
    CHECK(read_info(server, sh, &r).id == -5);
    CHECK(r == KY_OK);

    // the game destroys the player: both handles report it gone
    destroy(p);
    read_info(client, h, &r);
    CHECK(r == KY_E_GONE);
    read_info(server, sh, &r);
    CHECK(r == KY_E_GONE);

    // a World without its local player
    g.gc->world.local_player = nullptr;
    CHECK(api->creature_player(client, &h) == KY_E_PHASE);
    g.gc->world.local_player = p;
    game::on_world_exit();
}

TEST_CASE("game-api: creature_find gives either side a handle by game id") {
    // covers: add-entity-spawner 8.4: the server side reaches the player (the client side's local player) by its id
    ClientGame g;
    TestMod m;
    game::on_world_exit();
    const KyubuApi* api = api_table();
    const KyubuModHandle client = m.on(KY_SIDE_CLIENT), server = m.on(KY_SIDE_SERVER);
    KyubuCreature h = 77;
    CHECK(api->creature_find(server, 1, &h) == KY_E_PHASE);   // no world yet
    CHECK(h == 0);
    game::on_world_enter();
    step(&g.gc->world);

    // creature 1 is the player: the server side gets its own handle for it, the client side the one creature_player gave
    KyubuCreature player = 0, found = 0;
    REQUIRE(api->creature_player(client, &player) == KY_OK);
    REQUIRE(api->creature_find(server, 1, &found) == KY_OK);
    CHECK(found != 0);
    CHECK(found != player);   // handles are per side
    KyubuResult r = KY_OK;
    CHECK(read_info(server, found, &r).id == 1);
    CHECK(r == KY_OK);
    KyubuCreature again = 0;
    REQUIRE(api->creature_find(server, 1, &again) == KY_OK);
    CHECK(again == found);   // the same creature keeps its handle
    KyubuCreature on_client = 0;
    REQUIRE(api->creature_find(client, 1, &on_client) == KY_OK);
    CHECK(on_client == player);
    REQUIRE(api->creature_find(server, 2, &found) == KY_OK);
    CHECK(read_info(server, found, &r).id == 2);

    // an id the World does not have, and a null out
    CHECK(api->creature_find(server, 99, &found) == KY_E_NOT_FOUND);
    CHECK(found == 0);
    CHECK_MESSAGE(game::last_refusal() == "no creature with id 99 in this side's World", game::last_refusal());
    CHECK(api->creature_find(server, 1, nullptr) == KY_E_INVALID_ARG);

    // the game destroys the creature: the handle is gone, and the id is not found any more
    REQUIRE(api->creature_find(server, 2, &found) == KY_OK);
    cube_Creature* second = g.fake.creatures[1];
    destroy(second);
    read_info(server, found, &r);
    CHECK(r == KY_E_GONE);
    game::on_world_exit();
}

TEST_CASE("game-api: creature functions need handles") {
    ClientGame g;
    TestMod m;
    game::on_world_enter();
    step(&g.gc->world);
    load_base_map("verified", true, "candidate");   // the destructor detour is not verified: handles are unavailable
    game::check_entries();
    REQUIRE(game::available(game::Prim::entity_state));
    REQUIRE_FALSE(game::available(game::Prim::handles));
    KyubuCreature h = 1;
    CHECK(api_table()->creature_player(m.on(KY_SIDE_CLIENT), &h) == KY_E_UNAVAILABLE);
    CHECK(h == 0);
    CHECK_MESSAGE(contains(game::last_refusal(), "handles is unavailable: cube::Creature::~Creature"), game::last_refusal());
    game::on_world_exit();
}

namespace {

// A spawn request for the tests: a level-5 hostile green slime (type 37), facing south.
KyubuSpawn slime() {
    KyubuSpawn s = {};
    s.size = sizeof s;
    s.entity_type = 37;
    s.hostile_type = 1;
    s.level = 5;
    s.pos[0] = 65536LL * 100 + 5;
    s.pos[1] = 65536LL * 102;
    s.pos[2] = (1LL << 39) + 7;
    s.body_yaw = 180.0f;
    return s;
}

// Records the spawn recipe's calls for one test, and frees what the stand-in allocator gave out. Declare it after
// ClientGame, so it goes first (the handle tables go with ClientGame and never read a creature).
struct RecipeLog {
    std::vector<std::string> calls;
    RecipeLog() {
        g_recipe = &calls;
        g_slot_count = 0;
        g_slot_map = nullptr;
        memset(g_slots, 0, sizeof g_slots);
    }
    ~RecipeLog() {
        g_recipe = nullptr;
        for (void* p : g_allocated) VirtualFree(p, 0, MEM_RELEASE);
        g_allocated.clear();
        memset(g_slots, 0, sizeof g_slots);
        g_slot_count = 0;
    }
};

std::vector<std::string> recipe_calls(int64_t id, uint32_t type = 37, int level = 5) {
    return {"new 0x1E60", fmt("construct %lld", (long long)id), fmt("max hp: type %u, level %d", type, level),
            fmt("look %u", type)};
}

}  // namespace

TEST_CASE("game-api: spawning a creature") {
    // covers: game-api/Client side refused, game-api/Unknown entity type, the second half of game-api/Entity types
    // (creature_spawn accepts each value game_enum lists), and the recipe behind game-api/Spawn in single-player (its
    // in-game check is probe creature-spawn)
    ClientGame g;
    TestMod m;
    RecipeLog rec;
    const KyubuApi* api = api_table();
    const KyubuModHandle client = m.on(KY_SIDE_CLIENT), server = m.on(KY_SIDE_SERVER);
    game::on_world_enter();
    step(&g.gc->world);
    REQUIRE(game::available(game::Prim::spawn));
    const KyubuSpawn s = slime();
    KyubuCreature h = 77;

    // the client side is refused, and nothing is built
    const uint64_t seq = Log::get().sequence();
    CHECK(api->creature_spawn(client, &s, &h) == KY_E_SIDE);
    CHECK(h == 0);
    CHECK(game::last_refusal() == "creature_spawn is server-only and was called from the client side");
    CHECK(logged_lines(seq, "creature_spawn refused", "server-only") == 1);

    // a value out of range is refused naming its field, before anything is built
    struct Bad {
        void (*change)(KyubuSpawn&);
        const char* why;
    };
    const Bad bad[] = {
        {[](KyubuSpawn& b) { b.entity_type = 9999; }, "entity_type is 9999, not a value of cube::EntityType (game_enum lists them)"},
        {[](KyubuSpawn& b) { b.hostile_type = 0; }, "hostile_type is 0 (a player); a spawn takes another cube::HostileType value"},
        {[](KyubuSpawn& b) { b.hostile_type = 4; }, "hostile_type is 4, not a value of cube::HostileType"},
        {[](KyubuSpawn& b) { b.level = 0; }, "level is 0, must be 1 or more"},
        {[](KyubuSpawn& b) { b.behavior = 6; }, "behavior is 6; this loader knows KY_SPAWN_STILL (0) to KY_SPAWN_DEFAULT (5)"},
        {[](KyubuSpawn& b) { b.reserved = 1; }, "reserved is 1, must be 0"},
        {[](KyubuSpawn& b) { b.size = 44; }, "size is 44, below 64 (sizeof(KyubuSpawn))"},
        {[](KyubuSpawn& b) { b.body_yaw = NAN; }, "body_yaw is not a finite number"},
    };
    for (const Bad& b : bad) {
        KyubuSpawn x = s;
        b.change(x);
        h = 77;
        CHECK(api->creature_spawn(server, &x, &h) == KY_E_INVALID_ARG);
        CHECK(h == 0);
        CHECK_MESSAGE(game::last_refusal() == b.why, game::last_refusal());
    }
    CHECK(api->creature_spawn(server, nullptr, &h) == KY_E_INVALID_ARG);
    CHECK(api->creature_spawn(server, &s, nullptr) == KY_E_INVALID_ARG);
    CHECK(logged_lines(seq, "creature_spawn refused", "entity_type is 9999") == 1);
    CHECK(rec.calls.empty());
    CHECK(g_allocated.empty());

    // a green slime: the game's recipe in its order, inserted at once (not in a step) into the World's map
    REQUIRE(api->creature_spawn(server, &s, &h) == KY_OK);
    CHECK(h != 0);
    std::vector<std::string> expect = recipe_calls(-1);   // the lowest id in the map is 1: not above 0, minus 1
    expect.push_back("insert -1, in a step: 0");
    CHECK(rec.calls == expect);
    CHECK(g_slot_map == &g.gc->world.creatures);
    cube_Creature* c = g_slots[0];
    REQUIRE(c == g_allocated.back());
    CHECK(c->id == (uint64_t)-1);
    CHECK(*(const uint8_t*)&c->data.appearance == 0xA5);
    // creature_get reads it through the handle, which is the server side's
    KyubuResult r = KY_OK;
    KyubuCreatureInfo info = read_info(server, h, &r);
    REQUIRE(r == KY_OK);
    CHECK(info.id == -1);
    CHECK(info.entity_type == 37);
    CHECK(info.hostile_type == 1);
    CHECK(info.level == 5);
    CHECK(info.hp == 105.0f);   // the max hp the game function returned
    CHECK(info.pos[0] == 65536LL * 100 + 5);
    CHECK(info.pos[1] == 65536LL * 102);
    CHECK(info.pos[2] == (1LL << 39) + 7);
    CHECK(info.body_yaw == 180.0f);
    CHECK(api->creature_get(client, h, &info) == KY_E_SIDE);
    // with run-time creatures in the map (the lowest id at the head's left), the next id is one less
    g.fake.nodes[2].key = (uint64_t)-4;
    REQUIRE(api->creature_spawn(server, &s, &h) == KY_OK);
    CHECK(g_slots[1]->id == (uint64_t)-5);
    CHECK(rec.calls[6] == "construct -5");

    // every entity type game_enum lists is accepted
    int types = 0;
    for (uint32_t i = 0;; ++i) {
        int64_t value = 0;
        char name[64] = {};
        if (api->game_enum("cube::EntityType", i, &value, name, sizeof name) != KY_OK) break;
        KyubuSpawn x = s;
        x.entity_type = (uint32_t)value;
        CHECK_MESSAGE(api->creature_spawn(server, &x, &h) == KY_OK, name);
        ++types;
    }
    CHECK(types == (int)compiled_enum("cube::EntityType")->value_count);
    CHECK(g_slot_count == types + 2);
    game::on_world_exit();
}

TEST_CASE("game-api: spawning a monster") {
    // covers: game-api/Behaviour not mapped (KY_SPAWN_MONSTER reports `behavior` unavailable, and builds nothing, while
    // its entries are candidate), and the recipe behind entity-spawner/A hostile slime attacks (step 8: the
    // constructors and the appends in the game's order, the sequence stored in the creature; the in-game check is
    // probe spawn-monster)
    ClientGame g;
    TestMod m;
    RecipeLog rec;
    const KyubuApi* api = api_table();
    const KyubuModHandle server = m.on(KY_SIDE_SERVER);
    game::on_world_enter();
    step(&g.gc->world);
    REQUIRE(game::available(game::Prim::behavior));
    KyubuSpawn s = slime();
    KyubuCreature h = 0;

    // still: no behaviour, and the primitive is not asked for
    REQUIRE(api->creature_spawn(server, &s, &h) == KY_OK);
    CHECK(g_slots[0]->behavior == nullptr);
    CHECK(rec.calls == [] { auto v = recipe_calls(-1); v.push_back("insert -1, in a step: 0"); return v; }());

    // monster: the game's order (0x60EBAD): the sequence, then each child constructed and appended to the sequence's
    // list (at +4), the sequence stored in the creature, and the insert last. Blocks: 0 the first creature, 1 the
    // second, 2 the sequence, 3 the combat behaviour, 4 look at player, 5 random walk.
    rec.calls.clear();
    s.behavior = KY_SPAWN_MONSTER;
    REQUIRE(api->creature_spawn(server, &s, &h) == KY_OK);
    std::vector<std::string> expect = recipe_calls(-1);   // the stand-in map does not grow: the same id again
    for (const char* line : {"new 0xC", "sequence 2", "new 0x14", "combat 3, value 20.0", "append 3 to the list of 2", "new 0x4",
                             "look at player 4", "append 4 to the list of 2", "new 0x8", "random walk 5",
                             "append 5 to the list of 2", "insert -1, in a step: 0"})
        expect.push_back(line);
    CHECK(rec.calls == expect);
    REQUIRE(g_slots[1] != nullptr);
    CHECK(g_slots[1]->behavior == (cube_Behavior*)g_allocated[2]);
    CHECK(g_slots[1]->id == (uint64_t)-1);

    // an entry of the behaviour recipe is candidate: KY_SPAWN_MONSTER is refused before anything is built, and a still
    // spawn works as before
    load_base_map("verified", true, "verified", "verified", "candidate");
    game::check_entries();
    REQUIRE_FALSE(game::available(game::Prim::behavior));
    REQUIRE(game::available(game::Prim::spawn));
    rec.calls.clear();
    const size_t blocks = g_allocated.size();
    h = 77;
    CHECK(api->creature_spawn(server, &s, &h) == KY_E_UNAVAILABLE);
    CHECK(h == 0);
    CHECK_MESSAGE(contains(game::last_refusal(), "behavior is unavailable: "), game::last_refusal());
    CHECK(rec.calls.empty());
    CHECK(g_allocated.size() == blocks);
    s.behavior = KY_SPAWN_STILL;
    CHECK(api->creature_spawn(server, &s, &h) == KY_OK);
    game::on_world_exit();
}

TEST_CASE("game-api: spawning every kind of behaviour") {
    // covers: entity-spawner/Creatures that act and game-api/Behaviour not mapped (per kind): each kind attaches
    // through the mapped constructors in the game's order (a fake World), a companion without an owner fails with
    // "owner required", a guard without a path with "path required", an unknown kind fails, and a kind whose
    // constructors are candidate reports its primitive unavailable and builds nothing
    ClientGame g;
    TestMod m;
    RecipeLog rec;
    const KyubuApi* api = api_table();
    const KyubuModHandle server = m.on(KY_SIDE_SERVER);
    game::on_world_enter();
    step(&g.gc->world);
    REQUIRE(game::available(game::Prim::behavior));
    REQUIRE(game::available(game::Prim::behavior_companion));
    REQUIRE(game::available(game::Prim::behavior_path));
    KyubuSpawn s = slime();
    KyubuCreature owner = 0, h = 0;
    REQUIRE(api->creature_spawn(server, &s, &owner) == KY_OK);   // block 0, id -1: the companion's owner
    g.fake.nodes[2].key = (uint64_t)-4;                          // the next creature gets id -5
    rec.calls.clear();

    // Expected calls of a spawn whose creature is block `b`: the recipe (id -5), the sequence (b + 1), the children.
    auto expect = [&](std::initializer_list<std::string> steps) {
        std::vector<std::string> v = recipe_calls(-5);
        for (auto& st : steps) v.push_back(st);
        v.push_back("insert -5, in a step: 0");
        return v;
    };
    auto spawn_kind = [&](uint8_t kind, uint8_t hostile) {
        s = slime();
        s.behavior = kind;
        s.hostile_type = hostile;
        s.owner = owner;
        rec.calls.clear();
        h = 0;
        return api->creature_spawn(server, &s, &h);
    };

    // friendly: combat, look at player, random walk (it wanders and fights what is hostile to it when that comes close)
    size_t b = g_allocated.size();
    REQUIRE(spawn_kind(KY_SPAWN_FRIENDLY, 2) == KY_OK);
    CHECK(rec.calls == expect({"new 0xC", fmt("sequence %zu", b + 1), "new 0x14", fmt("combat %zu, value 20.0", b + 2),
                               fmt("append %zu to the list of %zu", b + 2, b + 1), "new 0x4", fmt("look at player %zu", b + 3),
                               fmt("append %zu to the list of %zu", b + 3, b + 1), "new 0x8", fmt("random walk %zu", b + 4),
                               fmt("append %zu to the list of %zu", b + 4, b + 1)}));
    CHECK(g_slots[g_slot_count - 1]->behavior == (cube_Behavior*)g_allocated[b + 1]);
    CHECK(g_slots[g_slot_count - 1]->data.parent_owner == 0);   // only a companion has an owner

    // companion: combat, then the companion behaviour with the owner's game id (-1) at +8 and +4 zeroed; the creature
    // keeps the owner's id too (parent_owner, +0x198), as the game's pet and summon paths store it
    b = g_allocated.size();
    REQUIRE(spawn_kind(KY_SPAWN_COMPANION, 3) == KY_OK);
    CHECK(rec.calls == expect({"new 0xC", fmt("sequence %zu", b + 1), "new 0x14", fmt("combat %zu, value 20.0", b + 2),
                               fmt("append %zu to the list of %zu", b + 2, b + 1), "new 0x10",
                               fmt("companion %zu, +4 00000000", b + 3), fmt("append %zu to the list of %zu", b + 3, b + 1)}));
    CHECK(*(const int64_t*)((const uint8_t*)g_allocated[b + 3] + 8) == -1);
    CHECK(g_slots[g_slot_count - 1]->behavior == (cube_Behavior*)g_allocated[b + 1]);
    CHECK((int64_t)g_slots[g_slot_count - 1]->data.parent_owner == -1);

    // a pet (hostile type 5, the game's own pet type) is a hostile type a spawn takes, with the same behaviour
    REQUIRE(spawn_kind(KY_SPAWN_COMPANION, 5) == KY_OK);
    CHECK(g_slots[g_slot_count - 1]->data.hostile_type == 5);
    CHECK((int64_t)g_slots[g_slot_count - 1]->data.parent_owner == -1);

    // guard: combat, then the walk path, whose waypoints are appended to the vector at +4 in order; +0x14 zeroed
    static const int64_t path[2][3] = {{65536, 131072, 7}, {-5, 6, 1LL << 40}};
    b = g_allocated.size();
    s = slime();
    s.behavior = KY_SPAWN_GUARD;
    s.path = path;
    s.path_count = 2;
    rec.calls.clear();
    REQUIRE(api->creature_spawn(server, &s, &h) == KY_OK);
    CHECK(rec.calls == expect({"new 0xC", fmt("sequence %zu", b + 1), "new 0x14", fmt("combat %zu, value 20.0", b + 2),
                               fmt("append %zu to the list of %zu", b + 2, b + 1), "new 0x1C",
                               fmt("walk path %zu, value 2.0, +0x14 00000000", b + 3),
                               fmt("waypoint to %zu: 65536, 131072, 7", b + 3),
                               fmt("waypoint to %zu: -5, 6, %lld", b + 3, 1LL << 40),
                               fmt("append %zu to the list of %zu", b + 3, b + 1)}));

    // default: the zone default (the monster's three children) for a hostile creature, none for a target
    b = g_allocated.size();
    REQUIRE(spawn_kind(KY_SPAWN_DEFAULT, 1) == KY_OK);
    CHECK(rec.calls == expect({"new 0xC", fmt("sequence %zu", b + 1), "new 0x14", fmt("combat %zu, value 20.0", b + 2),
                               fmt("append %zu to the list of %zu", b + 2, b + 1), "new 0x4", fmt("look at player %zu", b + 3),
                               fmt("append %zu to the list of %zu", b + 3, b + 1), "new 0x8", fmt("random walk %zu", b + 4),
                               fmt("append %zu to the list of %zu", b + 4, b + 1)}));
    REQUIRE(spawn_kind(KY_SPAWN_DEFAULT, 6) == KY_OK);
    CHECK(rec.calls == expect({}));
    CHECK(g_slots[g_slot_count - 1]->behavior == nullptr);

    // a request that misses what its kind needs is refused, naming the field, and builds nothing
    const size_t blocks_after = g_allocated.size();
    struct Bad {
        void (*change)(KyubuSpawn&);
        const char* why;
    };
    const Bad bad[] = {
        {[](KyubuSpawn& x) { x.behavior = KY_SPAWN_COMPANION; x.owner = 0; },
         "owner required: KY_SPAWN_COMPANION needs the handle of the creature it follows in owner"},
        {[](KyubuSpawn& x) { x.behavior = KY_SPAWN_GUARD; x.path = nullptr; x.path_count = 1; },
         "path required: KY_SPAWN_GUARD needs 1 to 64 waypoints in path (path_count is 1)"},
        {[](KyubuSpawn& x) { x.behavior = KY_SPAWN_GUARD; x.path = path; x.path_count = 0; },
         "path required: KY_SPAWN_GUARD needs 1 to 64 waypoints in path (path_count is 0)"},
        {[](KyubuSpawn& x) { x.behavior = KY_SPAWN_GUARD; x.path = path; x.path_count = 65; }, "path_count is 65, above 64"},
        {[](KyubuSpawn& x) { x.behavior = 6; }, "behavior is 6; this loader knows KY_SPAWN_STILL (0) to KY_SPAWN_DEFAULT (5)"},
    };
    for (const Bad& x : bad) {
        s = slime();
        x.change(s);
        rec.calls.clear();
        h = 77;
        CHECK(api->creature_spawn(server, &s, &h) == KY_E_INVALID_ARG);
        CHECK(h == 0);
        CHECK_MESSAGE(game::last_refusal() == x.why, game::last_refusal());
        CHECK(rec.calls.empty());
    }
    // an owner that is no creature of this side's world: refused before anything is built
    s = slime();
    s.behavior = KY_SPAWN_COMPANION;
    s.owner = 0x7777777;
    rec.calls.clear();
    h = 77;
    CHECK(api->creature_spawn(server, &s, &h) != KY_OK);
    CHECK(h == 0);
    CHECK(rec.calls.empty());
    CHECK(g_allocated.size() == blocks_after);
    // a kind ignores the fields it does not use: a friendly creature with an owner and a path is built as usual
    s = slime();
    s.behavior = KY_SPAWN_FRIENDLY;
    s.owner = 0x7777777;
    s.path = path;
    s.path_count = 2;
    CHECK(api->creature_spawn(server, &s, &h) == KY_OK);

    // the companion and walk-path constructors are candidate: those two kinds report their primitive unavailable and
    // build nothing, while the others (and a target's default) are unaffected
    load_base_map("verified", true, "verified", "verified", "verified", "candidate");
    game::check_entries();
    REQUIRE(game::available(game::Prim::behavior));
    REQUIRE_FALSE(game::available(game::Prim::behavior_companion));
    REQUIRE_FALSE(game::available(game::Prim::behavior_path));
    const size_t before = g_allocated.size();
    CHECK(spawn_kind(KY_SPAWN_COMPANION, 3) == KY_E_UNAVAILABLE);
    CHECK(h == 0);
    CHECK_MESSAGE(contains(game::last_refusal(), "behavior_companion is unavailable: "), game::last_refusal());
    s = slime();
    s.behavior = KY_SPAWN_GUARD;
    s.path = path;
    s.path_count = 2;
    CHECK(api->creature_spawn(server, &s, &h) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(contains(game::last_refusal(), "behavior_path is unavailable: "), game::last_refusal());
    CHECK(g_allocated.size() == before);
    CHECK(spawn_kind(KY_SPAWN_FRIENDLY, 2) == KY_OK);
    CHECK(spawn_kind(KY_SPAWN_DEFAULT, 6) == KY_OK);

    // the base constructors are candidate: every behaviour but a target's default (none) is unavailable
    load_base_map("verified", true, "verified", "verified", "candidate");
    game::check_entries();
    REQUIRE_FALSE(game::available(game::Prim::behavior));
    const size_t before2 = g_allocated.size();
    CHECK(spawn_kind(KY_SPAWN_DEFAULT, 1) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(contains(game::last_refusal(), "behavior is unavailable: "), game::last_refusal());
    CHECK(g_allocated.size() == before2);
    CHECK(spawn_kind(KY_SPAWN_DEFAULT, 6) == KY_OK);
    game::on_world_exit();
}

namespace {
KyubuModHandle g_spawner = nullptr;
KyubuCreature g_spawned[2];
KyubuResult g_spawn_result[2];
int64_t g_read_in_step = 0;
int g_inserts_in_step = -1;
}  // namespace

TEST_CASE("game-api: spawn during a World step") {
    // covers: game-api/Spawn during a World step
    ClientGame g;
    TestMod m;
    RecipeLog rec;
    const KyubuApi* api = api_table();
    g_spawner = m.on(KY_SIDE_SERVER);
    game::on_world_enter();
    step(&g.gc->world);

    // two spawns inside the step: each returns a handle at once that creature_get reads; the inserts wait
    g_inside_step = [] {
        const KyubuSpawn s = slime();
        for (int i = 0; i < 2; ++i) g_spawn_result[i] = api_table()->creature_spawn(g_spawner, &s, &g_spawned[i]);
        KyubuResult r = KY_OK;
        g_read_in_step = read_info(g_spawner, g_spawned[1], &r).id;
        g_inserts_in_step = g_slot_count;
    };
    step(&g.gc->world);
    g_inside_step = nullptr;
    CHECK(g_spawn_result[0] == KY_OK);
    CHECK(g_spawn_result[1] == KY_OK);
    CHECK(g_spawned[0] != 0);
    CHECK(g_spawned[1] != 0);
    CHECK(g_spawned[0] != g_spawned[1]);
    CHECK(g_read_in_step == -2);   // the first one waits for its insert, and still counts for the id rule
    CHECK(g_inserts_in_step == 0);
    // after the step returned: both in the map, in call order
    std::vector<std::string> expect = recipe_calls(-1);
    for (auto& call : recipe_calls(-2)) expect.push_back(call);
    expect.push_back("insert -1, in a step: 0");
    expect.push_back("insert -2, in a step: 0");
    CHECK(rec.calls == expect);
    REQUIRE(g_slot_count == 2);
    CHECK(g_slots[0]->id == (uint64_t)-1);
    CHECK(g_slots[1]->id == (uint64_t)-2);
    // the next step inserts nothing again; outside a step the insert is at once, and the ids waiting are gone (the
    // stand-in map does not grow, so -1 again)
    step(&g.gc->world);
    CHECK(g_slot_count == 2);
    const KyubuSpawn s = slime();
    KyubuCreature h = 0;
    REQUIRE(api->creature_spawn(g_spawner, &s, &h) == KY_OK);
    CHECK(rec.calls.back() == "insert -1, in a step: 0");

    // leaving the world with an insert still queued drops it: its id leaves the waiting list, and the log says so
    const uint64_t seq = Log::get().sequence();
    g_inside_step = [] {
        const KyubuSpawn s = slime();
        g_spawn_result[0] = api_table()->creature_spawn(g_spawner, &s, &g_spawned[0]);
        game::on_world_exit();
    };
    step(&g.gc->world);
    g_inside_step = nullptr;
    CHECK(g_spawn_result[0] == KY_OK);
    CHECK(g_slot_count == 3);
    CHECK(logged_lines(seq, "creature_spawn: the creature with id -1", "dropped with the world") == 1);
    game::on_world_enter();
    REQUIRE(api->creature_spawn(g_spawner, &s, &h) == KY_OK);
    CHECK(rec.calls.back() == "insert -1, in a step: 0");
    game::on_world_exit();
}

TEST_CASE("game-api: spawn on Server.exe") {
    // covers: game-api/Not mapped on this build
    Loader& l = L();
    const HostInfo saved = l.host;
    l.host.host = KY_HOST_SERVER;
    l.host.build_id = "test-build";
    cube_World* w = alloc_zeroed<cube_World>();
    FakeWorld fake;
    fake.init(w);
    load_base_map("verified", false);   // the recipe's functions are verified in the map, but spawn is for Cube.exe
    game::boot();
    step(w);
    TestMod m;
    RecipeLog rec;
    char why[256] = {};
    CHECK(api_table()->game_available("spawn", why, sizeof why) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(std::string(why) == "not mapped for Server.exe yet", why);
    const KyubuSpawn s = slime();
    KyubuCreature h = 77;
    CHECK(api_table()->creature_spawn(m.on(KY_SIDE_SERVER), &s, &h) == KY_E_UNAVAILABLE);
    CHECK(h == 0);
    CHECK(game::last_refusal() == "spawn is unavailable: not mapped for Server.exe yet");
    CHECK(rec.calls.empty());   // the game's memory is unchanged
    CHECK(g_allocated.empty());
    CHECK(w->creatures.size == 2);

    game::remove_world_hooks();
    game::remove_handle_hooks();
    DeleteCriticalSection((CRITICAL_SECTION*)&w->lock);
    VirtualFree(w, 0, MEM_RELEASE);
    SymbolTable::get().clear();
    l.host = saved;
    game::check_entries();
}

TEST_CASE("game-api: creatures in Lua") {
    // covers: game-api/Same creature, same object, and game-api/No world yet and game-api/Gone creature in Lua
    // through the Lua binding, in a full-trust mod; and kyubu.game.creature_spawn on both sides
    ClientGame g;
    TestMod m;
    RecipeLog rec;
    game::on_world_exit();
    Loader& l = L();
    const std::string saved_kyubu = l.kyubu_dir, saved_mods = l.mods_dir;
    const auto saved_phase = l.phase.load();
    const std::string game_dir = temp_dir("lua-creatures");
    l.kyubu_dir = game_dir + "/kyubu";
    l.mods_dir = game_dir + "/mods";
    l.phase = KY_PHASE_PREINIT;
    const std::string mod = l.mods_dir + "/fx_game_creatures";
    REQUIRE(make_dirs(mod + "/scripts"));
    REQUIRE(write_text(mod + "/mod.toml", "id = \"fx_game_creatures\"\nname = \"Game API: creatures from Lua\"\n"
                                          "version = \"1.0.0\"\nkyubu = \"^1.0\"\nsides = [\"client\", \"server\"]\n"
                                          "trust = \"full\"\ndescription = \"Unit test: kyubu.game.creature_player and "
                                          "creature_get.\"\n\n[scripts]\nclient = \"scripts/client.lua\"\n"
                                          "server = \"scripts/server.lua\"\n"));
    REQUIRE(write_text(mod + "/scripts/client.lua", R"(
local game = kyubu.game
local function say(s) kyubu.info("fx_game_creatures: " .. s) end
local p, why = game.creature_player()
say("menu " .. tostring(p) .. "/" .. tostring(why))
local player
kyubu.on("fx_game_creatures:world", function()
  local a, b = game.creature_player(), game.creature_player()
  local t = {}
  t[a] = "found"
  local info = a:get()
  say(("same=%s key=%s valid=%s pos=%.0f,%.0f,%.0f yaw=%g hp=%g level=%d type=%d hostile=%d id=%d get=%s"):format(
    tostring(a == b and rawequal(a, b)), tostring(t[b]), tostring(a:valid()), info.pos.x, info.pos.y, info.pos.z,
    info.yaw, info.hp, info.level, info.type, info.hostile, info.id, tostring(game.creature_get(b).id == info.id)))
  say("str=" .. tostring(a) .. " meta=" .. tostring(getmetatable(a)))
  local ok, err = pcall(game.creature_get, 42)
  say("bad=" .. tostring(ok) .. "/" .. tostring(err))
  local none, none_why = game.creature_spawn{ type = 37, hostile = 1, level = 1, pos = {0, 0, 0} }
  say("client spawn " .. tostring(none) .. "/" .. tostring(none_why))
  player = a
end)
kyubu.on("fx_game_creatures:gone", function()
  local info, info_why = player:get()
  local got, got_why = game.creature_get(player)
  say(("gone valid=%s get=%s/%s creature_get=%s/%s str=%s"):format(tostring(player:valid()), tostring(info),
    tostring(info_why), tostring(got), tostring(got_why), tostring(player)))
end)
)"));
    REQUIRE(write_text(mod + "/scripts/server.lua", R"(
local game = kyubu.game
local function say(s) kyubu.info("fx_game_creatures: " .. s) end
local p, why = game.creature_player()
say("server " .. tostring(p) .. "/" .. tostring(why))
kyubu.on("fx_game_creatures:spawn", function()
  local a = game.creature_spawn{ type = 37, hostile = 1, level = 5, pos = {6553605, 6684672, 7}, yaw = 180 }
  local b = game.creature_spawn{ type = 37, hostile = 2, level = 1, pos = {x = 1.75, y = -2.5, z = 3} }
  local ia, ib = a:get(), b:get()
  say(("spawn a=%d,%d,%d,%d,%g pos=%.0f,%.0f,%.0f yaw=%g valid=%s b pos=%.0f,%.0f,%.0f yaw=%g hostile=%d"):format(
    ia.id, ia.type, ia.hostile, ia.level, ia.hp, ia.pos.x, ia.pos.y, ia.pos.z, ia.yaw, tostring(a:valid()),
    ib.pos.x, ib.pos.y, ib.pos.z, ib.yaw, ib.hostile))
  local c, c_why = game.creature_spawn{ type = 9999, hostile = 1, level = 1, pos = {0, 0, 0} }
  say("refused " .. tostring(c) .. "/" .. tostring(c_why))
  local ok, err = pcall(game.creature_spawn, { type = 37, hostile = 1, level = 1.5, pos = {0, 0, 0} })
  say("level=" .. tostring(ok) .. "/" .. tostring(err))
  ok, err = pcall(game.creature_spawn, { type = 37, hostile = 1, level = 1, pos = {0, 0, 0}, behavior = "dance" })
  say("behavior=" .. tostring(ok) .. "/" .. tostring(err))
  local m = game.creature_spawn{ type = 37, hostile = 1, level = 1, pos = {0, 0, 0}, behavior = "monster" }
  say("monster=" .. tostring(m ~= nil and m:valid()))
  local no_owner, no_owner_why = game.creature_spawn{ type = 37, hostile = 3, level = 1, pos = {0, 0, 0}, behavior = "companion" }
  say("companion " .. tostring(no_owner) .. "/" .. tostring(no_owner_why))
  local pet = game.creature_spawn{ type = 37, hostile = 3, level = 1, pos = {0, 0, 0}, behavior = "companion", owner = a }
  say("pet=" .. tostring(pet ~= nil and pet:valid()))
  local guard = game.creature_spawn{ type = 37, hostile = 3, level = 1, pos = {0, 0, 0}, behavior = "guard",
                                     path = { {1, 2, 3}, {x = 4, y = 5, z = 6} } }
  say("guard=" .. tostring(guard ~= nil and guard:valid()))
  ok, err = pcall(game.creature_spawn, { type = 37, hostile = 3, level = 1, pos = {0, 0, 0}, behavior = "guard", path = { {1, 2} } })
  say("path=" .. tostring(ok) .. "/" .. tostring(err))
  ok, err = pcall(game.creature_spawn, { type = 37, hostile = 3, level = 1, pos = {0, 0, 0}, behavior = "companion", owner = 5 })
  say("owner=" .. tostring(ok) .. "/" .. tostring(err))
  local guard_none, guard_none_why = game.creature_spawn{ type = 37, hostile = 3, level = 1, pos = {0, 0, 0}, behavior = "guard" }
  say("guard " .. tostring(guard_none) .. "/" .. tostring(guard_none_why))
  local d = game.creature_spawn{ type = 37, hostile = 6, level = 1, pos = {0, 0, 0}, behavior = "default" }
  say("default=" .. tostring(d ~= nil and d:valid()))
end)
)"));

    const uint64_t seq = Log::get().sequence();
    auto& mm = ModManager::get();
    mm.discover_and_resolve();
    mm.load_native();
    LuaRuntime::get().load_all();
    Mod* fx = mm.find("fx_game_creatures");
    REQUIRE(fx);
    CHECK(fx->state == ModState::Loaded);

    // in a world: the player twice and its state; then the game destroys it
    game::on_world_enter();
    step(&g.gc->world);
    set_player_state(g.fake.creatures[0]);
    KyubuCreature h = 0;
    REQUIRE(api_table()->creature_player(m.on(KY_SIDE_CLIENT), &h) == KY_OK);   // handles are per side, not per mod
    Events::get().emit("fx_game_creatures:world", KY_SIDE_CLIENT);
    destroy(g.fake.creatures[0]);
    Events::get().emit("fx_game_creatures:gone", KY_SIDE_CLIENT);
    Events::get().emit("fx_game_creatures:spawn", KY_SIDE_SERVER);

    std::vector<std::string> lines;
    for (auto& entry : Log::get().since(seq)) {
        const size_t at = entry.text.find("fx_game_creatures: ");
        if (at != std::string::npos) lines.push_back(entry.text.substr(at + strlen("fx_game_creatures: ")));
    }
    std::string all;
    for (auto& s : lines) all += s + "\n";
    auto has = [&](const std::string& s) { return std::find(lines.begin(), lines.end(), s) != lines.end(); };
    const std::string object = fmt("creature 0x%016llX", (unsigned long long)h);
    CHECK_MESSAGE(has("menu nil/no world is loaded"), all);
    CHECK_MESSAGE(has("server nil/creature_player is client-only and was called from the server side"), all);
    CHECK_MESSAGE(has("same=true key=found valid=true pos=6553605,-196608,549755813895 yaw=90 hp=123.5 level=7 type=2 "
                      "hostile=0 id=-5 get=true"), all);
    CHECK_MESSAGE(has("str=" + object + " (id -5) meta=creature"), all);
    CHECK_MESSAGE(has("bad=false/kyubu.game.creature_get: expected a creature, got number"), all);
    CHECK_MESSAGE(has("gone valid=false get=nil/the creature is gone creature_get=nil/the creature is gone str=" + object +
                      " (gone)"), all);
    CHECK_MESSAGE(has("client spawn nil/creature_spawn is server-only and was called from the client side"), all);
    CHECK_MESSAGE(has("spawn a=-1,37,1,5,105 pos=6553605,6684672,7 yaw=180 valid=true b pos=1,-3,3 yaw=0 hostile=2"), all);
    CHECK_MESSAGE(has("refused nil/entity_type is 9999, not a value of cube::EntityType (game_enum lists them)"), all);
    CHECK_MESSAGE(has("level=false/kyubu.game.creature_spawn: 'level' must be an integer from -2147483648 to 2147483647, "
                      "got 1.5"), all);
    CHECK_MESSAGE(has("behavior=false/kyubu.game.creature_spawn: 'behavior' must be \"still\", \"monster\", "
                      "\"friendly\", \"companion\", \"guard\" or \"default\", got \"dance\""), all);
    CHECK_MESSAGE(has("monster=true"), all);
    CHECK_MESSAGE(has("companion nil/owner required: KY_SPAWN_COMPANION needs the handle of the creature it follows in owner"), all);
    CHECK_MESSAGE(has("pet=true"), all);
    CHECK_MESSAGE(has("guard=true"), all);
    CHECK_MESSAGE(has("path=false/kyubu.game.creature_spawn: path[1].z must be a number of world units, got nil"), all);
    CHECK_MESSAGE(has("owner=false/kyubu.game.creature_spawn: 'owner': expected a creature, got number"), all);
    CHECK_MESSAGE(has("guard nil/path required: KY_SPAWN_GUARD needs 1 to 64 waypoints in path (path_count is 0)"), all);
    CHECK_MESSAGE(has("default=true"), all);
    CHECK(lines.size() == 19);

    // leave no mods and no world behind for other tests
    game::on_world_exit();
    l.mods_dir = temp_dir("lua-creatures-empty");
    mm.discover_and_resolve();
    l.kyubu_dir = saved_kyubu;
    l.mods_dir = saved_mods;
    l.phase = saved_phase;
}

// ---- the fourth ability slot's entries (openspec add-ability-api, task 4.1) ----

// Stand-ins for the eight functions and sites of the ability primitives. Each has its own code, so each gets its own
// signature in the test symbol map.
#define ABILITY_STUB(n) \
    extern "C" NOINLINE int __cdecl game_test_ability_stub_##n(int x) { volatile int v = x * (n + 3); return v + (n << 4); }
ABILITY_STUB(0) ABILITY_STUB(1) ABILITY_STUB(2) ABILITY_STUB(3)
ABILITY_STUB(4) ABILITY_STUB(5) ABILITY_STUB(6) ABILITY_STUB(7)

namespace {

const char* const kAbilitySymbols[8] = {
    "cube::World::abilityDispatchSite", "cube::GameController::abilityBarBuiltSite", "IntVector::push_back",
    "ability_cooldown",                 "ability_cost",                              "cube::Creature::modeDuration",
    "save_character",                   "load_character"};

// The base map plus the eight symbols above, each verified except `candidate` (its name, or null for none).
void load_ability_map(const char* candidate) {
    load_base_map("verified");
    static const void* const fns[8] = {
        (const void*)&game_test_ability_stub_0, (const void*)&game_test_ability_stub_1, (const void*)&game_test_ability_stub_2,
        (const void*)&game_test_ability_stub_3, (const void*)&game_test_ability_stub_4, (const void*)&game_test_ability_stub_5,
        (const void*)&game_test_ability_stub_6, (const void*)&game_test_ability_stub_7};
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    std::string text = "build = \"test-build\"\n";
    for (int i = 0; i < 8; ++i)
        text += fmt("\n[[symbol]]\nname = \"%s\"\nrva = %u\nsignature = \"%s\"\nconvention = \"thiscall\"\nstatus = \"%s\"\n",
                    kAbilitySymbols[i], (uint32_t)((uintptr_t)fns[i] - base), code_bytes(fns[i]).c_str(),
                    candidate && strcmp(candidate, kAbilitySymbols[i]) == 0 ? "candidate" : "verified");
    std::vector<std::string> errors;
    SymbolTable::get().load_text(text, "ability-test.toml", "test-build", "kyubu", errors);
    REQUIRE(errors.empty());
}

int count_lines(uint64_t since, const char* a, const char* b = "", KyubuLogLevel level = (KyubuLogLevel)-1) {
    int n = 0;
    for (auto& line : Log::get().since(since))
        if (contains(line.text, a) && contains(line.text, b) && (level == (KyubuLogLevel)-1 || line.level == level)) ++n;
    return n;
}

}  // namespace

TEST_CASE("game-api: the fourth ability slot needs five primitives") {
    // covers: fourth-ability-slot/All or nothing (the gate; the unit-test half of "A candidate entry")
    Loader& l = L();
    const HostInfo saved = l.host;
    l.host.host = KY_HOST_CLIENT;
    l.host.build_id = "test-build";
    const KyubuApi* api = api_table();
    char why[256] = {};
    std::string slot_why;

    // every entry of the five primitives verified: the slot is available
    load_ability_map(nullptr);
    uint64_t seq = Log::get().sequence();
    game::check_entries();
    for (const char* p : {"ability_state", "ability_step", "ability_bar", "ability_stats", "ability_icons"})
        CHECK_MESSAGE(api->game_available(p, why, sizeof why) == KY_OK, p << ": " << why);
    CHECK_MESSAGE(game::ability_slot_available(&slot_why), slot_why);
    CHECK(slot_why.empty());
    CHECK(count_lines(seq, "the fourth ability slot is available") == 1);
    CHECK(count_lines(seq, "ability", "", KY_LOG_WARN) == 0);
    // outside the set: stun_time is a verified field of the loader's layouts since task 2.7 (probe ability-interrupts), as are
    // the rank entries here, so both are available. The unavailable path of ability_stun (a stun ends an ability with
    // "interrupted") has no stand-in symbol to test it with: the reason logic of 4.4 owns it
    CHECK_MESSAGE(api->game_available("ability_stun", why, sizeof why) == KY_OK, why);
    CHECK(count_lines(seq, "ability_stun is unavailable", "interrupted") == 0);
    CHECK(api->game_available("ability_ranks", why, sizeof why) == KY_OK);
    bool seen = false;
    for (auto& line : game::report())
        if (line == "fourth ability slot: available") seen = true;
    CHECK(seen);

    // one entry of the set is candidate: the slot is off, the log names the entry once, the other primitives stay
    load_ability_map("IntVector::push_back");
    seq = Log::get().sequence();
    game::check_entries();
    CHECK(api->game_available("ability_bar", why, sizeof why) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(contains(why, "IntVector::push_back: candidate, not verified (test-build)"), why);
    CHECK(api->game_available("ability_step", why, sizeof why) == KY_OK);
    CHECK(api->game_available("ability_icons", why, sizeof why) == KY_OK);
    CHECK(!game::ability_slot_available(&slot_why));
    CHECK_MESSAGE(slot_why == "ability_bar: IntVector::push_back: candidate, not verified (test-build)", slot_why);
    CHECK(count_lines(seq, "the fourth ability slot is off", "IntVector::push_back", KY_LOG_WARN) == 1);
    CHECK(count_lines(seq, "IntVector::push_back") == 1);   // once, not once per primitive
    seen = false;
    for (auto& line : game::report())
        if (line == "fourth ability slot: ability_bar: IntVector::push_back: candidate, not verified (test-build)") seen = true;
    CHECK(seen);

    // a missing entry of another primitive of the set blocks it the same way
    load_ability_map("ability_cost");
    game::check_entries();
    CHECK(!game::ability_slot_available(&slot_why));
    CHECK_MESSAGE(contains(slot_why.c_str(), "ability_stats: ability_cost: candidate, not verified"), slot_why);

    // ranks not mapped yet: save_character is candidate, the slot still runs, and the blocking entry is named
    load_ability_map("save_character");
    seq = Log::get().sequence();
    game::check_entries();
    CHECK_MESSAGE(game::ability_slot_available(&slot_why), slot_why);
    CHECK(api->game_available("ability_ranks", why, sizeof why) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(contains(why, "save_character: candidate, not verified (test-build)"), why);
    CHECK(count_lines(seq, "ability_ranks is unavailable", "every ability rank reads 1") == 1);
    CHECK(count_lines(seq, "ability", "", KY_LOG_WARN) == 0);

    // Server.exe has no skill bar: its slot is state + step + stats (add-skills-unleashed-multiplayer 3.2), the bar and icons
    // are "not mapped for Server.exe yet", and nothing is a warning or an error in the log
    load_ability_map(nullptr);
    l.host.host = KY_HOST_SERVER;
    seq = Log::get().sequence();
    game::check_entries();
    CHECK_MESSAGE(game::ability_slot_available(&slot_why), slot_why);
    CHECK(api->game_available("ability_state", why, sizeof why) == KY_OK);   // its entries are mapped for both builds
    CHECK(api->game_available("ability_step", why, sizeof why) == KY_OK);
    CHECK(api->game_available("ability_stats", why, sizeof why) == KY_OK);
    CHECK(api->game_available("ability_bar", why, sizeof why) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(contains(why, "not mapped for Server.exe yet"), why);
    CHECK(count_lines(seq, "ability", "", KY_LOG_WARN) == 0);
    CHECK(count_lines(seq, "ability", "", KY_LOG_ERROR) == 0);

    // a server twin still `candidate` (the stat functions before the user's review) keeps the server's slot off and names it
    load_ability_map("cube::Creature::modeDuration");
    seq = Log::get().sequence();
    game::check_entries();
    CHECK(!game::ability_slot_available(&slot_why));
    CHECK_MESSAGE(contains(slot_why.c_str(), "ability_stats: cube::Creature::modeDuration: candidate, not verified"), slot_why);
    CHECK(count_lines(seq, "the fourth ability slot is off on Server.exe") == 1);
    CHECK(count_lines(seq, "ability", "", KY_LOG_WARN) == 0);

    SymbolTable::get().clear();
    l.host = saved;
    game::check_entries();
}

// ---- the game API for abilities (openspec add-ability-api, task 6.1) ----

// Stand-ins for the two game functions ability_start asks, with the game's ABIs (thiscall, the creature in ECX).
static volatile bool g_use_interruptible = true;
static volatile bool g_use_can_use = true;
static volatile int32_t g_use_asked = -1;
extern "C" NOINLINE bool __fastcall game_test_mode_interruptible(cube_Creature*, void*) { return g_use_interruptible; }
extern "C" NOINLINE bool __fastcall game_test_can_use_ability(cube_Creature*, void*, int32_t id) {
    g_use_asked = id;
    return g_use_can_use;
}

namespace {

// The eight ability symbols of load_ability_map, and the two of ability_start (verified).
void load_ability_use_map(const char* candidate) {
    load_ability_map(candidate);
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    const struct { const char* name; const void* fn; } fns[] = {
        {"cube::Creature::modeInterruptible", (const void*)&game_test_mode_interruptible},
        {"cube::Creature::canUseAbility", (const void*)&game_test_can_use_ability}};
    std::string text = "build = \"test-build\"\n";
    for (auto& f : fns)
        text += fmt("\n[[symbol]]\nname = \"%s\"\nrva = %u\nsignature = \"%s\"\nconvention = \"thiscall\"\nstatus = \"verified\"\n",
                    f.name, (uint32_t)((uintptr_t)f.fn - base), code_bytes(f.fn).c_str());
    std::vector<std::string> errors;
    SymbolTable::get().load_text(text, "ability-use-test.toml", "test-build", "kyubu", errors);
    REQUIRE(errors.empty());
}

// A creature's cooldown map with one entry: std::map<int, int> as the game lays it out (head, root; key at +0x10, value at +0x14).
struct CooldownMap {
    struct Node {
        std_tree_node n;
        int32_t key;
        int32_t value;
    };
    static_assert(offsetof(Node, key) == 0x10 && offsetof(Node, value) == 0x14, "the fake follows the game's node");
    Node head = {}, root = {};
    void set(cube_Creature* c, int key, int value) {
        head = {};
        root = {};
        head.n.isnil = 1;
        head.n.parent = &root.n; head.n.left = &root.n; head.n.right = &root.n;
        root.n.parent = &head.n; root.n.left = &head.n; root.n.right = &head.n;
        root.key = key;
        root.value = value;
        c->ability_cooldowns.head = &head.n;
        c->ability_cooldowns.size = 1;
    }
};

std::vector<std::string> g_ability_calls;   // what the callbacks of game-test:burst saw
void KYUBU_CALL use_on_start(void*, const KyubuAbilityCtx* c) { g_ability_calls.push_back("start " + std::string(c->id)); }
void KYUBU_CALL use_on_step(void*, const KyubuAbilityCtx* c) { g_ability_calls.push_back("step " + std::to_string(c->steps)); }
void KYUBU_CALL use_on_end(void*, const KyubuAbilityCtx* c) {
    g_ability_calls.push_back(std::string("end ") + game::end_reason_name((game::EndReason)c->reason));
}

void reset_ability_registries() {
    Registries& R = Registries::get();
    R.~Registries();
    new (&R) Registries();
    R.init_builtin();
    ability_callbacks_clear();
}

// game-test:burst (number 112: 800 ms, mp 0.25) defined and frozen, the ten symbols verified, the World with its player
// (creature 1, the local player, hp 100, mp 1) and a monster (creature 2, hostile_type 1).
struct AbilityApiRig {
    ClientGame g;
    TestMod m;
    CooldownMap cd;
    KyubuPhase phase = L().phase.load();
    cube_Creature* player;
    cube_Creature* monster;
    AbilityApiRig() {
        reset_ability_registries();
        game::reset_ability_step_for_test();
        g_ability_calls.clear();
        g_use_interruptible = true;
        g_use_can_use = true;
        g_use_asked = -1;
        L().phase = KY_PHASE_REGISTER;
        static const uint32_t cooldown[4] = {6000, 5500, 5000, 4500};
        KyubuAbilityDef d = {};
        d.size = sizeof d;
        d.duration_ms = 800;
        d.mp_cost = 0.25f;
        d.max_rank = 3;
        d.cooldown_ms = cooldown;
        d.slot = KY_ABILITY_SLOT_FOURTH;
        d.class_type = 1;
        d.specialization = KY_ABILITY_SPEC_ANY;
        d.on_start = use_on_start;
        d.on_step = use_on_step;
        d.on_end = use_on_end;
        REQUIRE(api_table()->ability_define(m.on(KY_SIDE_CLIENT), "game-test:burst", &d) == KY_OK);
        Registries::get().freeze([](const std::string& o) { return o == "game-test" ? 0 : 100; });
        ability_slots_resolve(true);
        load_ability_use_map(nullptr);
        game::check_entries();
        player = g.fake.creatures[0];
        monster = g.fake.creatures[1];
        player->data.hp = 100;
        player->data.mp = 1.0f;
        monster->data.hp = 100;
        monster->data.mp = 1.0f;
        monster->data.hostile_type = 1;
        game::on_world_enter();
        step(&g.gc->world);
    }
    ~AbilityApiRig() {
        game::on_world_exit();
        game::reset_ability_step_for_test();
        L().phase = phase;
        reset_ability_registries();
    }
    KyubuCreature client(cube_Creature* c) { return game::handle_for(KY_SIDE_CLIENT, c); }
    KyubuCreature server(cube_Creature* c) { return game::handle_for(KY_SIDE_SERVER, c); }
    KyubuModHandle on_client() { return m.on(KY_SIDE_CLIENT); }
    KyubuModHandle on_server() { return m.on(KY_SIDE_SERVER); }
    // One World step with the hook at the site, then the pass (the mode time moves on after the site).
    void run(cube_Creature* c) {
        game::ability_step_creature(&g.gc->world, c);
        if (c->data.current_mode >= 111) c->data.mode_start_time += 16;
        game::ability_end_pass();
    }
};

}  // namespace

TEST_CASE("game-api: reading a creature's ability and its cooldowns") {
    // covers: game-api/A vanilla ability, Cooldown left, Gone (ability_get and ability_cooldown)
    AbilityApiRig r;
    const KyubuApi* api = api_table();
    const KyubuCreature h = r.client(r.player);
    KyubuAbilityInfo info = {};
    info.size = sizeof info;

    // in no ability: number 0, no id
    CHECK(api->ability_get(r.on_client(), h, &info) == KY_OK);
    CHECK(info.number == 0);
    CHECK(std::string(info.id).empty());
    CHECK(info.elapsed_ms == 0);

    // a vanilla ability, and the time since it started grows between two reads
    r.player->data.current_mode = 54;
    r.player->data.mode_start_time = 100;
    REQUIRE(api->ability_get(r.on_client(), h, &info) == KY_OK);
    CHECK(info.number == 54);
    CHECK(std::string(info.id) == "cubeworld:smash");
    CHECK(info.elapsed_ms == 100);
    r.player->data.mode_start_time = 340;
    REQUIRE(api->ability_get(r.on_client(), h, &info) == KY_OK);
    CHECK(info.elapsed_ms == 340);

    // a mod's ability, and a number the registry has no name for (an empty id)
    r.player->data.current_mode = 112;
    REQUIRE(api->ability_get(r.on_client(), h, &info) == KY_OK);
    CHECK(info.number == 112);
    CHECK(std::string(info.id) == "game-test:burst");
    r.player->data.current_mode = 200;
    REQUIRE(api->ability_get(r.on_client(), h, &info) == KY_OK);
    CHECK(info.number == 200);
    CHECK(std::string(info.id).empty());

    // a caller whose struct ends after `number` gets the first two fields; below that, or NULL, is a bad argument
    struct Small { uint32_t size, number; } small = {sizeof(Small), 77};
    CHECK(api->ability_get(r.on_client(), h, (KyubuAbilityInfo*)&small) == KY_OK);
    CHECK(small.number == 200);
    small.size = 4;
    CHECK(api->ability_get(r.on_client(), h, (KyubuAbilityInfo*)&small) == KY_E_INVALID_ARG);
    CHECK(api->ability_get(r.on_client(), h, nullptr) == KY_E_INVALID_ARG);

    // the other side's handle, and a handle that is not one
    CHECK(api->ability_get(r.on_server(), h, &info) == KY_E_SIDE);
    CHECK(api->ability_get(r.on_client(), 0, &info) == KY_E_INVALID_ARG);

    // cooldowns: the map's value for a number, 0 for an entry that ran out, a number with no entry, or a creature with no map
    uint32_t ms = 99;
    CHECK(api->ability_cooldown(r.on_client(), h, 111, &ms) == KY_OK);
    CHECK(ms == 0);   // the creature has no cooldown map at all
    r.cd.set(r.player, 111, 1500);
    CHECK(api->ability_cooldown(r.on_client(), h, 111, &ms) == KY_OK);
    CHECK(ms == 1500);
    CHECK(api->ability_cooldown(r.on_client(), h, 112, &ms) == KY_OK);
    CHECK(ms == 0);
    r.cd.set(r.player, 54, 0);
    CHECK(api->ability_cooldown(r.on_client(), h, 54, &ms) == KY_OK);
    CHECK(ms == 0);
    r.cd.set(r.player, 54, -20);
    CHECK(api->ability_cooldown(r.on_client(), h, 54, &ms) == KY_OK);
    CHECK(ms == 0);
    r.cd.set(r.player, 54, 3000);   // a vanilla number reads the same map
    CHECK(api->ability_cooldown(r.on_client(), h, 54, &ms) == KY_OK);
    CHECK(ms == 3000);
    CHECK(api->ability_cooldown(r.on_client(), h, 256, &ms) == KY_E_INVALID_ARG);
    CHECK(api->ability_cooldown(r.on_client(), h, 54, nullptr) == KY_E_INVALID_ARG);

    // a monster is the server side's, and either side reads it through its own handle
    CHECK(api->ability_get(r.on_server(), r.server(r.monster), &info) == KY_OK);
    CHECK(api->ability_get(r.on_client(), r.client(r.monster), &info) == KY_OK);

    // the game destroys the creature: both calls fail as gone, and `out` is left alone
    destroy(r.player);
    info.number = 7;
    CHECK(api->ability_get(r.on_client(), h, &info) == KY_E_GONE);
    CHECK(info.number == 7);
    ms = 5;
    CHECK(api->ability_cooldown(r.on_client(), h, 54, &ms) == KY_E_GONE);
    CHECK(ms == 0);
}

TEST_CASE("game-api: setting a creature's cooldown") {
    // covers: add-skills-unleashed-multiplayer 8.1 (ability_cooldown_set: a Fire Mage gives its cooldown back after a fizzle)
    AbilityApiRig r;
    const KyubuApi* api = api_table();
    const KyubuCreature h = r.client(r.player);
    uint32_t ms = 0;

    // the map's entry is written, a lower value and 0 too
    r.cd.set(r.player, 112, 5500);
    CHECK(api->ability_cooldown_set(r.on_client(), h, 112, 80000) == KY_OK);
    CHECK(r.cd.root.value == 80000);
    CHECK(api->ability_cooldown_set(r.on_client(), h, 112, 0) == KY_OK);
    CHECK(r.cd.root.value == 0);
    CHECK(api->ability_cooldown(r.on_client(), h, 112, &ms) == KY_OK);
    CHECK(ms == 0);

    // a number with no entry: nothing is made (the game makes it at the first step)
    CHECK(api->ability_cooldown_set(r.on_client(), h, 111, 3000) == KY_OK);
    CHECK(r.cd.root.key == 112);
    CHECK(r.player->ability_cooldowns.size == 1);

    // bad numbers and times
    CHECK(api->ability_cooldown_set(r.on_client(), h, 0, 10) == KY_E_INVALID_ARG);
    CHECK(api->ability_cooldown_set(r.on_client(), h, 256, 10) == KY_E_INVALID_ARG);
    CHECK(api->ability_cooldown_set(r.on_client(), h, 112, KY_ABILITY_COOLDOWN_MAX + 1) == KY_E_INVALID_ARG);
    CHECK(api->ability_cooldown_set(r.on_client(), h, 112, KY_ABILITY_COOLDOWN_MAX) == KY_OK);

    // only the side that owns the creature: the client cannot write a monster's, the server can
    r.cd.set(r.monster, 112, 400);
    CHECK(api->ability_cooldown_set(r.on_client(), r.client(r.monster), 112, 0) == KY_E_SIDE);
    CHECK(r.cd.root.value == 400);
    CHECK(api->ability_cooldown_set(r.on_server(), r.server(r.monster), 112, 0) == KY_OK);
    CHECK(r.cd.root.value == 0);

    destroy(r.player);
    CHECK(api->ability_cooldown_set(r.on_client(), h, 112, 0) == KY_E_GONE);
}

TEST_CASE("game-api: starting and ending a mod ability") {
    // covers: game-api/Start by API, Refused on cooldown, Vanilla refused, Not the owner (ability_start and ability_end)
    AbilityApiRig r;
    const KyubuApi* api = api_table();
    const KyubuCreature h = r.client(r.player);
    cube_World* w = &r.g.gc->world;

    // vanilla and undefined numbers are bad arguments, and the creature is unchanged
    for (uint32_t number : {0u, 54u, 110u, 113u, 255u, 300u}) {
        CHECK_MESSAGE(api->ability_start(r.on_client(), h, number) == KY_E_INVALID_ARG, number);
        CHECK(r.player->data.current_mode == 0);
    }
    CHECK(g_use_asked == -1);   // the game's canUseAbility was not asked

    // the start: the id becomes the mode and the time 0, the way abilityStartSite does it; nothing else is written yet
    r.player->data.mode_start_time = 5000;
    CHECK(api->ability_start(r.on_client(), h, 112) == KY_OK);
    CHECK(r.player->data.current_mode == 112);
    CHECK(r.player->data.mode_start_time == 0);
    CHECK(r.player->data.mp == 1.0f);
    CHECK(g_use_asked == 112);
    CHECK(g_ability_calls.empty());

    // the step runner then runs it as it runs a press of key 4: on_start, on_step, the mp taken once
    r.run(r.player);
    CHECK(r.player->data.mp == 0.75f);
    REQUIRE(g_ability_calls.size() == 2);
    CHECK(g_ability_calls[0] == "start game-test:burst");
    CHECK(g_ability_calls[1] == "step 1");
    r.run(r.player);
    CHECK(r.player->data.mp == 0.75f);

    // ending: the next safe point writes mode 0 and runs on_end with `cancelled`
    CHECK(api->ability_end(r.on_client(), h) == KY_OK);
    CHECK(r.player->data.current_mode == 112);   // not yet
    game::ability_end_pass();
    CHECK(r.player->data.current_mode == 0);
    CHECK(g_ability_calls.back() == "end cancelled");
    // and again: nothing is left to end
    CHECK(api->ability_end(r.on_client(), h) == KY_E_NOT_FOUND);

    // a start the step runner has not seen yet is taken back at once, and nothing ran
    g_ability_calls.clear();
    CHECK(api->ability_start(r.on_client(), h, 112) == KY_OK);
    CHECK(api->ability_end(r.on_client(), h) == KY_OK);
    CHECK(r.player->data.current_mode == 0);
    CHECK(g_ability_calls.empty());

    // on cooldown (the game stored it at the first step): refused, with the reason, and nothing starts
    r.cd.set(r.player, 112, 3377);
    CHECK(api->ability_start(r.on_client(), h, 112) == KY_E_REFUSED);
    CHECK_MESSAGE(game::last_refusal().find("on cooldown") != std::string::npos, game::last_refusal());
    CHECK_MESSAGE(game::last_refusal().find("3377 ms left") != std::string::npos, game::last_refusal());
    CHECK(r.player->data.current_mode == 0);
    r.cd.set(r.player, 112, 0);   // the cooldown ran out
    CHECK(api->ability_start(r.on_client(), h, 112) == KY_OK);
    r.player->data.current_mode = 0;

    // not enough mp: 0.25 is the cost
    r.player->data.mp = 0.2f;
    CHECK(api->ability_start(r.on_client(), h, 112) == KY_E_REFUSED);
    CHECK_MESSAGE(game::last_refusal().find("not enough mp") != std::string::npos, game::last_refusal());
    CHECK(r.player->data.current_mode == 0);
    r.player->data.mp = 0.25f;   // exactly the cost is enough, as the game's canUseAbility compares
    CHECK(api->ability_start(r.on_client(), h, 112) == KY_OK);
    r.player->data.current_mode = 0;
    r.player->data.mp = 1.0f;

    // busy: the game says the current mode cannot be interrupted
    g_use_interruptible = false;
    r.player->data.current_mode = 48;
    CHECK(api->ability_start(r.on_client(), h, 112) == KY_E_REFUSED);
    CHECK_MESSAGE(game::last_refusal().find("busy") != std::string::npos, game::last_refusal());
    CHECK(r.player->data.current_mode == 48);
    g_use_interruptible = true;
    r.player->data.current_mode = 0;

    // the game's own canUseAbility has the last word (a timer in the creature)
    g_use_can_use = false;
    CHECK(api->ability_start(r.on_client(), h, 112) == KY_E_REFUSED);
    CHECK_MESSAGE(game::last_refusal().find("canUseAbility") != std::string::npos, game::last_refusal());
    CHECK(r.player->data.current_mode == 0);
    g_use_can_use = true;

    // the owner: the client side starts only the local player's abilities; the server side the monsters'
    CHECK(api->ability_start(r.on_client(), r.client(r.monster), 112) == KY_E_SIDE);
    CHECK(r.monster->data.current_mode == 0);
    CHECK(api->ability_start(r.on_server(), r.server(r.player), 112) == KY_E_SIDE);
    CHECK(api->ability_end(r.on_client(), r.client(r.monster)) == KY_E_SIDE);
    CHECK(api->ability_start(r.on_server(), r.server(r.monster), 112) == KY_OK);
    CHECK(r.monster->data.current_mode == 112);
    r.run(r.monster);
    CHECK(r.monster->data.mp == 0.75f);
    CHECK(api->ability_end(r.on_server(), r.server(r.monster)) == KY_OK);
    game::ability_end_pass();
    CHECK(r.monster->data.current_mode == 0);

    // a creature the step runner would never run (another player's, in single-player): refused, not left to hang
    r.monster->data.hostile_type = 0;
    CHECK(api->ability_start(r.on_server(), r.server(r.monster), 112) == KY_E_REFUSED);
    CHECK_MESSAGE(game::last_refusal().find("step runner") != std::string::npos, game::last_refusal());
    CHECK(r.monster->data.current_mode == 0);
    r.monster->data.hostile_type = 1;
    w->joined = 1;   // a monster in a joined client's World is the server's: the client side has no say
    CHECK(api->ability_start(r.on_client(), r.client(r.monster), 112) == KY_E_SIDE);
    w->joined = 0;

    // a bad handle, and a gone creature
    CHECK(api->ability_start(r.on_client(), 0, 112) == KY_E_INVALID_ARG);
    CHECK(api->ability_end(r.on_client(), 0) == KY_E_INVALID_ARG);
    destroy(r.player);
    CHECK(api->ability_start(r.on_client(), h, 112) == KY_E_GONE);
    CHECK(api->ability_end(r.on_client(), h) == KY_E_GONE);
}

TEST_CASE("game-api: ability_start needs the fourth slot and the game's two functions") {
    // covers: game-api/Starting and ending a mod ability (unavailable)
    AbilityApiRig r;
    const KyubuApi* api = api_table();
    char why[256] = {};
    const KyubuCreature h = r.client(r.player);
    CHECK(api->game_available("ability_use", why, sizeof why) == KY_OK);

    // one of the slot's five primitives is candidate: ability_use stays available, the slot does not, and nothing is written
    load_ability_use_map("IntVector::push_back");
    game::check_entries();
    CHECK(api->game_available("ability_use", why, sizeof why) == KY_OK);
    CHECK(api->ability_start(r.on_client(), h, 112) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(game::last_refusal().find("IntVector::push_back") != std::string::npos, game::last_refusal());
    CHECK(r.player->data.current_mode == 0);
    CHECK(api->ability_end(r.on_client(), h) == KY_E_UNAVAILABLE);
    // the reads need only ability_state
    KyubuAbilityInfo info = {};
    info.size = sizeof info;
    CHECK(api->ability_get(r.on_client(), h, &info) == KY_OK);

    // a build where the game's two functions are not mapped: no ability_start, and the slot is untouched
    load_ability_map(nullptr);
    game::check_entries();
    CHECK(game::ability_slot_available());
    CHECK(api->game_available("ability_use", why, sizeof why) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(std::string(why).find("cube::Creature::modeInterruptible") != std::string::npos, why);
    CHECK(api->ability_start(r.on_client(), h, 112) == KY_E_UNAVAILABLE);
    CHECK(r.player->data.current_mode == 0);
    CHECK(api->ability_get(r.on_client(), h, &info) == KY_OK);

    // Server.exe has no skill bar
    load_ability_use_map(nullptr);
    L().host.host = KY_HOST_SERVER;
    game::check_entries();
    CHECK(api->game_available("ability_use", why, sizeof why) == KY_E_UNAVAILABLE);
    L().host.host = KY_HOST_CLIENT;
    game::check_entries();
}

// ---- the ability ranks (openspec add-ability-api, task 5.2) ----

namespace {

// The ranks file in a temporary folder; the binding reads it from there, and nothing is selected.
struct RankFile {
    std::filesystem::path dir;
    RankFile() {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        dir = std::filesystem::path(tmp) / ("kyubu-rank-bind-" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "Save");
        game::set_rank_path_for_test(file());
    }
    ~RankFile() {
        game::set_rank_path_for_test("");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string file() const { return (dir / "Save" / "characters.kyubu-abilities.json").string(); }
    std::string text() const {
        auto d = read_file(file());
        return d ? std::string(d->begin(), d->end()) : std::string("<missing>");
    }
    // The game starts again: the store forgets everything but the file.
    void restart() { game::set_rank_path_for_test(file()); }
};

void play(cube_Creature* c, int32_t slot, const char* name) {
    memset(c->data.name, 0, sizeof c->data.name);
    memcpy(c->data.name, name, strlen(name));
    game::ability_rank_character(slot, c);
}

}  // namespace

TEST_CASE("game-api: ability_rank_get and ability_rank_set") {
    // covers: game-api/Set to 0, Above the maximum; fourth-ability-slot/Kept across a restart, Another character
    AbilityApiRig r;
    RankFile f;
    const KyubuApi* api = api_table();
    uint32_t rank = 77;

    // nobody is selected yet: rank 1 on the bar, a refusal that says so, and no file
    CHECK(game::ability_rank_of(112) == 1);
    CHECK(api->ability_rank_get(r.on_client(), 112, &rank) == KY_E_REFUSED);
    CHECK_MESSAGE(game::last_refusal().find("no character is selected") != std::string::npos, game::last_refusal());
    CHECK(api->ability_rank_set(r.on_client(), 112, 2, &rank) == KY_E_REFUSED);
    CHECK(!file_exists(f.file()));

    // a character with no entry has rank 1; set clamps to the ability's max_rank (3), writes at once, and the bar's rank follows
    play(r.player, 0, "Tester");
    REQUIRE(api->ability_rank_get(r.on_client(), 112, &rank) == KY_OK);
    CHECK(rank == 1);
    REQUIRE(api->ability_rank_set(r.on_client(), 112, 2, &rank) == KY_OK);
    CHECK(rank == 2);
    CHECK(game::ability_rank_of(112) == 2);
    CHECK(f.text().find("\"game-test:burst\": 2") != std::string::npos);
    REQUIRE(api->ability_rank_set(r.on_client(), 112, 9, &rank) == KY_OK);   // Above the maximum
    CHECK(rank == 3);
    CHECK(game::ability_rank_of(112) == 3);
    REQUIRE(api->ability_rank_set(r.on_client(), 112, 0, nullptr) == KY_OK);   // Set to 0
    REQUIRE(api->ability_rank_get(r.on_client(), 112, &rank) == KY_OK);
    CHECK(rank == 0);
    CHECK(game::ability_rank_of(112) == 0);
    CHECK(api->ability_rank_set(r.on_client(), 112, 3, nullptr) == KY_OK);

    // Kept across a restart: the game starts again, the character is played again
    f.restart();
    CHECK(game::ability_rank_of(112) == 1);   // nothing selected after a restart
    play(r.player, 0, "Tester");
    REQUIRE(api->ability_rank_get(r.on_client(), 112, &rank) == KY_OK);
    CHECK(rank == 3);
    CHECK(game::ability_rank_of(112) == 3);

    // Another character: slot 1 has its own ranks, and a slot whose name differs reads the default
    play(r.player, 1, "Other");
    REQUIRE(api->ability_rank_get(r.on_client(), 112, &rank) == KY_OK);
    CHECK(rank == 1);
    play(r.player, 0, "Renamed");
    REQUIRE(api->ability_rank_get(r.on_client(), 112, &rank) == KY_OK);
    CHECK(rank == 1);
    play(r.player, 0, "Tester");
    REQUIRE(api->ability_rank_get(r.on_client(), 112, &rank) == KY_OK);
    CHECK(rank == 3);

    // a negative slot selects nothing new
    play(r.player, 1, "Other");
    game::ability_rank_character(-1, r.monster);
    REQUIRE(api->ability_rank_get(r.on_client(), 112, &rank) == KY_OK);
    CHECK(rank == 1);
    play(r.player, 0, "Tester");

    // arguments: a vanilla number, a number nobody defined, a NULL out
    CHECK(api->ability_rank_get(r.on_client(), 54, &rank) == KY_E_INVALID_ARG);
    CHECK(api->ability_rank_set(r.on_client(), 200, 1, &rank) == KY_E_INVALID_ARG);
    CHECK(api->ability_rank_get(r.on_client(), 112, nullptr) == KY_E_INVALID_ARG);
    CHECK(game::ability_rank_of(54) == 1);

    // client side only
    CHECK(api->ability_rank_get(r.on_server(), 112, &rank) == KY_E_SIDE);
    CHECK(api->ability_rank_set(r.on_server(), 112, 1, &rank) == KY_E_SIDE);

    // the file cannot be written (a folder is where it goes): the rank still holds in memory and the call says so
    std::error_code ec;
    std::filesystem::remove_all(f.dir / "Save", ec);
    std::filesystem::create_directories(f.dir / "Save" / "characters.kyubu-abilities.json");
    CHECK(api->ability_rank_set(r.on_client(), 112, 2, &rank) == KY_E_REFUSED);
    CHECK(rank == 2);
    CHECK(game::ability_rank_of(112) == 2);
    CHECK_MESSAGE(game::last_refusal().find("could not be written") != std::string::npos, game::last_refusal());
}

TEST_CASE("game-api: the ability ranks need ability_ranks") {
    // covers: fourth-ability-slot/All or nothing (the rank storage is not part of the set); game-api/The local player's ability ranks
    AbilityApiRig r;
    RankFile f;
    const KyubuApi* api = api_table();
    uint32_t rank = 0;
    play(r.player, 0, "Tester");
    REQUIRE(api->ability_rank_set(r.on_client(), 112, 3, &rank) == KY_OK);
    CHECK(game::ability_rank_of(112) == 3);

    // load_character is a candidate: the slot still runs, every rank reads 1, and both functions name the blocking entry
    load_ability_use_map("load_character");
    game::check_entries();
    CHECK(game::ability_slot_available());
    CHECK(game::ability_rank_of(112) == 1);
    CHECK(api->ability_rank_get(r.on_client(), 112, &rank) == KY_E_UNAVAILABLE);
    CHECK_MESSAGE(game::last_refusal().find("load_character: candidate, not verified") != std::string::npos, game::last_refusal());
    CHECK(api->ability_rank_set(r.on_client(), 112, 1, &rank) == KY_E_UNAVAILABLE);
    CHECK(f.text().find("\"game-test:burst\": 3") != std::string::npos);   // nothing was written

    // the stored rank comes back with the mapping
    load_ability_use_map(nullptr);
    game::check_entries();
    CHECK(game::ability_rank_of(112) == 3);

    // Server.exe has no skill bar and no ranks
    L().host.host = KY_HOST_SERVER;
    game::check_entries();
    CHECK(game::ability_rank_of(112) == 1);
    L().host.host = KY_HOST_CLIENT;
    game::check_entries();
}

TEST_CASE("game-api: the console command ability rank") {
    // covers: game-api/The local player's ability ranks (the console command)
    AbilityApiRig r;
    RankFile f;
    auto one = [](const std::vector<std::string>& v) { return v.size() == 1 ? v[0] : "<" + std::to_string(v.size()) + " lines>"; };
    CHECK(one(game::ability_rank_lines("")) == "usage: ability rank <id> [rank]");
    CHECK(contains(one(game::ability_rank_lines(" nobody:such")), "no ability \"nobody:such\""));
    CHECK(contains(one(game::ability_rank_lines(" game-test:burst")), "no character is selected"));
    play(r.player, 0, "Tester");
    CHECK(one(game::ability_rank_lines(" game-test:burst")) == "game-test:burst: rank 1 of 3");
    CHECK(one(game::ability_rank_lines(" game-test:burst 2")) == "game-test:burst: rank 2 of 3");
    CHECK(one(game::ability_rank_lines(" game-test:burst  9 ")) == "game-test:burst: rank 3 of 3");
    CHECK(one(game::ability_rank_lines(" game-test:burst")) == "game-test:burst: rank 3 of 3");
    CHECK(one(game::ability_rank_lines(" game-test:burst 0")) == "game-test:burst: rank 0 of 3");
    CHECK(game::ability_rank_of(112) == 0);
    CHECK(contains(one(game::ability_rank_lines(" game-test:burst x")), "the rank must be a whole number"));
    CHECK(contains(one(game::ability_rank_lines(" game-test:burst -1")), "the rank must be a whole number"));
    CHECK(contains(one(game::ability_rank_lines(" cubeworld:smash")), "not a defined mod ability"));
    load_ability_use_map("load_character");
    game::check_entries();
    CHECK(contains(one(game::ability_rank_lines(" game-test:burst")), "load_character: candidate, not verified"));
    load_ability_use_map(nullptr);
    game::check_entries();
}

TEST_CASE("game-api: particle_spawn queues an effect and the World step pushes it") {
    // covers: vanilla-particles/Starting one of the game's particle effects, vanilla-particles/A bad effect is refused
    // (the in-game check is probe particle-types)
    ClientGame g;
    TestMod m;
    const KyubuApi* api = api_table();
    const KyubuModHandle client = m.on(KY_SIDE_CLIENT), server = m.on(KY_SIDE_SERVER);
    game::on_world_enter();
    step(&g.gc->world);
    // the entries' layouts are candidates in the loader's compiled layouts until the user raises them: stand in for that
    game::force_primitive_for_test(game::Prim::particles, 1);
    REQUIRE(game::available(game::Prim::particles));
    game::clear_particles_for_test();

    KyubuParticle p = {};
    p.size = sizeof p;
    p.type = KY_PARTICLE_BOMB;
    p.pos[0] = 65536 * 10;
    p.pos[1] = -65536 * 3;
    p.pos[2] = 65536 * 2 + 5;
    p.accel[2] = 10.0f;
    p.color[0] = 0.6f;
    p.color[1] = 0.6f;
    p.color[2] = 1.0f;
    p.color[3] = 1.0f;
    p.scale = 0.5f;
    p.count = 8;
    p.spreading = 3.0f;

    // the server side is refused (the push is the client's), and nothing is queued
    const uint64_t seq = Log::get().sequence();
    CHECK(api->particle_spawn(server, &p) == KY_E_SIDE);
    CHECK(logged_lines(seq, "particle_spawn refused", "client-only") == 1);
    CHECK(game::queued_particles_for_test() == 0);

    // a field out of range is refused naming it, and nothing is queued
    struct Bad {
        void (*change)(KyubuParticle&);
        const char* why;
    };
    const Bad bad[] = {
        {[](KyubuParticle& b) { b.size = 8; }, "p->size is 8, below 72"},
        {[](KyubuParticle& b) { b.type = 5; }, "type is 5, above 4"},
        {[](KyubuParticle& b) { b.count = 0; }, "count is 0, outside 1 to 1000"},
        {[](KyubuParticle& b) { b.count = 1001; }, "count is 1001, outside 1 to 1000"},
        {[](KyubuParticle& b) { b.scale = 0.0f; }, "scale is 0, outside above 0 to 16"},
        {[](KyubuParticle& b) { b.scale = NAN; }, "accel, scale or spreading is not a finite number"},
        {[](KyubuParticle& b) { b.spreading = 65.0f; }, "spreading is 65, outside 0 to 64"},
        {[](KyubuParticle& b) { b.color[2] = 1.5f; }, "color[2] is outside 0 to 1"},
        {[](KyubuParticle& b) { b.accel[1] = INFINITY; }, "accel, scale or spreading is not a finite number"},
    };
    for (const Bad& b : bad) {
        KyubuParticle x = p;
        b.change(x);
        CHECK(api->particle_spawn(client, &x) == KY_E_INVALID_ARG);
        CHECK_MESSAGE(game::last_refusal() == b.why, game::last_refusal());
    }
    CHECK(api->particle_spawn(client, nullptr) == KY_E_INVALID_ARG);
    CHECK(game::queued_particles_for_test() == 0);

    // a good effect waits for a step, then lands on that step's output, in the output's particle list (+0x10), whole
    REQUIRE(api->particle_spawn(client, &p) == KY_OK);
    CHECK(game::queued_particles_for_test() == 1);
    std::vector<ParticlePush> pushes;
    g_particle_pushes = &pushes;
    cube_StepOutput out = {};
    auto volatile update = &game_test_world_update;
    update(&g.gc->world, nullptr, 20, &out, nullptr);
    g_particle_pushes = nullptr;
    REQUIRE(pushes.size() == 1);
    CHECK(pushes[0].list == (void*)((char*)&out + 0x10));
    CHECK(pushes[0].list == (void*)&out.particles);
    const cube_ParticleData& d = pushes[0].data;
    CHECK(d.pos.x == 65536 * 10);
    CHECK(d.pos.y == -65536 * 3);
    CHECK(d.pos.z == 65536 * 2 + 5);
    CHECK(d.accel[2] == 10.0f);
    CHECK(d.color[0] == 0.6f);
    CHECK(d.color[3] == 1.0f);
    CHECK(d.scale == 0.5f);
    CHECK(d.count == 8);
    CHECK(d.particle_type == KY_PARTICLE_BOMB);
    CHECK(d.spreading == 3.0f);
    CHECK(d.reserved == 0);
    CHECK(game::queued_particles_for_test() == 0);

    // nothing is pushed twice; a step with no output (the helper passes none) pushes nothing and keeps the effect for the next
    pushes.clear();
    g_particle_pushes = &pushes;
    update(&g.gc->world, nullptr, 20, &out, nullptr);
    CHECK(pushes.empty());
    REQUIRE(api->particle_spawn(client, &p) == KY_OK);
    step(&g.gc->world);
    CHECK(pushes.empty());
    CHECK(game::queued_particles_for_test() == 1);
    update(&g.gc->world, nullptr, 20, &out, nullptr);
    g_particle_pushes = nullptr;
    CHECK(pushes.size() == 1);
    CHECK(game::queued_particles_for_test() == 0);

    game::clear_particles_for_test();
    game::force_primitive_for_test(game::Prim::particles, -1);
}

TEST_CASE("game-api: light_spawn, light_set and light_remove put a mod's lights into the frame's light vector") {
    // covers: light-api/Starting a light, light-api/A bad light is refused, light-api/Moving and ending a light,
    // light-api/A light's life and fade, light-api/Lights end with their mod and the world (the in-game check is probe light-place)
    ClientGame g;
    TestMod m, other;
    other.mod.id = "game-test-other";
    const KyubuApi* api = api_table();
    const KyubuModHandle client = m.on(KY_SIDE_CLIENT), server = m.on(KY_SIDE_SERVER), client2 = other.on(KY_SIDE_CLIENT);
    game::on_world_enter();
    step(&g.gc->world);
    game::force_primitive_for_test(game::Prim::lights, 1);
    REQUIRE(game::available(game::Prim::lights));
    game::lights_world_exit();
    game::set_light_clock_for_test(10000);

    KyubuLight l = {};
    l.size = sizeof l;
    l.pos[0] = 65536 * 10;
    l.pos[1] = -65536 * 3;
    l.pos[2] = 65536 * 2 + 5;
    l.radius = 20.0f;
    l.color[0] = 1.6f;
    l.color[1] = 0.8f;
    l.color[2] = 0.24f;
    uint32_t id = 0;

    // the server side is refused, and no light starts
    const uint64_t seq = Log::get().sequence();
    CHECK(api->light_spawn(server, &l, &id) == KY_E_SIDE);
    CHECK(logged_lines(seq, "light_spawn refused", "client-only") == 1);
    CHECK(game::light_count_for_test() == 0);

    // a field out of range is refused naming it
    struct Bad {
        void (*change)(KyubuLight&);
        const char* why;
    };
    const Bad bad[] = {
        {[](KyubuLight& b) { b.size = 8; }, "l->size is 8, below 56"},
        {[](KyubuLight& b) { b.flags = 1; }, "flags is 1, not 0"},
        {[](KyubuLight& b) { b.radius = 0.0f; }, "radius is 0, outside above 0 to 64"},
        {[](KyubuLight& b) { b.radius = 65.0f; }, "radius is 65, outside above 0 to 64"},
        {[](KyubuLight& b) { b.color[1] = -0.1f; }, "color[1] is outside 0 to 16"},
        {[](KyubuLight& b) { b.color[2] = INFINITY; }, "color[2] is outside 0 to 16"},
        {[](KyubuLight& b) { b.fade_ms = 10; }, "fade_ms is 10, above life_ms 0"},
    };
    for (const Bad& b : bad) {
        KyubuLight x = l;
        b.change(x);
        CHECK(api->light_spawn(client, &x, &id) == KY_E_INVALID_ARG);
        CHECK_MESSAGE(game::last_refusal() == b.why, game::last_refusal());
    }
    {
        KyubuLight x = l;
        x.radius = NAN;
        CHECK(api->light_spawn(client, &x, &id) == KY_E_INVALID_ARG);
    }
    CHECK(api->light_spawn(client, nullptr, &id) == KY_E_INVALID_ARG);
    CHECK(api->light_spawn(client, &l, nullptr) == KY_E_INVALID_ARG);
    CHECK(game::light_count_for_test() == 0);

    // a good light is pushed into the vector the render passes, whole, with the sort key 0 (first)
    REQUIRE(api->light_spawn(client, &l, &id) == KY_OK);
    CHECK(id >= 1);
    std::vector<LightPush> pushes;
    g_light_pushes = &pushes;
    int vector = 0;
    game::push_lights(&vector, 10000);
    REQUIRE(pushes.size() == 1);
    CHECK(pushes[0].vector == &vector);
    const cube_Light r = pushes[0].light;
    CHECK(r.pos.x == 65536 * 10);
    CHECK(r.pos.y == -65536 * 3);
    CHECK(r.pos.z == 65536 * 2 + 5);
    CHECK(r.radius == 20.0f);
    CHECK(r.color[0] == 1.6f);
    CHECK(r.color[2] == 0.24f);
    CHECK(r.distance_sq == 0.0f);
    game::push_lights(&vector, 10020);                  // again every frame
    CHECK(pushes.size() == 2);

    // light_set moves it; another mod can neither set nor remove it
    KyubuLight moved = l;
    moved.pos[0] = 65536 * 11;
    CHECK(api->light_set(client2, id, &moved) == KY_E_NOT_FOUND);
    CHECK(api->light_remove(client2, id) == KY_E_NOT_FOUND);
    REQUIRE(api->light_set(client, id, &moved) == KY_OK);
    pushes.clear();
    game::push_lights(&vector, 10040);
    REQUIRE(pushes.size() == 1);
    CHECK(pushes[0].light.pos.x == 65536 * 11);
    CHECK(api->light_set(client, id + 100, &moved) == KY_E_NOT_FOUND);
    REQUIRE(api->light_remove(client, id) == KY_OK);
    CHECK(api->light_remove(client, id) == KY_E_NOT_FOUND);
    CHECK(game::light_count_for_test() == 0);

    // a life: full, then fading over the last fade_ms, then gone
    KyubuLight timed = l;
    timed.life_ms = 1000;
    timed.fade_ms = 500;
    REQUIRE(api->light_spawn(client, &timed, &id) == KY_OK);
    pushes.clear();
    game::push_lights(&vector, 10400);
    game::push_lights(&vector, 10750);
    game::push_lights(&vector, 11000);
    REQUIRE(pushes.size() == 2);
    CHECK(pushes[0].light.color[0] == doctest::Approx(1.6f));
    CHECK(pushes[1].light.color[0] == doctest::Approx(0.8f));      // 250 of 500 ms of fade left
    CHECK(game::light_count_for_test() == 0);

    // at most KY_LIGHT_MAX alive
    for (uint32_t i = 0; i < KY_LIGHT_MAX; ++i) REQUIRE(api->light_spawn(client, &l, &id) == KY_OK);
    CHECK(api->light_spawn(client2, &l, &id) == KY_E_REFUSED);
    CHECK_MESSAGE(game::last_refusal() == "16 lights are alive already", game::last_refusal());

    // the lights of a mod that is no longer loaded go at the next frame; a reload drops the mod's; leaving the world ends all
    game::lights_world_exit();
    REQUIRE(api->light_spawn(client, &l, &id) == KY_OK);
    REQUIRE(api->light_spawn(client2, &l, &id) == KY_OK);
    other.mod.state = ModState::Faulted;
    pushes.clear();
    game::push_lights(&vector, 12000);
    CHECK(pushes.size() == 1);
    CHECK(game::light_count_for_test() == 1);
    game::lights_drop_mod("game-test");
    CHECK(game::light_count_for_test() == 0);
    other.mod.state = ModState::Loaded;
    REQUIRE(api->light_spawn(client, &l, &id) == KY_OK);
    game::on_world_exit();
    CHECK(game::light_count_for_test() == 0);
    g_light_pushes = nullptr;

    game::set_light_clock_for_test(0);
    game::force_primitive_for_test(game::Prim::lights, -1);
}

TEST_CASE("game-api: creature_set_flags, and the hit sites obey unstoppable and invulnerable") {
    // covers: the add-dragon-boss task 3.2 unit tests (the in-game check is probe creature-flags)
    ClientGame g;
    TestMod m;
    const KyubuApi* api = api_table();
    const KyubuModHandle client = m.on(KY_SIDE_CLIENT), server = m.on(KY_SIDE_SERVER);
    game::on_world_enter();
    step(&g.gc->world);
    game::force_primitive_for_test(game::Prim::creature_flags, 1);
    game::creature_flags_world_exit();
    REQUIRE(api->creature_set_flags != nullptr);

    cube_Creature* boss = g.fake.creatures[1];   // the server side owns it; the client side owns the player (creatures[0])
    cube_Creature* other = g.fake.creatures[0];
    KyubuCreature h = 0;
    REQUIRE(api->creature_find(server, (int64_t)boss->id, &h) == KY_OK);

    // setting, reading back, clearing; a bit that is not defined is refused; the other side may not flag it
    CHECK(api->creature_set_flags(server, h, KY_CREATURE_UNSTOPPABLE | KY_CREATURE_INVULNERABLE) == KY_OK);
    KyubuCreatureInfo info = {};
    info.size = sizeof info;
    REQUIRE(api->creature_get(server, h, &info) == KY_OK);
    CHECK(info.flags == 3u);
    CHECK(api->creature_set_flags(server, h, 8) == KY_E_INVALID_ARG);
    CHECK(game::creature_flags_of(boss->id) == 3u);
    KyubuCreature hc = 0;
    REQUIRE(api->creature_find(client, (int64_t)boss->id, &hc) == KY_OK);
    CHECK(api->creature_set_flags(client, hc, 0) == KY_E_SIDE);
    CHECK(game::creature_flags_of(boss->id) == 3u);
    CHECK(api->creature_set_flags(server, h, 0) == KY_OK);
    CHECK(game::creature_flags_of(boss->id) == 0u);
    CHECK(api->creature_set_flags(server, h, KY_CREATURE_UNSTOPPABLE) == KY_OK);

    auto at = [](cube_Creature* c) { return (uint32_t)(uintptr_t)c; };

    // unstoppable: the push, the velocity reset, the slow and the stun are neutralised; a creature without the flag is not touched
    boss->data.extra_vel = {1.0f, 2.0f, 3.0f};
    other->data.extra_vel = {4.0f, 5.0f, 6.0f};
    KyubuCpuContext ctx = {};
    for (int i = 5; i < 8; ++i) ctx.xmm[i].f32[0] = 9.0f;
    ctx.esi = at(boss);
    game::flags_on_push_site(&ctx);
    CHECK(ctx.xmm[5].f32[0] == 1.0f);
    CHECK(ctx.xmm[6].f32[0] == 2.0f);
    CHECK(ctx.xmm[7].f32[0] == 3.0f);
    for (int i = 5; i < 8; ++i) ctx.xmm[i].f32[0] = 9.0f;
    ctx.esi = at(other);
    game::flags_on_push_site(&ctx);
    CHECK(ctx.xmm[5].f32[0] == 9.0f);

    boss->data.velocity = {0.5f, 0.6f, 0.7f};
    ctx.esi = at(boss);
    game::flags_on_velocity_site(&ctx);
    boss->data.velocity = {0.0f, 0.0f, 0.0f};   // what the game's reset does
    game::flags_on_velocity_end_site(&ctx);
    CHECK(boss->data.velocity.x == 0.5f);
    CHECK(boss->data.velocity.y == 0.6f);
    CHECK(boss->data.velocity.z == 0.7f);
    other->data.velocity = {0.0f, 0.0f, 0.0f};
    ctx.esi = at(other);
    game::flags_on_velocity_site(&ctx);
    game::flags_on_velocity_end_site(&ctx);
    CHECK(other->data.velocity.x == 0.0f);   // nothing was saved for it

    boss->data.slowed_time = 5;
    ctx.esi = at(boss);
    ctx.ecx = 3000;
    game::flags_on_slow_site(&ctx);
    CHECK(ctx.ecx == 5u);
    boss->data.stun_time = -7;
    ctx.ecx = 3000;
    game::flags_on_stun_site(&ctx);
    CHECK((int32_t)ctx.ecx == -7);
    boss->data.stun_time = 400;   // a stun from somewhere else is ended at the next hit
    ctx.ecx = 3000;
    game::flags_on_stun_site(&ctx);
    CHECK(ctx.ecx == 0u);
    other->data.stun_time = -1;
    ctx.esi = at(other);
    ctx.ecx = 3000;
    game::flags_on_stun_site(&ctx);
    CHECK(ctx.ecx == 3000u);

    // invulnerable: a hit on it does no damage at the list site and at the damage site; a heal and another creature's hit stay
    CHECK(api->creature_set_flags(server, h, KY_CREATURE_INVULNERABLE) == KY_OK);
    cube_Hit hit = {};
    hit.target_id = boss->id;
    hit.damage = 12.5f;
    ctx = {};
    ctx.esp = (uint32_t)(uintptr_t)&hit - 0xC4;
    game::flags_on_list_site(&ctx);
    CHECK(hit.damage == 0.0f);
    hit.damage = -8.0f;
    game::flags_on_list_site(&ctx);
    CHECK(hit.damage == -8.0f);
    hit.target_id = other->id;
    hit.damage = 12.5f;
    game::flags_on_list_site(&ctx);
    CHECK(hit.damage == 12.5f);
    ctx = {};
    ctx.ebx = (uint32_t)(uintptr_t)&hit;
    ctx.esi = at(other);
    game::flags_on_damage_site(&ctx);
    CHECK(hit.damage == 12.5f);
    ctx.esi = at(boss);
    game::flags_on_damage_site(&ctx);
    CHECK(hit.damage == 0.0f);
    // invulnerable alone does not stop the push
    boss->data.extra_vel = {1.0f, 2.0f, 3.0f};
    ctx.esi = at(boss);
    for (int i = 5; i < 8; ++i) ctx.xmm[i].f32[0] = 9.0f;
    game::flags_on_push_site(&ctx);
    CHECK(ctx.xmm[5].f32[0] == 9.0f);

    // the flags end with the creature and with the world
    game::on_creature_destroyed(boss);
    CHECK(game::creature_flags_of(boss->id) == 0u);
    game::creature_flags_set(other->id, 3);
    game::on_world_exit();
    CHECK(game::creature_flags_of(other->id) == 0u);
    game::force_primitive_for_test(game::Prim::creature_flags, -1);
}

TEST_CASE("game-api: KY_CREATURE_ROOTED holds a creature at the AI tick and the move step; creature_state_get and _set") {
    // covers: add-skills-unleashed-multiplayer 10.3 unit tests (the in-game check is probe creature-root-server)
    ClientGame g;
    TestMod m;
    const KyubuApi* api = api_table();
    const KyubuModHandle client = m.on(KY_SIDE_CLIENT), server = m.on(KY_SIDE_SERVER);
    game::on_world_enter();
    step(&g.gc->world);
    game::force_primitive_for_test(game::Prim::creature_root, 1);
    game::force_primitive_for_test(game::Prim::creature_state, 1);
    game::force_primitive_for_test(game::Prim::creature_flags, 0);   // as on Server.exe: only the root bit can be set
    game::creature_flags_world_exit();
    REQUIRE(api->creature_state_get != nullptr);
    REQUIRE(api->creature_state_set != nullptr);

    cube_Creature* mob = g.fake.creatures[1];
    cube_Creature* other = g.fake.creatures[0];
    KyubuCreature h = 0;
    REQUIRE(api->creature_find(server, (int64_t)mob->id, &h) == KY_OK);
    CHECK(api->creature_set_flags(server, h, KY_CREATURE_ROOTED) == KY_OK);
    CHECK(game::creature_flags_of(mob->id) == (uint32_t)KY_CREATURE_ROOTED);
    CHECK(api->creature_set_flags(server, h, KY_CREATURE_ROOTED | KY_CREATURE_UNSTOPPABLE) == KY_E_UNAVAILABLE);
    CHECK(api->creature_set_flags(server, h, 8) == KY_E_INVALID_ARG);
    KyubuCreatureInfo info = {};
    info.size = sizeof info;
    REQUIRE(api->creature_get(server, h, &info) == KY_OK);
    CHECK(info.flags == (uint32_t)KY_CREATURE_ROOTED);

    // the frame both sites read: the creature at [EBP-0x2B14], the step at EBP-0x1C
    alignas(16) static uint8_t stack[0x3000];
    uint8_t* ebp = stack + sizeof stack - 16;
    uint32_t* slot = (uint32_t*)(ebp - 0x2B14);
    float* stepv = (float*)(ebp - 0x1C);
    KyubuCpuContext ctx = {};
    ctx.ebp = (uint32_t)(uintptr_t)ebp;

    // the AI tick: a rooted creature's behaviour timer and mode go to 0; another's are untouched
    mob->behavior_time = 40;
    mob->data.current_mode = 21;
    mob->data.mode_start_time = 300;
    *slot = (uint32_t)(uintptr_t)mob;
    game::root_on_ai_site(&ctx);
    CHECK(mob->behavior_time == 0);
    CHECK(mob->data.current_mode == 0);
    CHECK(mob->data.mode_start_time == 0u);
    other->behavior_time = 40;
    other->data.current_mode = 21;
    *slot = (uint32_t)(uintptr_t)other;
    game::root_on_ai_site(&ctx);
    CHECK(other->behavior_time == 40);
    CHECK(other->data.current_mode == 21);

    // the move step: no x, y move and no rise; a fall stays; the push, the x, y velocity and the knockback are zeroed
    mob->data.velocity = {3.0f, 4.0f, -5.0f};
    mob->data.accel = {1.0f, 1.0f, 1.0f};
    mob->data.extra_vel = {6.0f, 7.0f, 8.0f};
    stepv[0] = 0.5f; stepv[1] = 0.25f; stepv[2] = -0.125f;
    *slot = (uint32_t)(uintptr_t)mob;
    game::root_on_move_site(&ctx);
    CHECK(stepv[0] == 0.0f);
    CHECK(stepv[1] == 0.0f);
    CHECK(stepv[2] == -0.125f);
    CHECK(mob->data.velocity.x == 0.0f);
    CHECK(mob->data.velocity.y == 0.0f);
    CHECK(mob->data.velocity.z == -5.0f);
    CHECK(mob->data.accel.x == 0.0f);
    CHECK(mob->data.extra_vel.x == 0.0f);
    CHECK(mob->data.extra_vel.z == 0.0f);
    stepv[2] = 0.5f;
    mob->data.velocity.z = 2.0f;
    game::root_on_move_site(&ctx);
    CHECK(stepv[2] == 0.0f);
    CHECK(mob->data.velocity.z == 0.0f);
    other->data.extra_vel = {6.0f, 7.0f, 8.0f};
    stepv[0] = 0.5f;
    *slot = (uint32_t)(uintptr_t)other;
    game::root_on_move_site(&ctx);
    CHECK(stepv[0] == 0.5f);
    CHECK(other->data.extra_vel.x == 6.0f);

    // cleared: the creature moves again
    CHECK(api->creature_set_flags(server, h, 0) == KY_OK);
    CHECK(game::creature_flags_of(mob->id) == 0u);
    *slot = (uint32_t)(uintptr_t)mob;
    mob->behavior_time = 40;
    game::root_on_ai_site(&ctx);
    CHECK(mob->behavior_time == 40);

    // creature_state_get reads every field; _set writes only what `fields` names, for a creature the side owns
    mob->data.pos = {65536 * 10, 65536 * 20, 65536 * 30};
    mob->data.velocity = {1.0f, 2.0f, 3.0f};
    mob->data.extra_vel = {4.0f, 5.0f, 6.0f};
    mob->data.current_mode = 7;
    mob->data.mode_start_time = 120;
    mob->data.stun_time = -3000;
    mob->data.slowed_time = 50;
    mob->data.charged_mp = 0.75f;
    mob->data.ray_hit = {7.0f, 8.0f, 9.0f};
    mob->data.appearance.scale = {0.8f, 0.9f, 1.8f};
    KyubuCreatureState s = {};
    s.size = sizeof s;
    REQUIRE(api->creature_state_get(server, h, &s) == KY_OK);
    CHECK(s.box[0] == 0.8f);
    CHECK(s.box[2] == 1.8f);
    {   // a 10.3 caller's struct ends at `aim`: it is read, `box` is left alone
        KyubuCreatureState old = {};
        old.size = (uint32_t)offsetof(KyubuCreatureState, box);
        old.box[2] = -1.0f;
        CHECK(api->creature_state_get(server, h, &old) == KY_OK);
        CHECK(old.aim[2] == 9.0f);
        CHECK(old.box[2] == -1.0f);
    }
    CHECK(s.fields == (uint32_t)KY_STATE_ALL);
    CHECK(s.pos[1] == 65536 * 20);
    CHECK(s.velocity[2] == 3.0f);
    CHECK(s.knock[0] == 4.0f);
    CHECK(s.mode == 7u);
    CHECK(s.mode_ms == 120);
    CHECK(s.stun_ms == -3000);
    CHECK(s.slowed_ms == 50);
    CHECK(s.charge == 0.75f);
    CHECK(s.aim[2] == 9.0f);
    KyubuCreatureState w = {};
    w.size = sizeof w;
    w.fields = KY_STATE_KNOCK | KY_STATE_STUN | KY_STATE_MODE;
    w.knock[0] = 1.5f;
    w.stun_ms = 400;
    w.mode = 0;
    w.velocity[0] = 99.0f;   // not named in fields: not written
    REQUIRE(api->creature_state_set(server, h, &w) == KY_OK);
    CHECK(mob->data.extra_vel.x == 1.5f);
    CHECK(mob->data.stun_time == 400);
    CHECK(mob->data.current_mode == 0);
    CHECK(mob->data.velocity.x == 1.0f);
    CHECK(mob->data.charged_mp == 0.75f);
    w.fields = 512;
    CHECK(api->creature_state_set(server, h, &w) == KY_E_INVALID_ARG);
    w.fields = KY_STATE_VELOCITY;
    w.velocity[0] = std::numeric_limits<float>::infinity();
    CHECK(api->creature_state_set(server, h, &w) == KY_E_INVALID_ARG);
    w.fields = KY_STATE_MODE;
    w.mode = 300;
    CHECK(api->creature_state_set(server, h, &w) == KY_E_INVALID_ARG);
    w.size = 8;
    CHECK(api->creature_state_set(server, h, &w) == KY_E_INVALID_ARG);
    CHECK(api->creature_state_get(server, h, nullptr) == KY_E_INVALID_ARG);
    KyubuCreature hc = 0;
    REQUIRE(api->creature_find(client, (int64_t)mob->id, &hc) == KY_OK);
    w = {};
    w.size = sizeof w;
    w.fields = KY_STATE_STUN;
    CHECK(api->creature_state_set(client, hc, &w) == KY_E_SIDE);
    CHECK(api->creature_state_get(client, hc, &s) == KY_OK);   // reading any creature is allowed

    game::creature_flags_world_exit();
    game::force_primitive_for_test(game::Prim::creature_flags, -1);
    game::force_primitive_for_test(game::Prim::creature_root, -1);
    game::force_primitive_for_test(game::Prim::creature_state, -1);
}

namespace {

// A World with one loaded zone, zone (zx, zy), whose columns are 4 blocks tall from base_z 10 except column (3, 5) of the zone,
// which runs from z -2 to z 1 with an air block at z 0 and an interior air block at z -1.
struct TerrainRig {
    cube_World* w = alloc_zeroed<cube_World>();
    cube_Region* region = alloc_zeroed<cube_Region>();
    cube_Zone* zone = alloc_zeroed<cube_Zone>();
    cube_Field* fields = (cube_Field*)VirtualAlloc(nullptr, sizeof(cube_Field) * 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    cube_Block tall[4] = {};
    cube_Block low[4] = {};
    static constexpr int32_t zx = 32800, zy = 32770;   // region (512, 512), slot (32, 2)
    TerrainRig() {
        w->regions[(zx / 64) * 1024 + zy / 64] = region;
        region->zones[(zx % 64) * 64 + zy % 64] = zone;
        zone->fields = fields;
        for (int i = 0; i < 4; ++i) {
            tall[i].color[0] = (uint8_t)(10 + i);
            tall[i].type = (uint8_t)(4 + i);
        }
        for (int i = 0; i < 65536; ++i) {
            fields[i].base_z = 10;
            fields[i].blocks = tall;
            fields[i].size = 4;
        }
        low[0].type = 1;      // z -2: solid
        low[1].type = 0x40;   // z -1: air inside a building
        low[2].type = 0;      // z 0: air
        low[3].type = 5;      // z 1: ground
        cube_Field& f = fields[5 * 256 + 3];
        f.base_z = -2;
        f.blocks = low;
    }
    ~TerrainRig() {
        VirtualFree(fields, 0, MEM_RELEASE);
        VirtualFree(zone, 0, MEM_RELEASE);
        VirtualFree(region, 0, MEM_RELEASE);
        VirtualFree(w, 0, MEM_RELEASE);
    }
    int32_t bx(int32_t col) const { return zx * 256 + col; }
    int32_t by(int32_t row) const { return zy * 256 + row; }
};

}  // namespace

TEST_CASE("game-api: world_block walks the World, region, zone and column") {
    // covers: server-ability-runtime/The server reads the world's blocks (the walk, a zone not loaded)
    TerrainRig t;
    KyubuBlock b = {};
    // inside a column: the stored block
    REQUIRE(game::terrain_block(t.w, t.bx(0), t.by(0), 12, &b) == KY_OK);
    CHECK(b.type == 6);
    CHECK(b.color[0] == 12);
    // under the column: the game's solid block; above it: air, and water at z 0 and below
    REQUIRE(game::terrain_block(t.w, t.bx(255), t.by(255), 9, &b) == KY_OK);
    CHECK(b.type == 1);
    CHECK(b.color[0] == 200);
    REQUIRE(game::terrain_block(t.w, t.bx(1), t.by(1), 14, &b) == KY_OK);
    CHECK(b.type == 0);
    CHECK(b.color[2] == 255);
    // the low column (3, 5): a stored air block at z 0 reads as water, an interior one stays air, z 1 is the stored ground
    REQUIRE(game::terrain_block(t.w, t.bx(3), t.by(5), 0, &b) == KY_OK);
    CHECK(b.type == 2);
    REQUIRE(game::terrain_block(t.w, t.bx(3), t.by(5), -1, &b) == KY_OK);
    CHECK(b.type == 0x40);
    REQUIRE(game::terrain_block(t.w, t.bx(3), t.by(5), 1, &b) == KY_OK);
    CHECK(b.type == 5);
    REQUIRE(game::terrain_block(t.w, t.bx(3), t.by(5), 2, &b) == KY_OK);
    CHECK(b.type == 0);   // above, at z 1 and up: air
    // x and y are not swapped: column (5, 3) is a tall one
    REQUIRE(game::terrain_block(t.w, t.bx(5), t.by(3), 1, &b) == KY_OK);
    CHECK(b.type == 1);   // under base_z 10
    // unknown, never air: the next zone in x and in y, another region, out of range
    CHECK(game::terrain_block(t.w, t.bx(256), t.by(0), 12, &b) == KY_E_NOT_FOUND);
    CHECK(game::terrain_block(t.w, t.bx(0), t.by(256), 12, &b) == KY_E_NOT_FOUND);
    CHECK(game::terrain_block(t.w, t.bx(0) - 64 * 256, t.by(0), 12, &b) == KY_E_NOT_FOUND);
    CHECK(game::terrain_block(t.w, -1, t.by(0), 12, &b) == KY_E_NOT_FOUND);
    CHECK(game::terrain_block(t.w, 0x1000000, t.by(0), 12, &b) == KY_E_NOT_FOUND);
    // a zone with no columns yet
    t.zone->fields = nullptr;
    CHECK(game::terrain_block(t.w, t.bx(0), t.by(0), 12, &b) == KY_E_NOT_FOUND);
}

TEST_CASE("game-api: world_block through the API on both sides") {
    // covers: server-ability-runtime/The server reads the world's blocks (either side, the guard)
    AbilityApiRig r;
    const KyubuApi* api = api_table();
    KyubuBlock b = {};
    std::string why;
    CHECK_MESSAGE(game::available(game::Prim::terrain, &why), why);
    CHECK(api->world_block(r.on_client(), 100, 100, 0, &b) == KY_E_NOT_FOUND);   // the fake World has no region
    CHECK(api->world_block(r.on_server(), 100, 100, 0, &b) == KY_E_NOT_FOUND);
    CHECK(api->world_block(r.on_client(), 100, 100, 0, nullptr) == KY_E_INVALID_ARG);
}

TEST_CASE("game-api: sim.world_step once per step, and creature_hit through hitCreature with the mod's stun and direction") {
    // covers: server-ability-runtime/A stun of the mod's choosing (the in-game check is probe world-step-hit-server)
    ClientGame g;
    TestMod m;
    const KyubuApi* api = api_table();
    const KyubuModHandle client = m.on(KY_SIDE_CLIENT), server = m.on(KY_SIDE_SERVER);
    game::on_world_enter();
    step(&g.gc->world);
    game::force_primitive_for_test(game::Prim::world_step, 1);
    REQUIRE(api->creature_hit != nullptr);
    cube_Creature* player = g.fake.creatures[0];
    cube_Creature* monster = g.fake.creatures[1];
    monster->data.hp = 50.0f;
    KyubuCreature attacker = 0, target = 0;
    REQUIRE(api->creature_find(server, (int64_t)player->id, &attacker) == KY_OK);
    REQUIRE(api->creature_find(server, (int64_t)monster->id, &target) == KY_OK);

    KyubuHit hit = {};
    hit.size = sizeof hit;
    hit.damage = 7.5f;
    hit.stun_ms = 300;
    hit.knock[0] = 1.0f;
    hit.knock[1] = -2.0f;
    hit.knock[2] = 3.0f;
    hit.flags = KY_HIT_MAGIC;

    // outside a step: refused, and hitCreature is not called
    std::vector<HitCall> calls;
    g_hit_calls = &calls;
    int landed = -1;
    CHECK(api->creature_hit(server, attacker, target, &hit, &landed) == KY_E_PHASE);
    CHECK(landed == 0);
    CHECK_MESSAGE(contains(game::last_refusal(), "not inside a World step"), game::last_refusal());
    CHECK(calls.empty());

    // the site's frame as cube::World::update lays it out: the World at [EBP-0x2B18], the output at [EBP-0x2B28], the zone set at
    // EBP-0x2BE8, dt at [EBP+8]
    static uint8_t frame[0x3000];
    const uintptr_t ebp = (uintptr_t)frame + 0x2C00;
    cube_StepOutput out = {};
    *(cube_World**)(ebp - 0x2B18) = &g.gc->world;
    *(void**)(ebp - 0x2B28) = &out;
    *(int32_t*)(ebp + 8) = 20;
    void* const zones = (void*)(ebp - 0x2BE8);

    std::vector<KyubuWorldStepData> seen_client, seen_server;
    std::vector<KyubuResult> results;
    std::vector<int> landings;
    const KyubuId sc = Events::get().subscribe_internal(
        "sim.world_step", KY_SIDE_CLIENT, 0, [&](KyubuEvent* ev) { seen_client.push_back(*(const KyubuWorldStepData*)ev->data); });
    const KyubuId ss = Events::get().subscribe_internal("sim.world_step", KY_SIDE_SERVER, 0, [&](KyubuEvent* ev) {
        seen_server.push_back(*(const KyubuWorldStepData*)ev->data);
        int l = -1;
        results.push_back(api->creature_hit(server, attacker, target, &hit, &l));
        landings.push_back(l);
    });
    static std::function<void()> inside;
    auto at_site = [ebp] {
        KyubuCpuContext ctx = {};
        ctx.ebp = (uint32_t)ebp;
        game::step_on_world_site(&ctx);
    };
    inside = at_site;
    g_inside_step = [] { inside(); };
    step(&g.gc->world);
    g_inside_step = nullptr;

    // single-player: one event per side, with the frame's pointers
    REQUIRE(seen_client.size() == 1);
    REQUIRE(seen_server.size() == 1);
    for (const KyubuWorldStepData* s : {&seen_client[0], &seen_server[0]}) {
        CHECK(s->world == &g.gc->world);
        CHECK(s->dt == 20);
        CHECK(s->step_out == &out);
        CHECK(s->effects == zones);
    }
    // the hit went through hitCreature with the step's pointers and chance 0, and the mod's stun and direction are in its Hit
    REQUIRE(results.size() == 1);
    CHECK(results[0] == KY_OK);
    CHECK(landings[0] == 1);
    REQUIRE(calls.size() == 1);
    const HitCall& c = calls[0];
    CHECK(c.world == &g.gc->world);
    CHECK(c.target == monster);
    CHECK(c.attacker == player);
    CHECK(c.damage == 7.5f);
    CHECK(c.chance == 0.0f);
    CHECK(c.critical == 0);
    CHECK(c.powered == 0);
    CHECK(c.magic == 1);
    CHECK(c.ability == 0);
    CHECK(c.projectile == nullptr);
    CHECK(c.allow_block == 1);
    CHECK(c.position == &player->data.pos);
    CHECK(c.direction.x == 1.0f);
    CHECK(c.direction.y == -2.0f);
    CHECK(c.direction.z == 3.0f);
    CHECK(c.out == &out);
    CHECK(c.zones == zones);
    CHECK(c.hit.stun_duration == 300);
    CHECK(c.hit.hit_dir.x == 1.0f);
    CHECK(c.hit.hit_dir.y == -2.0f);
    CHECK(c.hit.hit_dir.z == 3.0f);
    CHECK(c.hit.damage == 7.5f);

    // after the step, refused again
    CHECK(api->creature_hit(server, attacker, target, &hit, nullptr) == KY_E_PHASE);
    CHECK(calls.size() == 1);

    // the game's own hits pass the list site untouched
    {
        alignas(8) uint8_t stack[0xC4 + sizeof(cube_Hit)] = {};
        cube_Hit* h = (cube_Hit*)(stack + 0xC4);
        h->stun_duration = 900;
        h->hit_dir.z = 10.0f;
        KyubuCpuContext ctx = {};
        ctx.esp = (uint32_t)(uintptr_t)stack;
        game::step_on_list_site(&ctx);
        CHECK(h->stun_duration == 900);
        CHECK(h->hit_dir.z == 10.0f);
    }

    // inside a step: a bad hit is refused naming the reason; no stun, no knock and no attacker are fine; a dodged hit is false
    Events::get().unsubscribe(nullptr, ss);
    results.clear();
    landings.clear();
    calls.clear();
    std::vector<std::string> refusals;
    inside = [&] {
        at_site();
        struct Bad {
            void (*change)(KyubuHit&);
            const char* why;
        };
        const Bad bad[] = {
            {[](KyubuHit& b) { b.size = 8; }, "hit->size is 8, below 28"},
            {[](KyubuHit& b) { b.damage = NAN; }, "damage or knock is not a finite number"},
            {[](KyubuHit& b) { b.knock[2] = INFINITY; }, "damage or knock is not a finite number"},
            {[](KyubuHit& b) { b.stun_ms = -1; }, "stun_ms is -1, outside 0 to 60000"},
            {[](KyubuHit& b) { b.stun_ms = 60001; }, "stun_ms is 60001, outside 0 to 60000"},
            {[](KyubuHit& b) { b.flags = 2; }, "flags 0x2 has bits that are not defined"},
        };
        for (const Bad& b : bad) {
            KyubuHit x = hit;
            b.change(x);
            results.push_back(api->creature_hit(server, attacker, target, &x, nullptr));
            refusals.push_back(game::last_refusal());
        }
        results.push_back(api->creature_hit(server, attacker, target, nullptr, nullptr));
        KyubuHit plain = {};
        plain.size = sizeof plain;
        plain.damage = 2.0f;
        KyubuCreature tc = 0;
        api->creature_find(client, (int64_t)monster->id, &tc);
        g_hit_result = false;
        int l = -1;
        results.push_back(api->creature_hit(client, 0, tc, &plain, &l));
        landings.push_back(l);
        g_hit_result = true;
    };
    g_inside_step = [] { inside(); };
    step(&g.gc->world);
    g_inside_step = nullptr;
    REQUIRE(results.size() == 8);
    for (int i = 0; i < 7; ++i) CHECK(results[i] == KY_E_INVALID_ARG);
    CHECK(refusals == std::vector<std::string>{"hit->size is 8, below 28", "damage or knock is not a finite number",
                                               "damage or knock is not a finite number", "stun_ms is -1, outside 0 to 60000",
                                               "stun_ms is 60001, outside 0 to 60000", "flags 0x2 has bits that are not defined"});
    CHECK(results[7] == KY_OK);
    CHECK(landings == std::vector<int>{0});
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].attacker == nullptr);
    CHECK(calls[0].position == &monster->data.pos);   // no attacker: the target's own position
    CHECK(calls[0].magic == 0);
    CHECK(calls[0].hit.stun_duration == 0);           // the chance's stun is taken back: exactly the mod's 0
    CHECK(calls[0].hit.hit_dir.z == 0.0f);

    // a joined client: the event goes to the client side only
    seen_client.clear();
    seen_server.clear();
    const KyubuId ss2 = Events::get().subscribe_internal(
        "sim.world_step", KY_SIDE_SERVER, 0, [&](KyubuEvent* ev) { seen_server.push_back(*(const KyubuWorldStepData*)ev->data); });
    L().mode = KY_MODE_REMOTE_CLIENT;
    inside = at_site;
    g_inside_step = [] { inside(); };
    step(&g.gc->world);
    g_inside_step = nullptr;
    L().mode = KY_MODE_MENU_OR_SINGLEPLAYER;
    CHECK(seen_client.size() == 1);
    CHECK(seen_server.empty());

    Events::get().unsubscribe(nullptr, sc);
    Events::get().unsubscribe(nullptr, ss2);
    g_hit_calls = nullptr;
    inside = nullptr;
    game::remove_world_step_hooks();
    game::force_primitive_for_test(game::Prim::world_step, -1);
}

TEST_CASE("game-api: shot_take gives the projectile packets the receive site copied, and projectile_push puts one in the World") {
    // covers: the player_shots primitive (the in-game check is replay server-sniper-0720)
    ClientGame g;
    TestMod m;
    const KyubuApi* api = api_table();
    const KyubuModHandle server = m.on(KY_SIDE_SERVER);
    game::on_world_enter();
    step(&g.gc->world);
    game::force_primitive_for_test(game::Prim::world_step, 1);
    game::force_primitive_for_test(game::Prim::player_shots, 1);
    game::reset_shots_for_test();
    REQUIRE(api->shot_take != nullptr);
    REQUIRE(api->projectile_push != nullptr);

    // the receive site's frame: the projectile at EBP-0x110; EDI the closure, whose connection vector holds the player's connection
    static uint8_t frame[0x200];
    const uintptr_t ebp = (uintptr_t)frame + 0x180;
    cube_Projectile* sent = (cube_Projectile*)(ebp - 0x110);
    memset(sent, 0, sizeof *sent);
    sent->entity_id = 7;
    sent->velocity.x = 100.0f;
    alignas(8) static uint8_t conn[sizeof(cube_Connection)] = {};
    *(int64_t*)(conn + offsetof(cube_Connection, creature_id)) = 7;
    void* conns[2] = {nullptr, conn};
    void* vec[3] = {&conns[0], &conns[2], &conns[2]};
    uint8_t closure[0x24] = {};
    *(void**)(closure + 4) = vec;
    *(uint32_t*)(closure + 8) = 1;
    KyubuCpuContext ctx = {};
    ctx.ebp = (uint32_t)ebp;
    ctx.edi = (uint32_t)(uintptr_t)closure;

    // nothing is copied before a mod's first take
    game::shots_on_receive_site(&ctx);
    KyubuShot out[4];
    for (auto& o : out) o.size = sizeof o;
    uint32_t n = 99;
    CHECK(api->shot_take(server, out, 4, &n) == KY_OK);
    CHECK(n == 0);
    // then each packet, with its connection (creature id + 1), byte for byte, once
    game::shots_on_receive_site(&ctx);
    sent->entity_id = 8;
    game::shots_on_receive_site(&ctx);
    CHECK(sent->entity_id == 8);   // the game's packet is left as it was
    CHECK(api->shot_take(server, out, 1, &n) == KY_OK);
    REQUIRE(n == 1);
    CHECK(out[0].connection == 8);
    CHECK(((cube_Projectile*)out[0].data)->entity_id == 7);
    CHECK(((cube_Projectile*)out[0].data)->velocity.x == 100.0f);
    CHECK(api->shot_take(server, out, 4, &n) == KY_OK);
    REQUIRE(n == 1);
    CHECK(((cube_Projectile*)out[0].data)->entity_id == 8);
    CHECK(api->shot_take(server, out, 4, &n) == KY_OK);
    CHECK(n == 0);
    // refusals
    CHECK(api->shot_take(server, out, 4, nullptr) == KY_E_INVALID_ARG);
    CHECK(api->shot_take(server, nullptr, 4, &n) == KY_E_INVALID_ARG);
    out[1].size = 8;
    CHECK(api->shot_take(server, out, 4, &n) == KY_E_INVALID_ARG);
    CHECK_MESSAGE(contains(game::last_refusal(), "out[1].size is 8"), game::last_refusal());
    out[1].size = sizeof out[1];
    // the queue keeps the newest 256
    for (int i = 0; i < 300; ++i) {
        sent->entity_id = i;
        game::shots_on_receive_site(&ctx);
    }
    std::vector<int64_t> ids;
    for (;;) {
        CHECK(api->shot_take(server, out, 4, &n) == KY_OK);
        for (uint32_t i = 0; i < n; ++i) ids.push_back(((cube_Projectile*)out[i].data)->entity_id);
        if (n < 4) break;
    }
    REQUIRE(ids.size() == 256);
    CHECK(ids.front() == 44);
    CHECK(ids.back() == 299);

    // projectile_push: outside a step refused; inside, one copy onto World+0x14 with the game's push
    std::vector<ProjectilePush> pushes;
    g_projectile_pushes = &pushes;
    uint8_t data[KY_SHOT_DATA_SIZE] = {};
    ((cube_Projectile*)data)->entity_id = 7;
    ((cube_Projectile*)data)->kind = 1;
    CHECK(api->projectile_push(server, data, sizeof data) == KY_E_PHASE);
    CHECK(api->projectile_push(server, data, 0x6F) == KY_E_INVALID_ARG);
    CHECK(api->projectile_push(server, nullptr, sizeof data) == KY_E_INVALID_ARG);
    static uint8_t sframe[0x3000];
    const uintptr_t sebp = (uintptr_t)sframe + 0x2C00;
    cube_StepOutput sout = {};
    *(cube_World**)(sebp - 0x2B18) = &g.gc->world;
    *(void**)(sebp - 0x2B28) = &sout;
    *(int32_t*)(sebp + 8) = 20;
    static std::function<void()> inside;
    std::vector<KyubuResult> results;
    inside = [&] {
        KyubuCpuContext c = {};
        c.ebp = (uint32_t)sebp;
        game::step_on_world_site(&c);
        results.push_back(api->projectile_push(server, data, sizeof data));
    };
    g_inside_step = [] { inside(); };
    step(&g.gc->world);
    g_inside_step = nullptr;
    REQUIRE(results.size() == 1);
    CHECK(results[0] == KY_OK);
    REQUIRE(pushes.size() == 1);
    CHECK(pushes[0].list == (char*)&g.gc->world + 0x14);
    CHECK(pushes[0].p.entity_id == 7);
    CHECK(pushes[0].p.kind == 1);

    g_projectile_pushes = nullptr;
    inside = nullptr;
    game::remove_world_step_hooks();
    game::reset_shots_for_test();
    game::force_primitive_for_test(game::Prim::player_shots, -1);
    game::force_primitive_for_test(game::Prim::world_step, -1);
}

namespace {
int g_applied = 0;
void __fastcall fake_apply_hit(cube_World*, void*, const cube_Hit*, void*, void*) { ++g_applied; }
}  // namespace

TEST_CASE("game-api: server_hits drops applyHit's second push of hitCreature's hit on a player, and passes every other call") {
    // covers: the server_hits primitive (the in-game check is replay server-guardian-0720)
    cube_World* w = alloc_zeroed<cube_World>();
    FakeWorld fake;
    fake.init(w);
    w->local_player = nullptr;                      // Server.exe's World
    fake.creatures[0]->data.hostile_type = 0;       // creature 1, a player
    fake.creatures[1]->data.hostile_type = 2;       // creature 2, a monster
    game::remove_server_hit_hooks();
    game::set_server_hits_next_for_test(fake_apply_hit);
    g_applied = 0;

    // hitCreature's Hit sits at ESP+0xC4 at the list site
    static uint8_t frame[0x200];
    KyubuCpuContext ctx = {};
    ctx.esp = (uint32_t)(uintptr_t)frame;
    cube_Hit* hit = (cube_Hit*)(frame + 0xC4);
    memset(hit, 0, sizeof *hit);
    hit->entity_id = 2;

    // a hit on the player: listed, then the applyHit call on the same Hit is dropped
    hit->target_id = 1;
    game::server_hits_on_list_site(&ctx);
    game::server_hits_apply_detour(w, nullptr, hit, nullptr, nullptr);
    CHECK(g_applied == 0);
    CHECK(game::server_hits_dropped() == 1);
    // the same Hit again without the list site (a later call): passed on
    game::server_hits_apply_detour(w, nullptr, hit, nullptr, nullptr);
    CHECK(g_applied == 1);

    // a hit on the monster: listed and applied
    hit->entity_id = 1;
    hit->target_id = 2;
    game::server_hits_on_list_site(&ctx);
    game::server_hits_apply_detour(w, nullptr, hit, nullptr, nullptr);
    CHECK(g_applied == 2);

    // a client's hit on the player, from the step's received list (another address): passed on
    hit->target_id = 1;
    game::server_hits_on_list_site(&ctx);
    cube_Hit received = *hit;
    game::server_hits_apply_detour(w, nullptr, &received, nullptr, nullptr);
    CHECK(g_applied == 3);
    // an unknown target: passed on, the game's applyHit returns by itself
    hit->target_id = 99;
    game::server_hits_on_list_site(&ctx);
    game::server_hits_apply_detour(w, nullptr, hit, nullptr, nullptr);
    CHECK(g_applied == 4);
    CHECK(game::server_hits_dropped() == 1);

    game::remove_server_hit_hooks();
    DeleteCriticalSection((CRITICAL_SECTION*)&w->lock);
    VirtualFree(w, 0, MEM_RELEASE);
}
