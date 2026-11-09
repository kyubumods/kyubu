// kyubu_probe (mods-dev/kyubu_probe): its tools run here in-process, against the real API table, the way the mod
// runs them in the game (add-mapping-workspace tasks 5.1 to 5.3).
#include "doctest.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "api.h"
#include "console.h"
#include "cubeworld/layout.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "lua.hpp"
#include "modmanager.h"
#include "probe.h"
#include "channels.h"
#include "util.h"

using namespace kyubu;

namespace {

// The probe as the loader would load it. Static: the loader's tables keep pointers to it.
Mod& probe_mod() {
    static Mod m;
    if (m.id.empty()) {
        m.id = "kyubu_probe";
        m.state = ModState::Loaded;
        m.manifest.sides = KY_SIDE_BOTH;
        for (int s = 1; s <= 2; ++s) { m.ctx[s].mod = &m; m.ctx[s].side = (KyubuSide)s; }
    }
    return m;
}

std::vector<std::string> console(const std::string& line) {
    static bool builtins = (register_builtin_commands(), true);   // hooks, help... as at boot
    (void)builtins;
    std::vector<std::string> out;
    Console::get().execute(line, [&](const std::string& s) { out.push_back(s); });
    return out;
}

bool log_has(const std::string& text, size_t n = 200) {
    for (auto& l : Log::get().tail(n))
        if (l.text.find(text) != std::string::npos) return true;
    return false;
}

// A fixture object with an unmapped pointer (live-probes, "Bad pointer").
struct Node {
    uint32_t id;
    float hp;
    Node* next;
    uint8_t flags[4];
};
const CwLayoutField kNodeFields[] = {
    {"id", "uint32_t", offsetof(Node, id), 4, 1, CW_KIND_UINT, 1, nullptr},
    {"hp", "float", offsetof(Node, hp), 4, 1, CW_KIND_FLOAT, 0, nullptr},
    {"next", "test::Node*", offsetof(Node, next), 4, 1, CW_KIND_POINTER, 0, "test_Node"},
};
const CwLayoutType kNodeType[] = {{"test::Node", "test_Node", sizeof(Node), 0, 3, kNodeFields}};

// A fixture World: a VC110 std::map (std::map and std::tree_node in mapping/types/vc110.toml) from a uint64 id to
// a creature, whose objects carry MSVC RTTI (vtable at +0, complete object locator at slot -1).
struct MapNode {
    MapNode* left;
    MapNode* parent;
    MapNode* right;
    char color;
    char isnil;
    uint64_t key;       // node+0x10
    uint32_t value;     // node+0x18
};
static_assert(offsetof(MapNode, key) == 0x10 && offsetof(MapNode, value) == 0x18, "the VC110 map<uint64, T*> node");

struct FakeTypeDescriptor { uint32_t vtable, spare; char name[24]; };
struct FakeLocator { uint32_t signature, offset, cd_offset, type_descriptor; };

struct CreatureWorld {
    FakeTypeDescriptor td{0, 0, ".?AVCreature@cube@@"};
    FakeLocator locator{0, 0, 0, 0};
    uint32_t vtable[2] = {};                            // [locator, slot 0]
    std::vector<std::vector<uint8_t>> creatures;
    MapNode head{}, nodes[5]{};
    std::vector<uint8_t> controller = std::vector<uint8_t>(sizeof(cube_GameController));

    CreatureWorld() {
        locator.type_descriptor = (uint32_t)(uintptr_t)&td;
        vtable[0] = (uint32_t)(uintptr_t)&locator;
        const uint64_t keys[5] = {1, 2, 3, 7, 0x100000005ULL};
        for (int i = 0; i < 5; ++i) {
            creatures.emplace_back(sizeof(cube_Creature));
            *(uint32_t*)creatures[i].data() = (uint32_t)(uintptr_t)&vtable[1];
            nodes[i].key = keys[i];
            nodes[i].value = (uint32_t)(uintptr_t)creatures[i].data();
            nodes[i].left = nodes[i].right = &head;
        }
        // 3 at the root, 2 (with 1) on its left, 0x100000005 (with 7) on its right
        MapNode *n1 = &nodes[0], *n2 = &nodes[1], *n3 = &nodes[2], *n7 = &nodes[3], *n5 = &nodes[4];
        n3->parent = &head; n3->left = n2; n3->right = n5;
        n2->parent = n3; n2->left = n1;
        n1->parent = n2;
        n5->parent = n3; n5->left = n7;
        n7->parent = n5;
        head.isnil = 1; head.parent = n3; head.left = n1; head.right = n5;
        gc()->world.creatures.head = (std_tree_node*)&head;
        gc()->world.creatures.size = 5;
        gc()->player = (cube_Creature*)creatures[0].data();
    }
    cube_GameController* gc() { return (cube_GameController*)controller.data(); }
    uintptr_t map() { return (uintptr_t)&gc()->world.creatures; }
};

}  // namespace

