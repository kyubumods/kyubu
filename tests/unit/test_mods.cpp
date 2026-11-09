#include "doctest.h"

#include <stdexcept>

#include "manifest.h"
#include "resolver.h"
#include "semver.h"

using namespace kyubu;

TEST_CASE("mod-packages: semantic versions and requirements") {
    auto v = [](const char* s) { return *Version::parse(s); };
    auto r = [](const char* s) { return *VersionReq::parse(s); };
    CHECK(v("1.2.3") < v("1.10.0"));
    CHECK(v("1.0.0-beta") < v("1.0.0"));
    CHECK(r("^1.2").matches(v("1.9.0")));
    CHECK_FALSE(r("^1.2").matches(v("2.0.0")));
    CHECK_FALSE(r("^1.2").matches(v("1.1.9")));
    CHECK(r("^0.3").matches(v("0.3.5")));
    CHECK_FALSE(r("^0.3").matches(v("0.4.0")));
    CHECK(r("~1.2.3").matches(v("1.2.9")));
    CHECK_FALSE(r("~1.2.3").matches(v("1.3.0")));
    CHECK(r(">=1.0 <2.0").matches(v("1.5.0")));
    CHECK(r(">=1.0, <2.0").matches(v("1.0.0")));
    CHECK_FALSE(r(">= 1.0, < 2.0").matches(v("2.0.0")));
    CHECK(r("=1.2.3").matches(v("1.2.3")));
    CHECK(r("*").matches(v("9.9.9")));
    CHECK(r("1.4").matches(v("1.7.0")));   // bare = caret
    CHECK_FALSE(VersionReq::parse("nonsense").has_value());
    CHECK_FALSE(Version::parse("1.x").has_value());
}

static const char* kGood = R"(
id = "alpha"
name = "Alpha"
version = "1.2.0"
kyubu = "^1.0"
sides = ["client", "server"]
load_after = ["beta"]
requires_server = true
[dependencies]
lib = ">=1.0"
opt = { version = "*", optional = true }
[native]
common = "bin/alpha.dll"
[scripts]
client = "scripts/client.lua"
)";

TEST_CASE("mod-packages: manifest schema") {
    std::vector<std::string> errors;
    auto m = Manifest::parse(kGood, "mod.toml", errors);
    REQUIRE(m);
    CHECK(errors.empty());
    CHECK(m->id == "alpha");
    CHECK(m->sides == (KY_SIDE_CLIENT | KY_SIDE_SERVER));
    CHECK(m->dependencies.size() == 2);
    CHECK(m->requires_server);
    CHECK(m->native.at("common") == "bin/alpha.dll");
    CHECK(m->scripts.at("client") == "scripts/client.lua");
    CHECK(m->load_after == std::vector<std::string>{"beta"});
}

TEST_CASE("mod-packages: invalid id") {
    // covers: mod-packages/Invalid id
    std::vector<std::string> errors;
    auto m = Manifest::parse("id = \"My Mod\"\nname = \"x\"\nversion = \"1.0.0\"\nkyubu = \"^1.0\"\nsides = [\"client\"]\n",
                             "mods/x/mod.toml", errors);
    CHECK_FALSE(m);
    REQUIRE(!errors.empty());
    CHECK(errors[0].find("mods/x/mod.toml") != std::string::npos);
    CHECK(errors[0].find("key 'id'") != std::string::npos);
    CHECK(errors[0].find("^[a-z][a-z0-9_]{1,31}$") != std::string::npos);
}

TEST_CASE("mod-packages: missing fields are reported with their keys") {
    std::vector<std::string> errors;
    auto m = Manifest::parse("id = \"ok\"\n", "mod.toml", errors);
    CHECK_FALSE(m);
    std::string all;
    for (auto& e : errors) all += e + "\n";
    CHECK(all.find("'name'") != std::string::npos);
    CHECK(all.find("'version'") != std::string::npos);
    CHECK(all.find("'kyubu'") != std::string::npos);
    CHECK(all.find("'sides'") != std::string::npos);
}

// ---------------------------------------------------------------- resolver
namespace {
Candidate cand(const std::string& id, const std::string& ver = "1.0.0", const std::string& extra = "",
               const std::string& path = "") {
    std::string text = "id = \"" + id + "\"\nname = \"" + id + "\"\nversion = \"" + ver +
                       "\"\nkyubu = \"^1.0\"\nsides = [\"client\", \"server\"]\n" + extra;
    Candidate c;
    c.path = path.empty() ? "mods/" + id : path;
    c.manifest = Manifest::parse(text, c.path + "/mod.toml", c.errors);
    return c;
}
const Version kApi{1, 0, 0, ""};
auto all_enabled = [](const std::string&) { return true; };
const ResolvedMod& get(const Resolution& r, const std::string& id) {
    for (auto& m : r.mods) if (m.id == id) return m;
    throw std::runtime_error("no " + id);
}
}  // namespace

