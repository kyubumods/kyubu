// Task 8.1: Install / Modpacks, the pack plan and its display (design D7), no game calls.
// Build like test_browse.cpp: -DKYUBU_MM_TEST_KEY, the monocypher objects, -I loader/src, loader/src/mods/semver.cpp and plan.cpp.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "packs.h"

using plan::Kind;

namespace {

plan::Installed inst(const char* id, const char* ver, bool enabled = true, bool verified = true, std::map<std::string, std::string> needs = {}) {
    plan::Installed m;
    m.id = id;
    m.version = ver;
    m.enabled = enabled;
    m.verified = verified;
    m.needs = std::move(needs);
    return m;
}

plan::CatalogVersion cv(const char* ver, std::map<std::string, std::string> needs = {}, long size = 2048) {
    plan::CatalogVersion v;
    v.version = ver;
    v.kyubu = "^1.1";
    v.needs = std::move(needs);
    v.size = size;
    return v;
}

// The test catalog's shape: cat_alpha 1.0.0 and 1.1.0 (1.1.0 needs cat_lib), cat_beta, cat_lib.
plan::World world() {
    plan::World w;
    w.api = "1.2.0";
    w.installed = {inst("mod_manager", "1.0.0")};
    w.catalog = {{"cat_alpha", {cv("1.0.0", {}, 507), cv("1.1.0", {{"cat_lib", "^1.0"}})}},
                 {"cat_beta", {cv("1.0.0", {}, 487)}},
                 {"cat_lib", {cv("1.0.0", {}, 300)}}};
    return w;
}

cindex::Pack pack() {
    cindex::Pack p;
    p.id = "cat_pack";
    p.name = "Test pack";
    p.authors = {"Kyubu tests"};
    p.version = "1.0.0";
    p.description = "Pins the older cat_alpha.";
    p.kyubu = "^1.2";
    p.mods = {{"cat_alpha", "1.0.0"}, {"cat_beta", "1.0.0"}, {"cat_lib", "1.0.0"}};
    p.config = R"({"cat_alpha": {"strength": 8}})";
    return p;
}

form::Field field(const char* key, const char* type, const char* def, const char* value) {
    return form::make_field(key, type, key, "", "", def, value, "", "", "", false);
}

std::map<std::string, std::vector<form::Field>> g_fields;
const std::vector<form::Field>* fields_of(const std::string& id) {
    auto it = g_fields.find(id);
    return it == g_fields.end() ? nullptr : &it->second;
}

const plan::Op* op_of(const packs::Plan& p, const char* id) {
    for (const plan::Op& op : p.result.ops)
        if (op.id == id) return &op;
    return nullptr;
}

std::wstring flat(const list::InfoRow& r) {
    std::wstring s = r.label;
    for (const list::Seg& seg : r.line.segs) s += seg.text;
    return s;
}

bool has_line(const std::vector<list::InfoRow>& rows, const std::wstring& text) {
    for (const list::InfoRow& r : rows)
        if (flat(r) == text) return true;
    return false;
}

float width(const std::wstring& s) { return 8.0f * static_cast<float>(s.size()); }
std::wstring name_of(const std::string& id) { return browse::wide(id); }

}  // namespace

TEST_CASE("pack plan: a pin older than the installed version is a change down, never the latest") {
    g_fields.clear();
    plan::World w = world();
    w.installed.push_back(inst("cat_alpha", "1.1.0", true, true, {{"cat_lib", "^1.0"}}));
    w.installed.push_back(inst("cat_lib", "1.0.0"));
    const packs::Plan p = packs::plan_of(pack(), w, fields_of);
    REQUIRE(p.result.ok);
    const plan::Op* a = op_of(p, "cat_alpha");
    REQUIRE(a);
    CHECK(a->kind == Kind::Download);
    CHECK(a->version == "1.0.0");
    CHECK(a->from == "1.1.0");
    CHECK(packs::op_text(*a).text == L"will change 1.1.0 > 1.0.0 (507 B)");
    CHECK(op_of(p, "cat_lib")->kind == Kind::Keep);
    CHECK(packs::op_text(*op_of(p, "cat_lib")).text == L"installed 1.0.0");
    CHECK(packs::op_text(*op_of(p, "cat_beta")).text == L"will download 1.0.0 (487 B)");
}

TEST_CASE("pack plan: a missing mod downloads the pinned version, not the latest") {
    g_fields.clear();
    const packs::Plan p = packs::plan_of(pack(), world(), fields_of);
    REQUIRE(p.result.ok);
    CHECK(op_of(p, "cat_alpha")->kind == Kind::Download);
    CHECK(op_of(p, "cat_alpha")->version == "1.0.0");
    CHECK(op_of(p, "cat_alpha")->from.empty());
}

TEST_CASE("pack plan: other installed mods are disabled, ModManager is not; a disabled pinned mod is enabled") {
    g_fields.clear();
    plan::World w = world();
    w.installed.push_back(inst("cat_beta", "1.0.0", false));
    w.installed.push_back(inst("by_hand", "0.1.0", true, false));
    const packs::Plan p = packs::plan_of(pack(), w, fields_of);
    REQUIRE(p.result.ok);
    CHECK(op_of(p, "cat_beta")->kind == Kind::Enable);
    CHECK(packs::op_text(*op_of(p, "cat_beta")).text == L"installed 1.0.0, will be enabled");
    REQUIRE(op_of(p, "by_hand"));
    CHECK(op_of(p, "by_hand")->kind == Kind::Disable);
    CHECK(packs::op_text(*op_of(p, "by_hand")).text == L"will be disabled");
    CHECK_FALSE(op_of(p, "mod_manager"));
}