TEST_CASE("live-probes: dev mode off") {
    // covers: live-probes/Dev mode off
    L().host.host = KY_HOST_CLIENT;
    L().dev_mode = false;
    Mod& m = probe_mod();
    const KyubuApi* api = api_table();
    CHECK(kprobe::Probe::init(api, m.handle(KY_SIDE_CLIENT), KY_SIDE_CLIENT) == 0);
    CHECK(kprobe::Probe::init(api, m.handle(KY_SIDE_SERVER), KY_SIDE_SERVER) == 0);
    CHECK(kprobe::Probe::get() == nullptr);
    CHECK(log_has("inactive: dev mode is off"));
    CHECK(Hooks::get().list("kyubu_probe").empty());
    CHECK(console("hooks kyubu_probe").empty());                                   // the scenario's own check
    CHECK(console("kyubu_probe:help") == std::vector<std::string>{"unknown command 'kyubu_probe:help' (try help)"});
    void* iface = nullptr;
    CHECK(api->get_interface(KYUBU_PROBE_FINDER, 1, &iface, nullptr) == KY_E_NOT_FOUND);
    kprobe::Probe::shutdown(KY_SIDE_CLIENT);
    kprobe::Probe::shutdown(KY_SIDE_SERVER);
}

TEST_CASE("live-probes: address expressions") {
    const KyubuApi* api = api_table();
    uint32_t target = 0x11223344;
    uint32_t cell = (uint32_t)(uintptr_t)&target;
    std::string err;
    uintptr_t a = 0;
    CHECK(kprobe::eval_address(api, kprobe::strf("0x%X", (unsigned)(uintptr_t)&cell), &a, &err));
    CHECK(a == (uintptr_t)&cell);
    CHECK(kprobe::eval_address(api, kprobe::strf("[0x%X] + 4 - 0x2", (unsigned)(uintptr_t)&cell), &a, &err));
    CHECK(a == (uintptr_t)&target + 2);
    CHECK_FALSE(kprobe::eval_address(api, "[0x10]", &a, &err));                   // an unmapped pointer
    CHECK(err == "cannot read the pointer at 0x00000010");
    CHECK_FALSE(kprobe::eval_address(api, "no::such::symbol", &a, &err));
    CHECK(err.find("no::such::symbol") != std::string::npos);
    CHECK_FALSE(kprobe::eval_address(api, "@0x1000", &a, &err));                  // below the image base

    // ecx(<label>): the this of a traced function's latest call, supplied by the probe's tracer
    kprobe::EcxLookup ecx = [&](const std::string& label, uintptr_t* out, std::string* why) {
        if (label != "@0x427FF0") { *why = label + " has no recorded calls"; return false; }
        *out = (uintptr_t)&cell - 4;
        return true;
    };
    CHECK(kprobe::eval_address(api, "[ecx(@0x427FF0) + 4]", &a, &err, &ecx));
    CHECK(a == (uintptr_t)&target);
    CHECK_FALSE(kprobe::eval_address(api, "ecx(other)", &a, &err, &ecx));
    CHECK(err == "other has no recorded calls");
    CHECK_FALSE(kprobe::eval_address(api, "ecx(@0x427FF0)", &a, &err));           // no tracer to ask
}

