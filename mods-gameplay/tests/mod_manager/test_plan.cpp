// Task 6.2: the planner (design D8), pure logic with the loader's semver.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plan.h"

using namespace plan;

namespace {

Installed inst(const char* id, const char* ver, bool enabled = true, bool verified = true,
               std::map<std::string, std::string> needs = {}) {
    Installed m;
    m.id = id;
    m.version = ver;
    m.enabled = enabled;
    m.verified = verified;
    m.needs = std::move(needs);
    return m;
}

CatalogVersion cv(const char* ver, const char* kyubu = "^1.1", std::map<std::string, std::string> needs = {}, long size = 100) {
    CatalogVersion v;
    v.version = ver;
    v.kyubu = kyubu;
    v.needs = std::move(needs);
    v.size = size;
    return v;
}

World world() {
    World w;
    w.api = "1.2.0";
    w.installed = {inst("mod_manager", "1.0.0")};
    w.catalog = {{"emtweaks", {cv("1.2.0"), cv("1.3.0", "^1.1", {}, 48)}},
                 {"ring", {cv("1.0.0", "^1.1", {{"emtweaks", "^1.2"}})}},
                 {"future", {cv("1.0.0", "^1.5")}},
                 {"loop_a", {cv("1.0.0", "^1.1", {{"loop_b", "*"}})}},
                 {"loop_b", {cv("1.0.0", "^1.1", {{"loop_a", "*"}})}}};
    return w;
}

const Op* op_of(const Result& r, const char* id) {
    for (const Op& o : r.ops)
        if (o.id == id) return &o;
    return nullptr;
}

}  // namespace

TEST_CASE("install: a missing mod downloads its newest fitting version") {
    Result r = install(world(), {"emtweaks"});
    REQUIRE(r.ok);
    REQUIRE(r.ops.size() == 1);
    CHECK(r.ops[0].kind == Kind::Download);
    CHECK(r.ops[0].version == "1.3.0");
    CHECK(r.ops[0].size == 48);
}

TEST_CASE("install: an installed enabled mod is kept, a disabled one is enabled") {
    World w = world();
    w.installed.push_back(inst("emtweaks", "1.3.0", true));
    CHECK(install(w, {"emtweaks"}).ops[0].kind == Kind::Keep);
    w.installed[1].enabled = false;
    Result r = install(w, {"emtweaks"});
    REQUIRE(r.ok);
    CHECK(r.ops[0].kind == Kind::Enable);
}

TEST_CASE("install: a version needing a newer loader API is refused") {
    Result r = install(world(), {"future"});
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("1.2.0") != std::string::npos);
}

TEST_CASE("install: an older version that fits the API is chosen over a newer one that does not") {
    World w = world();
    w.catalog[0].versions.push_back(cv("2.0.0", "^1.9"));
    Result r = install(w, {"emtweaks"});
    REQUIRE(r.ok);
    CHECK(r.ops[0].version == "1.3.0");
}

TEST_CASE("install: a dependency chain downloads the dependency first") {
    Result r = install(world(), {"ring"});
    REQUIRE(r.ok);
    REQUIRE(r.ops.size() == 2);
    CHECK(r.ops[0].id == "emtweaks");
    CHECK(r.ops[1].id == "ring");
}

TEST_CASE("install: an installed dependency is used, and enabled when off") {
    World w = world();
    w.installed.push_back(inst("emtweaks", "1.2.0", false));
    Result r = install(w, {"ring"});
    REQUIRE(r.ok);
    REQUIRE(r.ops.size() == 2);
    CHECK(op_of(r, "emtweaks")->kind == Kind::Enable);
    CHECK(op_of(r, "ring")->kind == Kind::Download);
}

TEST_CASE("install: an installed dependency that is too old is replaced by a download") {
    World w = world();
    w.installed.push_back(inst("emtweaks", "1.1.0", true));
    Result r = install(w, {"ring"});
    REQUIRE(r.ok);
    CHECK(op_of(r, "emtweaks")->kind == Kind::Download);
    CHECK(op_of(r, "emtweaks")->from == "1.1.0");
}

TEST_CASE("install: a dependency missing from the catalog is a reason") {
    World w = world();
    w.catalog[1].versions[0].needs["ghost"] = "*";
    Result r = install(w, {"ring"});
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("ghost") != std::string::npos);
}

TEST_CASE("install: two goals that need incompatible versions of one mod fail") {
    World w = world();
    w.catalog.push_back({"old_ring", {cv("1.0.0", "^1.1", {{"emtweaks", "=1.2.0"}})}});
    Result r = install(w, {"ring", "old_ring"});
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("emtweaks") != std::string::npos);
}

TEST_CASE("install: a cycle is a reason") {
    Result r = install(world(), {"loop_a"});
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("cycle") != std::string::npos);
}