TEST_CASE("pack plan: an installed pinned version that is Unverified is downloaded again") {
    g_fields.clear();
    plan::World w = world();
    w.installed.push_back(inst("cat_beta", "1.0.0", true, false));
    const packs::Plan p = packs::plan_of(pack(), w, fields_of);
    REQUIRE(p.result.ok);
    CHECK(op_of(p, "cat_beta")->kind == Kind::Download);
    CHECK(packs::op_text(*op_of(p, "cat_beta")).text == L"will download 1.0.0 again, not verified (487 B)");
}

TEST_CASE("pack plan: a loader that does not fit the pack, or a pin the catalog lacks, cannot be applied") {
    g_fields.clear();
    cindex::Pack p = pack();
    p.kyubu = "^1.5";
    packs::Plan r = packs::plan_of(p, world(), fields_of);
    CHECK_FALSE(r.result.ok);
    CHECK(r.result.reason == "the pack needs loader API ^1.5, this is 1.2.0");
    p = pack();
    p.mods["cat_alpha"] = "0.9.0";
    r = packs::plan_of(p, world(), fields_of);
    CHECK_FALSE(r.result.ok);
    CHECK(r.result.reason == "cat_alpha 0.9.0 is not in the catalog");
    const auto rows = packs::info_rows(p, r, name_of, 400.0f, width);
    CHECK(has_line(rows, L"CANNOT BE APPLIED:"));
    CHECK(has_line(rows, L"cat_alpha 0.9.0 is not in the catalog"));
    CHECK(has_line(rows, L"cat_alpha  0.9.0"));
    CHECK_FALSE(has_line(rows, L"PLAN:"));
}

TEST_CASE("pack settings: named values that differ, unlisted values off their default, a download counts its named ones") {
    plan::World w = world();
    w.installed.push_back(inst("cat_alpha", "1.0.0"));
    w.installed.push_back(inst("cat_lib", "1.0.0"));
    g_fields.clear();
    g_fields["cat_alpha"] = {field("strength", "int", "5", "5"), field("glow", "bool", "true", "false"), field("speed", "float", "1.0", "1.00")};
    g_fields["cat_lib"] = {field("mode", "string", "a", "b")};
    packs::Plan p = packs::plan_of(pack(), w, fields_of);
    REQUIRE(p.result.ok);
    // strength 5 -> 8, glow back to true, cat_lib mode back to a; speed "1.00" is its default 1.0
    CHECK(p.settings == 3);
    CHECK(p.settings_exact == false);   // cat_beta is downloaded and its options are not known yet
    CHECK(packs::settings_text(p) == L"3 or more settings change");

    cindex::Pack keep = pack();
    keep.reset_unlisted = false;
    w.installed.push_back(inst("cat_beta", "1.0.0"));
    p = packs::plan_of(keep, w, fields_of);
    CHECK(p.settings == 1);   // only strength
    CHECK(p.settings_exact);
    CHECK(packs::settings_text(p) == L"1 setting changes");

    g_fields["cat_alpha"][0].value = "8";
    p = packs::plan_of(keep, w, fields_of);
    CHECK(p.settings == 0);
    CHECK(packs::settings_text(p) == L"0 settings change");
}

TEST_CASE("pack config: values become the loader's option text") {
    cindex::Pack p = pack();
    p.config = R"({"a": {"on": true, "n": 3, "x": 0.5, "s": "hi"}, "bad": 4})";
    const auto c = packs::config_of(p);
    CHECK(c.at("a").at("on") == "true");
    CHECK(c.at("a").at("n") == "3");
    CHECK(c.at("a").at("x") == "0.5");
    CHECK(c.at("a").at("s") == "hi");
    CHECK(c.count("bad") == 0);
    p.config = "not json";
    CHECK(packs::config_of(p).empty());
}

TEST_CASE("pack rows and details") {
    cindex::Index ix;
    ix.packs = {pack()};
    const auto rows = packs::pack_rows(ix);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].name == L"Test pack");
    CHECK(rows[0].authors == L"Kyubu tests");
    CHECK(rows[0].size_text == L"3 MODS");
    CHECK(rows[0].version == L"1.0.0");
    CHECK(packs::find_pack(ix, "cat_pack") != nullptr);
    CHECK(packs::find_pack(ix, "nope") == nullptr);

    g_fields.clear();
    plan::World w = world();
    w.installed.push_back(inst("cat_lib", "1.0.0"));
    w.installed.push_back(inst("other", "2.0.0"));
    const packs::Plan p = packs::plan_of(pack(), w, fields_of);
    const auto info = packs::info_rows(pack(), p, name_of, 600.0f, width);
    CHECK(has_line(info, L"by Kyubu tests"));
    CHECK(has_line(info, L"Verified  -  3 MODS"));
    CHECK(has_line(info, L"Loader ^1.2"));
    CHECK(has_line(info, L"Pins the older cat_alpha."));
    CHECK(has_line(info, L"PLAN:"));
    CHECK(has_line(info, L"cat_alpha  will download 1.0.0 (507 B)"));
    CHECK(has_line(info, L"cat_lib  installed 1.0.0"));
    CHECK(has_line(info, L"other  will be disabled"));
    CHECK(has_line(info, L"1 or more settings change"));
    CHECK(has_line(info, L"Options the pack does not name go back to their defaults."));

    // a narrow column: the op goes under the name, and the long lines wrap
    const auto narrow = packs::info_rows(pack(), p, name_of, 200.0f, width);
    CHECK(has_line(narrow, L"cat_alpha"));
    CHECK(has_line(narrow, L"  will download 1.0.0"));
    for (const list::InfoRow& r : narrow) CHECK(width(flat(r)) <= 200.0f);
}