TEST_CASE("live-probes: the field finder on a region that changes in a known way") {
    // A synthetic creature: hp decreases when "hit", a timer increases every step, the rest stays the same.
    const KyubuApi* api = api_table();
    auto page = (uint8_t*)VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(page);
    float* f = (float*)page;
    for (int i = 0; i < 64; ++i) f[i] = 1.5f;
    f[4] = 0;          // +0x10 timer
    f[16] = 100;       // +0x40 hp
    f[20] = 7;         // +0x50 unrelated, changes up and down
    kprobe::Finder finder(api);
    std::string err;
    int id = finder.create((uintptr_t)page, 256, kprobe::VType::F32, 0, &err);
    REQUIRE(id > 0);
    CHECK(finder.filter(id, kprobe::Op::Decreased, 0, &err) == -1);             // needs a snapshot first
    CHECK(finder.snapshot(id, &err) == 64);
    CHECK(finder.filter(id, kprobe::Op::Decreased, 0, &err) == -1);             // needs two
    CHECK(err.find("compares two snapshots") != std::string::npos);

    f[4] += 1; f[16] = 80; f[20] = 9;                                           // hit: hp 100 -> 80
    CHECK(finder.snapshot(id, &err) == 64);
    CHECK(finder.filter(id, kprobe::Op::Decreased, 0, &err) == 1);
    f[4] += 1; f[16] = 65; f[20] = 5;                                           // hit again: 80 -> 65
    CHECK(finder.snapshot(id, &err) == 1);
    CHECK(finder.filter(id, kprobe::Op::Decreased, 0, &err) == 1);
    auto hits = finder.results(id, 10, &err);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].offset == 0x40);
    CHECK(hits[0].value == 65);
    CHECK(hits[0].previous == 80);
    CHECK(hits[0].changes == 2);
    CHECK(finder.filter(id, kprobe::Op::Equal, 65, &err) == 1);
    CHECK(finder.filter(id, kprobe::Op::Delta, -15, &err) == 1);
    CHECK(finder.filter(id, kprobe::Op::Delta, -20, &err) == 0);

    // Ranking and the other filters on a fresh search: changed offsets first, most often changed first.
    int s2 = finder.create((uintptr_t)page, 256, kprobe::VType::F32, 0, &err);
    finder.snapshot(s2, &err);
    f[4] += 1; f[20] = 6;
    finder.snapshot(s2, &err);
    f[4] += 1;
    finder.snapshot(s2, &err);
    CHECK(finder.filter(s2, kprobe::Op::Unchanged, 0, &err) == 63);            // only the timer changed last time
    int s3 = finder.create((uintptr_t)page, 256, kprobe::VType::F32, 0, &err);
    finder.snapshot(s3, &err);
    f[4] += 1; f[20] = 8;
    finder.snapshot(s3, &err);
    f[4] += 1;
    finder.snapshot(s3, &err);
    auto ranked = finder.results(s3, 3, &err);
    REQUIRE(ranked.size() == 3);
    CHECK(ranked[0].offset == 0x10);    // changed twice
    CHECK(ranked[1].offset == 0x50);    // once
    CHECK(ranked[0].changes == 2);
    CHECK(finder.filter(s3, kprobe::Op::Increased, 0, &err) == 1);

    // A range over an unreadable page keeps only the readable candidates, and never faults.
    DWORD old;
    REQUIRE(VirtualProtect(page + 4096, 4096, PAGE_NOACCESS, &old));
    int s4 = finder.create((uintptr_t)page + 4096 - 16, 64, kprobe::VType::U32, 0, &err);
    CHECK(finder.snapshot(s4, &err) == 4);
    int s5 = finder.create((uintptr_t)page + 4096, 64, kprobe::VType::U32, 0, &err);
    CHECK(finder.snapshot(s5, &err) == -1);
    CHECK(err.find("not readable") != std::string::npos);
    CHECK(finder.create((uintptr_t)page, kprobe::kMaxRange + 1, kprobe::VType::U8, 0, &err) == 0);
    VirtualFree(page, 0, MEM_RELEASE);
}

