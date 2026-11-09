// Task 8.2: Apply modpack and Restore (design D7), no game calls: the work a pack's plan turns into, the "Before" profile, and the
// restore plan from what is still installed or kept in kyubu\backup\.
// Build like test_packs.cpp: -DKYUBU_MM_TEST_KEY, the monocypher objects, -I loader/src, loader/src/mods/semver.cpp and plan.cpp.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "profile.h"

using actions::Queued;

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
    p.version = "1.0.0";
    p.kyubu = "^1.2";
    p.mods = {{"cat_alpha", "1.0.0"}, {"cat_beta", "1.0.0"}, {"cat_lib", "1.0.0"}};
    p.config = R"({"cat_alpha": {"strength": 8}, "cat_lib": {"glow": true}})";
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

std::map<std::string, std::string> g_backup;   // "<id>-<version>" -> path
std::string backup_of(const std::string& id, const std::string& v) {
    auto it = g_backup.find(id + "-" + v);
    return it == g_backup.end() ? "" : it->second;
}

int count(const profile::Work& w, Queued::Kind k, const char* id) {
    int n = 0;
    for (const Queued& q : w.queue) n += q.kind == k && q.id == id;
    return n;
}

const Queued* config_of(const profile::Work& w, const char* id, const char* key) {
    for (const Queued& q : w.queue)
        if (q.kind == Queued::Config && q.id == id && q.key == key) return &q;
    return nullptr;
}

const profile::OptSet* live_of(const profile::Work& w, const char* id, const char* key) {
    for (const profile::OptSet& o : w.live)
        if (o.id == id && o.key == key) return &o;
    return nullptr;
}

const profile::Step* step_of(const std::vector<profile::Step>& s, const char* id) {
    for (const profile::Step& x : s)
        if (x.id == id) return &x;
    return nullptr;
}

size_t index_of(const profile::Work& w, Queued::Kind k, const char* id, const char* key = "") {
    for (size_t i = 0; i < w.queue.size(); ++i)
        if (w.queue[i].kind == k && w.queue[i].id == id && w.queue[i].key == key) return i;
    return w.queue.size();
}

}  // namespace

TEST_CASE("pack work: downloads are installed from staging, enabled, and get the pack's options as config ops after the install") {
    g_fields.clear();
    plan::World w = world();
    w.installed.push_back(inst("by_hand", "0.1.0", true, false));
    const cindex::Pack p = pack();
    const packs::Plan pl = packs::plan_of(p, w, fields_of);
    REQUIRE(pl.result.ok);
    const profile::Work wk = profile::pack_work(p, pl.result, w, fields_of);
    CHECK(count(wk, Queued::Install, "cat_alpha") == 1);
    CHECK(count(wk, Queued::Enable, "cat_alpha") == 1);
    CHECK(count(wk, Queued::Install, "cat_beta") == 1);
    CHECK(count(wk, Queued::Install, "cat_lib") == 1);
    CHECK(count(wk, Queued::Disable, "by_hand") == 1);
    CHECK(count(wk, Queued::Disable, "mod_manager") == 0);
    for (const Queued& q : wk.queue)
        if (q.kind == Queued::Install) {
            CHECK(q.from == "kyubu/staging/" + q.id + "-1.0.0.zip");
            CHECK(q.prev.empty());
        }
    const Queued* s = config_of(wk, "cat_alpha", "strength");
    REQUIRE(s);
    CHECK(s->value == "8");
    CHECK_FALSE(s->optional);
    CHECK(index_of(wk, Queued::Install, "cat_alpha") < index_of(wk, Queued::Config, "cat_alpha", "strength"));
    REQUIRE(config_of(wk, "cat_lib", "glow"));
    CHECK(config_of(wk, "cat_lib", "glow")->value == "true");
    CHECK(wk.live.empty());
}