TEST_CASE("disable: dependents go with it, dependents first") {
    World w = world();
    w.installed.push_back(inst("emtweaks", "1.3.0"));
    w.installed.push_back(inst("ring", "1.0.0", true, true, {{"emtweaks", "^1.2"}}));
    w.installed.push_back(inst("spark", "1.0.0", true, true, {{"ring", "*"}}));
    Result r = disable(w, "emtweaks");
    REQUIRE(r.ok);
    REQUIRE(r.ops.size() == 3);
    CHECK(r.ops[0].id == "spark");
    CHECK(r.ops[1].id == "ring");
    CHECK(r.ops[2].id == "emtweaks");
    CHECK_FALSE(r.ops[2].also);
    CHECK(r.also_disables == std::vector<std::string>{"spark", "ring"});
}

TEST_CASE("disable: a disabled dependent is left alone, and ModManager cannot be disabled") {
    World w = world();
    w.installed.push_back(inst("emtweaks", "1.3.0"));
    w.installed.push_back(inst("ring", "1.0.0", false, true, {{"emtweaks", "*"}}));
    Result r = disable(w, "emtweaks");
    REQUIRE(r.ok);
    CHECK(r.ops.size() == 1);
    CHECK_FALSE(disable(w, "mod_manager").ok);
}

TEST_CASE("disable: a mod ModManager needs cannot be disabled") {
    World w = world();
    w.installed[0].needs["emtweaks"] = "*";
    w.installed.push_back(inst("emtweaks", "1.3.0"));
    CHECK_FALSE(disable(w, "emtweaks").ok);
}

TEST_CASE("pack: a verified installed pinned version is kept, others download") {
    World w = world();
    w.installed.push_back(inst("emtweaks", "1.3.0"));
    Result r = apply_pack(w, {{"emtweaks", "1.3.0"}, {"ring", "1.0.0"}});
    REQUIRE(r.ok);
    CHECK(op_of(r, "emtweaks")->kind == Kind::Keep);
    CHECK(op_of(r, "ring")->kind == Kind::Download);
}

TEST_CASE("pack: an Unverified installed pinned version is downloaded again") {
    World w = world();
    w.installed.push_back(inst("emtweaks", "1.3.0", true, false));
    Result r = apply_pack(w, {{"emtweaks", "1.3.0"}});
    REQUIRE(r.ok);
    CHECK(op_of(r, "emtweaks")->kind == Kind::Download);
    CHECK(op_of(r, "emtweaks")->from == "1.3.0");
}

TEST_CASE("pack: a pin older than the installed version replaces it (a downgrade)") {
    World w = world();
    w.installed.push_back(inst("emtweaks", "1.3.0"));
    Result r = apply_pack(w, {{"emtweaks", "1.2.0"}});
    REQUIRE(r.ok);
    CHECK(op_of(r, "emtweaks")->kind == Kind::Download);
    CHECK(op_of(r, "emtweaks")->version == "1.2.0");
    CHECK(op_of(r, "emtweaks")->from == "1.3.0");
}

TEST_CASE("pack: a pinned version missing from the index or needing a newer API fails") {
    CHECK_FALSE(apply_pack(world(), {{"emtweaks", "9.9.9"}}).ok);
    CHECK_FALSE(apply_pack(world(), {{"future", "1.0.0"}}).ok);
}

TEST_CASE("pack: every other enabled mod is disabled except ModManager") {
    World w = world();
    w.installed.push_back(inst("hand_made", "0.1.0", true, false));
    w.installed.push_back(inst("off_already", "1.0.0", false));
    Result r = apply_pack(w, {{"emtweaks", "1.3.0"}});
    REQUIRE(r.ok);
    CHECK(op_of(r, "hand_made")->kind == Kind::Disable);
    CHECK(op_of(r, "off_already") == nullptr);
    CHECK(op_of(r, "mod_manager") == nullptr);
}

TEST_CASE("pack: a pinned mod whose dependency the pack drops or pins out of range fails") {
    Result dropped = apply_pack(world(), {{"ring", "1.0.0"}});
    CHECK_FALSE(dropped.ok);
    CHECK(dropped.reason.find("emtweaks") != std::string::npos);
    Result range = apply_pack(world(), {{"ring", "1.0.0"}, {"emtweaks", "1.1.0"}});
    World w = world();
    w.catalog[0].versions.push_back(cv("1.1.0"));
    range = apply_pack(w, {{"ring", "1.0.0"}, {"emtweaks", "1.1.0"}});
    CHECK_FALSE(range.ok);
    CHECK(range.reason.find("1.1.0") != std::string::npos);
}

TEST_CASE("pack: a pack that disables a dependency of a mod it keeps fails") {
    // ring is pinned, emtweaks (its dependency) is installed but not in the pack: the pack would disable it.
    World w = world();
    w.installed.push_back(inst("emtweaks", "1.3.0"));
    Result r = apply_pack(w, {{"ring", "1.0.0"}});
    CHECK_FALSE(r.ok);
}