TEST_CASE("live-probes: the field finder from Lua") {
    // The published interface, driven by mods-dev/kyubu_probe/lua/kyubu_probe.lua in the loader's LuaJIT.
    const KyubuApi* api = api_table();
    kprobe::Finder finder(api);
    const KyubuProbeFinder* iface = kprobe::finder_interface(&finder);
    CHECK(iface->size == sizeof(KyubuProbeFinder));
    CHECK(sizeof(KyubuProbeHit) == 32);
    std::vector<int32_t> region(64, 5);
    region[9] = 1000;                   // +0x24: "mana", spent twice below

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    REQUIRE(luaL_loadfile(L, KYUBU_SOURCE_DIR "/mods-dev/kyubu_probe/lua/kyubu_probe.lua") == 0);
    REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
    lua_setglobal(L, "make_probe");
    lua_pushlightuserdata(L, (void*)iface);
    lua_setglobal(L, "iface");
    lua_pushinteger(L, (lua_Integer)(uintptr_t)region.data());
    lua_setglobal(L, "region");
    const char* code = R"(
        local ffi = require("ffi")
        local kyubu = { interface = function(name, v) assert(name == "kyubu_probe:finder" and v == 1) return iface, 1 end }
        local probe = make_probe(kyubu)
        local mem = ffi.cast("int32_t*", region)
        local s = assert(probe.search(("0x%X"):format(region), 256, "i32"))
        assert(s:snapshot() == 64)
        mem[9] = mem[9] - 30
        mem[3] = mem[3] + 1
        s:snapshot()
        assert(s:filter("decreased") == 1)
        mem[9] = mem[9] - 30
        s:snapshot()
        assert(s:filter("delta", -30) == 1)
        local r = s:results(5)
        local bad, why = probe.search("[0x10]", 4, "i32")
        local bad2, why2 = s:filter("sideways")
        s:destroy()
        return r[1].offset, r[1].value, r[1].changes, #r, tostring(bad) .. " " .. why, why2
    )";
    int rc = luaL_loadstring(L, code) || lua_pcall(L, 0, 6, 0);
    REQUIRE_MESSAGE(rc == 0, lua_tostring(L, -1));
    CHECK(lua_tointeger(L, -6) == 0x24);
    CHECK(lua_tonumber(L, -5) == 940);
    CHECK(lua_tointeger(L, -4) == 2);
    CHECK(lua_tointeger(L, -3) == 1);
    CHECK(std::string(lua_tostring(L, -2)) == "nil cannot read the pointer at 0x00000010 (-1)");
    CHECK(std::string(lua_tostring(L, -1)) == "unknown filter sideways (-1)");
    lua_close(L);
    CHECK(finder.ids().empty());
}

TEST_CASE("live-probes: bad pointer") {
    // covers: live-probes/Bad pointer
    const KyubuApi* api = api_table();
    kprobe::Layout layout;
    layout.types = kNodeType;
    layout.type_count = 1;
    kprobe::Inspector insp(api, layout);
    Node node{42, 12.5f, (Node*)(uintptr_t)0x10, {1, 2, 3, 4}};   // next: an address that is not mapped
    uint64_t faults_before = 0;
    for (auto& l : Log::get().tail(500)) faults_before += l.text.find("FAULTED") != std::string::npos;

    auto rows = insp.rows((uintptr_t)&node, &kNodeType[0]);
    REQUIRE(rows.size() == 5);                                  // the object, 3 fields, the 4 bytes no field covers
    CHECK(rows[0].readable);
    CHECK(rows[1].name == "id");
    CHECK(rows[1].value == "42 (0x2A)");
    CHECK(rows[1].status == "verified");
    CHECK(rows[2].value == "12.5");
    CHECK(rows[2].status == "candidate");
    CHECK(rows[3].name == "next");
    CHECK(rows[3].value == "0x00000010 (unreadable)");
    CHECK(rows[3].follow == std::string("test_Node"));
    CHECK(rows[4].name == "_unk_c");
    CHECK(rows[4].value == "01 02 03 04");
    CHECK(rows[4].status == "");

    auto target = insp.rows(rows[3].target, &kNodeType[0]);    // following the pointer
    REQUIRE(target.size() == 1);
    CHECK_FALSE(target[0].readable);
    CHECK(target[0].value == "unreadable: 0x00000010 (0x10 bytes) is not mapped");

    uint64_t faults_after = 0;
    for (auto& l : Log::get().tail(500)) faults_after += l.text.find("FAULTED") != std::string::npos;
    CHECK(faults_after == faults_before);                       // no fault logged
}