TEST_CASE("pack work: a pin older than the installed version replaces it; reset_unlisted resets its old options as optional ops") {
    g_fields.clear();
    plan::World w = world();
    w.installed.push_back(inst("cat_alpha", "1.1.0", true, true, {{"cat_lib", "^1.0"}}));
    w.installed.push_back(inst("cat_lib", "1.0.0"));
    w.installed.push_back(inst("cat_beta", "1.0.0", false));
    g_fields["cat_alpha"] = {field("strength", "int", "5", "5"), field("speed", "float", "1.0", "2.5"), field("mode", "string", "\"a\"", "\"a\"")};
    g_fields["cat_lib"] = {field("glow", "bool", "false", "false"), field("size", "int", "3", "7")};
    g_fields["cat_beta"] = {field("loud", "bool", "false", "true")};
    const cindex::Pack p = pack();
    const packs::Plan pl = packs::plan_of(p, w, fields_of);
    REQUIRE(pl.result.ok);
    const profile::Work wk = profile::pack_work(p, pl.result, w, fields_of);
    // cat_alpha 1.1.0 > 1.0.0: install, no extra Enable (the replaced one is on), the named option and the reset of `speed`
    const size_t at = index_of(wk, Queued::Install, "cat_alpha");
    REQUIRE(at < wk.queue.size());
    CHECK(wk.queue[at].version == "1.0.0");
    CHECK(wk.queue[at].prev == "1.1.0");
    CHECK(wk.queue[at].was_on);
    CHECK(count(wk, Queued::Enable, "cat_alpha") == 0);
    CHECK(config_of(wk, "cat_alpha", "strength")->value == "8");
    REQUIRE(config_of(wk, "cat_alpha", "speed"));
    CHECK(config_of(wk, "cat_alpha", "speed")->value == "1.0");
    CHECK(config_of(wk, "cat_alpha", "speed")->optional);
    CHECK_FALSE(config_of(wk, "cat_alpha", "mode"));   // at its default already
    // cat_lib stays: its options are set at once, the named one and the reset of `size`
    CHECK(count(wk, Queued::Install, "cat_lib") == 0);
    REQUIRE(live_of(wk, "cat_lib", "glow"));
    CHECK(live_of(wk, "cat_lib", "glow")->value == "true");
    REQUIRE(live_of(wk, "cat_lib", "size"));
    CHECK(live_of(wk, "cat_lib", "size")->value == "3");
    // cat_beta was off: enabled, and its option goes back to the default
    CHECK(count(wk, Queued::Enable, "cat_beta") == 1);
    REQUIRE(live_of(wk, "cat_beta", "loud"));
    CHECK(live_of(wk, "cat_beta", "loud")->value == "false");
}

TEST_CASE("pack work: without reset_unlisted only the named options change") {
    g_fields.clear();
    plan::World w = world();
    w.installed.push_back(inst("cat_alpha", "1.0.0"));
    w.installed.push_back(inst("cat_beta", "1.0.0"));
    w.installed.push_back(inst("cat_lib", "1.0.0"));
    g_fields["cat_alpha"] = {field("strength", "int", "5", "5"), field("speed", "float", "1.0", "2.5")};
    cindex::Pack p = pack();
    p.reset_unlisted = false;
    const packs::Plan pl = packs::plan_of(p, w, fields_of);
    REQUIRE(pl.result.ok);
    const profile::Work wk = profile::pack_work(p, pl.result, w, fields_of);
    CHECK(wk.queue.empty());
    CHECK(live_of(wk, "cat_alpha", "strength"));
    CHECK_FALSE(live_of(wk, "cat_alpha", "speed"));
    CHECK(live_of(wk, "cat_lib", "glow"));   // no fields known: the named value is set as it is
    CHECK_FALSE(profile::in_use(pl.result, wk));
}