TEST_CASE("mod-packages: missing dependency") {
    // covers: mod-packages/Missing dependency
    auto r = resolve_mods({cand("bb", "1.0.0", "[dependencies]\naa = \">=1.0\"\n")}, all_enabled, 3, kApi);
    CHECK(get(r, "bb").status == ResolveStatus::Failed);
    CHECK(get(r, "bb").reason == "bb needs aa >=1.0, which is not installed");
}

TEST_CASE("mod-packages: cascade") {
    // covers: mod-packages/Cascade
    auto r = resolve_mods({cand("bb", "1.0.0", "[dependencies]\naa = \"*\"\n"), cand("cc", "1.0.0", "[dependencies]\nbb = \"*\"\n")},
                          all_enabled, 3, kApi);
    CHECK(get(r, "cc").status == ResolveStatus::Failed);
    CHECK(get(r, "cc").reason.find("cc needs bb, which failed to load (bb needs aa") == 0);
}

TEST_CASE("mod-packages: version mismatch and incompatibility") {
    auto r = resolve_mods({cand("aa", "0.9.0"), cand("bb", "1.0.0", "[dependencies]\naa = \"^1.0\"\n")}, all_enabled, 3, kApi);
    CHECK(get(r, "bb").reason == "bb needs aa ^1.0 but 0.9.0 is installed");
    auto r2 = resolve_mods({cand("aa", "1.0.0"), cand("bb", "1.0.0", "[incompatible]\naa = \"<2.0\"\n")}, all_enabled, 3, kApi);
    CHECK(get(r2, "bb").status == ResolveStatus::Failed);
    CHECK(get(r2, "bb").reason.find("incompatible with aa 1.0.0") == 0);
    CHECK(get(r2, "aa").status == ResolveStatus::Load);
}

TEST_CASE("mod-packages: duplicate id") {
    // covers: mod-packages/Duplicate id
    auto r = resolve_mods({cand("foo", "1.0.0", "", "mods/foo"), cand("foo", "0.9.0", "", "mods/foo-old.zip")}, all_enabled, 3, kApi);
    for (auto& m : r.mods) {
        CHECK(m.status == ResolveStatus::Failed);
        CHECK(m.reason.find("mods/foo, mods/foo-old.zip") != std::string::npos);
    }
    CHECK(r.order.empty());
}

TEST_CASE("mod-packages: API version mismatch") {
    // covers: mod-packages/API version mismatch
    Candidate c = cand("xx");
    c.manifest->api_req = *VersionReq::parse("^2.0");
    auto r = resolve_mods({c}, all_enabled, 3, Version{1, 4, 0, ""});
    CHECK(get(r, "xx").status == ResolveStatus::Failed);
    CHECK(get(r, "xx").reason == "requires loader API ^2.0 but this loader implements 1.4.0");
}

TEST_CASE("mod-packages: same result every launch") {
    // covers: mod-packages/Same result every launch
    std::vector<Candidate> a = {cand("zeta"), cand("alpha", "1.0.0", "load_after = [\"zeta\"]\n"), cand("mid"),
                                cand("lib"), cand("user", "1.0.0", "[dependencies]\nlib = \"*\"\n")};
    std::vector<Candidate> b(a.rbegin(), a.rend());   // different discovery order
    auto r1 = resolve_mods(a, all_enabled, 3, kApi);
    auto r2 = resolve_mods(b, all_enabled, 3, kApi);
    CHECK(r1.order == r2.order);
    CHECK(r1.order == std::vector<std::string>{"lib", "mid", "user", "zeta", "alpha"});
}

TEST_CASE("mod-packages: cycle") {
    // covers: mod-packages/Cycle
    auto r = resolve_mods({cand("aa", "1.0.0", "load_after = [\"bb\"]\n"), cand("bb", "1.0.0", "load_after = [\"aa\"]\n"), cand("cc")},
                          all_enabled, 3, kApi);
    CHECK(get(r, "aa").status == ResolveStatus::Failed);
    CHECK(get(r, "bb").status == ResolveStatus::Failed);
    CHECK(get(r, "aa").reason == "load order cycle: aa \xE2\x86\x92 bb \xE2\x86\x92 aa");
    CHECK(r.order == std::vector<std::string>{"cc"});
}

TEST_CASE("mod-packages: disabled mod and side filtering") {
    // covers: mod-packages/Disabled mod
    auto r = resolve_mods({cand("aa"), cand("bb", "1.0.0", "[dependencies]\naa = \"*\"\n")},
                          [](const std::string& id) { return id != "aa"; }, 3, kApi);
    CHECK(get(r, "aa").status == ResolveStatus::Disabled);
    CHECK(get(r, "bb").reason == "bb needs aa, which is disabled");
    // covers: mod-packages/Server-only mod in a multiplayer client (resolution part)
    std::vector<std::string> e;
    Candidate s;
    s.path = "mods/srv";
    s.manifest = Manifest::parse("id = \"srv\"\nname = \"s\"\nversion = \"1.0.0\"\nkyubu = \"^1.0\"\nsides = [\"server\"]\n", "m", e);
    auto r2 = resolve_mods({s}, all_enabled, KY_SIDE_CLIENT, kApi);
    CHECK(get(r2, "srv").status == ResolveStatus::SkippedSide);
    CHECK(r2.order.empty());
}