TEST_CASE("live-probes: the inspector with the generated layout, and editing") {
    const KyubuApi* api = api_table();
    kprobe::Inspector insp(api, kprobe::Layout::generated());
    const CwLayoutType* t = insp.layout().find("cube::CreatureData");
    REQUIRE(t);
    CHECK(insp.layout().find("cube_CreatureData") == t);
    std::vector<uint8_t> obj(t->size);
    cube_CreatureData* c = (cube_CreatureData*)obj.data();
    c->hp = 250;
    c->pos.x = 3;
    std::memcpy(c->name, "Tester", 7);
    auto rows = insp.rows((uintptr_t)obj.data(), t);
    auto row = [&](const std::string& name) -> const kprobe::Row* {
        for (auto& r : rows) if (r.name == name) return &r;
        return nullptr;
    };
    REQUIRE(row("hp"));
    CHECK(row("hp")->value == "250");
    CHECK(row("hp")->offset == offsetof(cube_CreatureData, hp));
    CHECK(row("hp")->status == "verified");                      // probe:creature-fields (8.2)
    CHECK(row("hp")->editable);
    REQUIRE(row("pos.x"));
    CHECK(row("pos.x")->value == "3");
    CHECK(row("name")->value == "\"Tester\"");
    REQUIRE(row("equipment[0].level"));
    CHECK(row("equipment[4..12]"));                              // arrays show their first elements
    CHECK(insp.field_at(t, offsetof(cube_CreatureData, hp)) == "hp");
    CHECK(insp.field_at(t, offsetof(cube_CreatureData, pos) + 8) == "pos.y");
    CHECK(insp.field_at(t, offsetof(cube_CreatureData, equipment) + sizeof(cube_ItemData) + 0x10) ==
          "equipment[1].level");

    Mod& m = probe_mod();
    std::string err;
    CHECK(insp.edit(m.handle(KY_SIDE_CLIENT), *row("hp"), "123.5", &err));
    CHECK(c->hp == 123.5f);
    CHECK_FALSE(insp.edit(m.handle(KY_SIDE_CLIENT), *row("hp"), "lots", &err));
    CHECK(err == "not a number: lots");
    CHECK_FALSE(insp.edit(m.handle(KY_SIDE_CLIENT), *row("name"), "x", &err));   // a string is not a scalar

    // inspect set <field> from <address> [plus <n>]: one creature's position next to another's
    std::vector<uint8_t> other(t->size);
    cube_CreatureData* o = (cube_CreatureData*)other.data();
    o->pos.x = 0x123456789ALL;
    o->hp = 80;
    CHECK(insp.copy(m.handle(KY_SIDE_CLIENT), *row("pos.x"), (uintptr_t)&o->pos.x, 0x8000, &err));
    CHECK(c->pos.x == 0x123456789ALL + 0x8000);
    CHECK(insp.copy(m.handle(KY_SIDE_CLIENT), *row("hp"), (uintptr_t)&o->hp, 0, &err));
    CHECK(c->hp == 80.0f);
    CHECK(insp.copy(m.handle(KY_SIDE_CLIENT), *row("hp"), (uintptr_t)&o->hp, -0.5, &err));
    CHECK(c->hp == 79.5f);
    CHECK_FALSE(insp.copy(m.handle(KY_SIDE_CLIENT), *row("pos.x"), (uintptr_t)&o->pos.x, 0.5, &err));
    CHECK(err == "the delta is not a whole number");
    CHECK_FALSE(insp.copy(m.handle(KY_SIDE_CLIENT), *row("hp"), 0x10, 0, &err));
    CHECK(err.find("cannot read") == 0);
}