TEST_CASE("pack work: a pack that is already the setup in use has nothing to do") {
    g_fields.clear();
    plan::World w = world();
    w.installed.push_back(inst("cat_alpha", "1.0.0"));
    w.installed.push_back(inst("cat_beta", "1.0.0"));
    w.installed.push_back(inst("cat_lib", "1.0.0"));
    g_fields["cat_alpha"] = {field("strength", "int", "5", "8")};
    g_fields["cat_lib"] = {field("glow", "bool", "false", "true")};
    const cindex::Pack p = pack();
    const packs::Plan pl = packs::plan_of(p, w, fields_of);
    REQUIRE(pl.result.ok);
    const profile::Work wk = profile::pack_work(p, pl.result, w, fields_of);
    CHECK(wk.empty());
    CHECK(profile::in_use(pl.result, wk));
}

TEST_CASE("profile: capture keeps the enabled mods with versions and every option, not ModManager; the JSON reads back") {
    g_fields.clear();
    plan::World w = world();
    w.installed.push_back(inst("cat_alpha", "1.1.0"));
    w.installed.push_back(inst("cat_beta", "1.0.0", false));
    g_fields["cat_alpha"] = {field("strength", "int", "5", "6"), field("mode", "string", "\"a\"", "\"b c\"")};
    g_fields["cat_beta"] = {field("loud", "bool", "false", "true")};
    g_fields["mod_manager"] = {field("catalog_url", "string", "\"\"", "\"x\"")};
    const cindex::Pack before = profile::capture(w, fields_of, profile::before_id("cat_pack"), "Before Test pack", "Your setup.");
    CHECK(before.id == "before_cat_pack");
    CHECK(before.mods == std::map<std::string, std::string>{{"cat_alpha", "1.1.0"}});
    CHECK_FALSE(before.reset_unlisted);
    cindex::Pack back;
    REQUIRE(profile::from_json(profile::to_json(before), back));
    CHECK(back.id == before.id);
    CHECK(back.name == "Before Test pack");
    CHECK(back.mods == before.mods);
    const auto cfg = packs::config_of(back);
    CHECK(cfg.at("cat_alpha").at("strength") == "6");
    CHECK(cfg.at("cat_alpha").at("mode") == "b c");
    CHECK(cfg.at("cat_beta").at("loud") == "true");
    CHECK_FALSE(cfg.count("mod_manager"));
    CHECK_FALSE(back.reset_unlisted);
}

TEST_CASE("profile: a broken file is not a profile") {
    cindex::Pack p;
    CHECK_FALSE(profile::from_json("", p));
    CHECK_FALSE(profile::from_json("[1]", p));
    CHECK_FALSE(profile::from_json(R"({"id": "Bad Id"})", p));
    CHECK_FALSE(profile::from_json(R"({"id": "ok", "mods": [1]})", p));
    CHECK(profile::from_json(R"({"id": "ok"})", p));
    CHECK(p.mods.empty());
}