TEST_CASE("live-probes: walking the creature map") {
    const KyubuApi* api = api_table();
    kprobe::Inspector insp(api, kprobe::Layout::generated());
    CreatureWorld w;
    std::vector<kprobe::MapEntry> list;
    std::string err;
    REQUIRE(insp.map_entries(w.map(), 8, 0x18, &list, &err));
    REQUIRE(list.size() == 5);                                  // in key order, the 64-bit key last
    const uint64_t keys[5] = {1, 2, 3, 7, 0x100000005ULL};
    for (int i = 0; i < 5; ++i) CHECK(list[i].key == keys[i]);
    CHECK(list[0].value == (uint32_t)(uintptr_t)w.creatures[0].data());
    CHECK(list[4].node == (uintptr_t)&w.nodes[4]);
    CHECK(insp.rtti_name(list[2].value) == ".?AVCreature@cube@@");
    CHECK(insp.rtti_name((uintptr_t)&w.head).empty());          // no vtable there

    w.gc()->world.creatures.size = 4;                           // the game changed the map during the walk
    CHECK_FALSE(insp.map_entries(w.map(), 8, 0x18, &list, &err));
    CHECK(err == "more nodes than the map's size 4 (it changed during the walk?)");
    w.gc()->world.creatures.size = 6;
    CHECK_FALSE(insp.map_entries(w.map(), 8, 0x18, &list, &err));
    CHECK(err == "5 node(s), but the map's size is 6 (it changed during the walk?)");
    w.gc()->world.creatures.size = 5;
    w.nodes[1].right = (MapNode*)(uintptr_t)0x10;               // a broken link: reported, no fault
    CHECK_FALSE(insp.map_entries(w.map(), 8, 0x18, &list, &err));
    CHECK(err == "cannot read the node at 0x00000010");
    CHECK(list.size() == 2);                                    // what it read before
    w.nodes[1].right = &w.head;
    w.gc()->world.creatures.head = (std_tree_node*)&w.nodes[2];  // not a head node
    CHECK_FALSE(insp.map_entries(w.map(), 8, 0x18, &list, &err));
    CHECK(err.find("is not a head node") != std::string::npos);

    MapNode empty{};                                            // an empty map: the head points at itself
    empty.left = empty.parent = empty.right = &empty;
    empty.isnil = 1;
    w.gc()->world.creatures.head = (std_tree_node*)&empty;
    w.gc()->world.creatures.size = 0;
    CHECK(insp.map_entries(w.map(), 8, 0x18, &list, &err));
    CHECK(list.empty());
}

TEST_CASE("live-probes: the local-player and creature-list views") {
    L().host.host = KY_HOST_CLIENT;
    L().dev_mode = true;
    Mod& m = probe_mod();
    const KyubuApi* api = api_table();
    REQUIRE(kprobe::Probe::init(api, m.handle(KY_SIDE_CLIENT), KY_SIDE_CLIENT) == 0);
    kprobe::Probe* p = kprobe::Probe::get();
    REQUIRE(p);
    CreatureWorld w;
    auto run = [&](const std::string& line) {
        std::vector<std::string> out;
        p->run(line, KY_SIDE_CLIENT, [&](const std::string& s) { out.push_back(s); });
        return out;
    };
    std::string gc = kprobe::strf("0x%X", (unsigned)(uintptr_t)w.gc());
    std::string world = kprobe::strf("0x%X", (unsigned)(uintptr_t)&w.gc()->world);
    auto contains = [](const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; };

    auto list = run("inspect creatures " + world);
    REQUIRE(list.size() == 6);
    CHECK(list[0] == "creatures of the World at " + kprobe::strf("0x%08X", (unsigned)(uintptr_t)&w.gc()->world) + ": 5");
    CHECK(contains(list[1], "id 1 "));
    CHECK(contains(list[1], "cube::Creature"));
    CHECK(contains(list[5], "id 4294967301 "));
    // the local player is marked only when the controller is known: here cube::game_controller doesn't resolve
    CHECK_FALSE(contains(list[1], "(local player)"));
    CHECK(contains(run("inspect player")[0], "cube::game_controller does not resolve"));

    auto player = run("inspect player " + gc);
    REQUIRE(player.size() >= 2);
    CHECK(player[0] == "local player: cube::Creature at " +
                       kprobe::strf("0x%08X", (unsigned)(uintptr_t)w.creatures[0].data()) + " (GameController+0x8006D0)");
    CHECK(contains(player[1], "cube::Creature"));               // then the object's rows, as inspect <type> shows them
    CHECK(run("inspect player")[0] == player[0]);               // the addresses given last are used again
    list = run("inspect creatures");
    REQUIRE(list.size() == 6);
    CHECK(contains(list[1], "(local player)"));
    CHECK_FALSE(contains(list[2], "(local player)"));
    w.gc()->player = nullptr;
    CHECK(run("inspect player " + gc)[0] == "no local player: cube::GameController.player is null");

    // the expectation a probe record checks
    CHECK(contains(run("expect cube::World.creatures creatures == 5 " + world)[0],
                   "expect cube::World.creatures: held (5 creature(s) in the World at"));
    CHECK(contains(run("expect - creatures > 5 " + world)[0], "FAILED (5 creature(s)"));
    w.gc()->world.creatures.size = 9;
    CHECK(contains(run("expect - creatures == 5 " + world)[0], "FAILED (5 node(s), but the map's size is 9"));
    CHECK(contains(run("expect - creatures ~ 5 " + world)[0], "usage:"));

    kprobe::Probe::shutdown(KY_SIDE_CLIENT);
    CHECK(kprobe::Probe::get() == nullptr);
    Console::get().remove_mod(&m);
    Interfaces::get().remove_mod(&m);
    L().dev_mode = false;
}