TEST_CASE("restore: versions come back from the backup, the pack's mods are disabled, options are set back") {
    g_fields.clear();
    g_backup.clear();
    // before the pack: cat_alpha 1.1.0 (on, strength 6), by_hand (on); the pack then put in cat_alpha 1.0.0, cat_beta and cat_lib
    cindex::Pack before;
    before.id = "before_cat_pack";
    before.name = "Before Test pack";
    before.mods = {{"cat_alpha", "1.1.0"}, {"by_hand", "0.1.0"}, {"gone_mod", "2.0.0"}};
    before.config = R"({"cat_alpha": {"strength": "6"}, "by_hand": {"x": "1"}})";
    before.reset_unlisted = false;
    plan::World w = world();
    w.installed.push_back(inst("cat_alpha", "1.0.0"));
    w.installed.push_back(inst("cat_beta", "1.0.0"));
    w.installed.push_back(inst("cat_lib", "1.0.0"));
    w.installed.push_back(inst("by_hand", "0.1.0", false, false));
    g_fields["by_hand"] = {field("x", "int", "0", "0")};
    g_backup["cat_alpha-1.1.0"] = "kyubu/backup/cat_alpha-1.1.0.zip";
    const std::vector<profile::Step> steps = profile::restore_plan(before, w, backup_of);
    REQUIRE(step_of(steps, "cat_alpha"));
    CHECK(step_of(steps, "cat_alpha")->st == profile::St::Backup);
    CHECK(step_of(steps, "cat_beta")->st == profile::St::Disable);
    CHECK(step_of(steps, "cat_lib")->st == profile::St::Disable);
    CHECK(step_of(steps, "by_hand")->st == profile::St::Enable);
    CHECK(step_of(steps, "gone_mod")->st == profile::St::Gone);
    CHECK_FALSE(step_of(steps, "mod_manager"));
    CHECK(profile::step_text(*step_of(steps, "cat_alpha")).text == L"will change 1.0.0 > 1.1.0 from the backup");
    const profile::Work wk = profile::restore_work(before, steps, w, fields_of);
    const size_t at = index_of(wk, Queued::Install, "cat_alpha");
    REQUIRE(at < wk.queue.size());
    CHECK(wk.queue[at].from == "kyubu/backup/cat_alpha-1.1.0.zip");
    CHECK(wk.queue[at].version == "1.1.0");
    CHECK(count(wk, Queued::Enable, "cat_alpha") == 0);   // the replaced 1.0.0 is on
    CHECK(count(wk, Queued::Disable, "cat_beta") == 1);
    CHECK(count(wk, Queued::Enable, "by_hand") == 1);
    REQUIRE(config_of(wk, "cat_alpha", "strength"));
    CHECK(config_of(wk, "cat_alpha", "strength")->value == "6");
    CHECK(at < index_of(wk, Queued::Config, "cat_alpha", "strength"));
    REQUIRE(live_of(wk, "by_hand", "x"));
    CHECK(live_of(wk, "by_hand", "x")->value == "1");
    CHECK(count(wk, Queued::Install, "gone_mod") == 0);
    CHECK(profile::settings_of(wk) == 2);
}

TEST_CASE("restore: without a kept package the installed version stays and is switched on") {
    g_fields.clear();
    g_backup.clear();
    cindex::Pack before;
    before.id = "before_cat_pack";
    before.mods = {{"cat_alpha", "1.1.0"}};
    plan::World w = world();
    w.installed.push_back(inst("cat_alpha", "1.0.0", false));
    const std::vector<profile::Step> steps = profile::restore_plan(before, w, backup_of);
    CHECK(step_of(steps, "cat_alpha")->st == profile::St::Stays);
    CHECK(profile::step_text(*step_of(steps, "cat_alpha")).text == L"keeps 1.0.0: 1.1.0 is no longer kept");
    const profile::Work wk = profile::restore_work(before, steps, w, fields_of);
    CHECK(count(wk, Queued::Enable, "cat_alpha") == 1);
    CHECK(count(wk, Queued::Install, "cat_alpha") == 0);
}

TEST_CASE("restore: the setup in use has nothing to do") {
    g_fields.clear();
    g_backup.clear();
    plan::World w = world();
    w.installed.push_back(inst("cat_alpha", "1.1.0"));
    g_fields["cat_alpha"] = {field("strength", "int", "5", "6")};
    const cindex::Pack before = profile::capture(w, fields_of, "before_x", "Before X", "");
    const std::vector<profile::Step> steps = profile::restore_plan(before, w, backup_of);
    CHECK(profile::restore_work(before, steps, w, fields_of).empty());
}

TEST_CASE("profile rows: listed with the profile prefix, never as a pack id") {
    cindex::Pack a;
    a.id = "before_cat_pack";
    a.name = "Before Test pack";
    const std::vector<list::Mod> rows = profile::profile_rows({a});
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == L"profile:before_cat_pack");
    CHECK(rows[0].name == L"Before Test pack");
    CHECK(rows[0].size_text == L"PROFILE");
}