namespace {
int g_fake_calls = 0;
void* __cdecl fake_new(size_t) { ++g_fake_calls; return nullptr; }
void __cdecl fake_delete(void*) { ++g_fake_calls; }
}  // namespace

TEST_CASE("live-probes: the allocator check") {
    L().host.host = KY_HOST_CLIENT;
    L().dev_mode = true;
    Mod& m = probe_mod();
    const KyubuApi* api = api_table();
    REQUIRE(kprobe::Probe::init(api, m.handle(KY_SIDE_CLIENT), KY_SIDE_CLIENT) == 0);
    kprobe::Probe* p = kprobe::Probe::get();
    REQUIRE(p);
    auto run = [&](const std::string& line) {
        std::vector<std::string> out;
        p->run(line, KY_SIDE_CLIENT, [&](const std::string& s) { out.push_back(s); });
        return out;
    };
    auto contains = [](const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; };
    CHECK(contains(run("expect - alloc 0x1000")[0], "usage:"));
    CHECK(contains(run("expect - alloc 0x1000 0x2000 0")[0], "usage:"));

    // Two jmp [slot] thunks, as the game's (Cube.exe @0x68C3F8 and @0x68C3FE), in executable memory.
    auto* page = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    REQUIRE(page);
    uint32_t* slots = (uint32_t*)(page + 32);
    for (int i = 0; i < 2; ++i) {
        page[8 * i] = 0xFF;
        page[8 * i + 1] = 0x25;
        uint32_t slot = (uint32_t)(uintptr_t)&slots[i];
        memcpy(page + 8 * i + 2, &slot, 4);
    }
    std::string thunks = kprobe::strf("0x%X 0x%X", (unsigned)(uintptr_t)page, (unsigned)(uintptr_t)(page + 8));

    HMODULE crt = LoadLibraryA("msvcr110.dll");   // kept loaded: the loader may resolve game_alloc to it
    if (!crt) {
        CHECK(contains(run("expect - alloc " + thunks)[0], "FAILED (MSVCR110.dll is not loaded)"));
        MESSAGE("MSVCR110.dll is not installed: the allocator check's other paths are skipped");
    } else {
        // thunks to other functions: FAILED, and nothing is called
        slots[0] = (uint32_t)(uintptr_t)&fake_new;
        slots[1] = (uint32_t)(uintptr_t)&fake_delete;
        auto out = run("expect operator_new alloc " + thunks);
        CHECK(contains(out[0], "expect operator_new: FAILED ("));
        CHECK(contains(out[0], "not the loader's pair"));
        CHECK(g_fake_calls == 0);

        // thunks to MSVCR110.dll's pair: held exactly when the loader's game_alloc hands out blocks of that heap
        slots[0] = (uint32_t)(uintptr_t)GetProcAddress(crt, "??2@YAPAXI@Z");
        slots[1] = (uint32_t)(uintptr_t)GetProcAddress(crt, "??3@YAXPAX@Z");
        auto heap = (HANDLE)((intptr_t(__cdecl*)())GetProcAddress(crt, "_get_heap_handle"))();
        void* probe_block = api->game_alloc(8);
        bool loader_uses_crt = HeapValidate(heap, 0, probe_block) != 0;
        api->game_free(probe_block);
        out = run("expect operator_new alloc " + thunks + " 20");
        CHECK(contains(out[0], "leads to MSVCR110.dll!??2@YAPAXI@Z"));
        CHECK(contains(out[0], "to MSVCR110.dll!??3@YAXPAX@Z"));
        if (loader_uses_crt) {
            CHECK(contains(out[0], "expect operator_new: held ("));
            CHECK(contains(out[0], "20 round(s)"));
            CHECK(contains(out[0], "heap valid"));
        } else {
            CHECK(contains(out[0], "expect operator_new: FAILED ("));
            CHECK(contains(out[0], "game_alloc's block"));
        }
    }
    VirtualFree(page, 0, MEM_RELEASE);

    kprobe::Probe::shutdown(KY_SIDE_CLIENT);
    Console::get().remove_mod(&m);
    Interfaces::get().remove_mod(&m);
    L().dev_mode = false;
}
