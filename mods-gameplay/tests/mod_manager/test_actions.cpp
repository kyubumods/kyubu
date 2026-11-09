// Task 6.4: the collected changes, the Actions menu rows, the ops Apply queues and the undo record.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "actions.h"

using namespace actions;

namespace {

plan::Installed inst(const char* id, bool enabled, std::map<std::string, std::string> needs = {}) {
    plan::Installed m;
    m.id = id;
    m.version = "1.0.0";
    m.enabled = enabled;
    m.needs = std::move(needs);
    return m;
}

// a (needs b), b, c disabled (needs b), mod_manager
plan::World world() {
    plan::World w;
    w.api = "1.2.0";
    w.installed = {inst("a", true, {{"b", "*"}}), inst("b", true), inst("c", false, {{"b", "*"}}), inst("mod_manager", true)};
    return w;
}

}  // namespace

TEST_CASE("disabling a mod takes along the enabled mods that need it") {
    const plan::World w = world();
    Collected c;
    const Outcome o = toggle(w, c, "b");
    REQUIRE(o.ok);
    CHECK(o.also == std::vector<std::string>{"a"});
    CHECK(c.count() == 2);
    CHECK_FALSE(wanted(w, c, "b"));
    CHECK_FALSE(wanted(w, c, "a"));
}

TEST_CASE("toggling twice collects nothing") {
    const plan::World w = world();
    Collected c;
    REQUIRE(toggle(w, c, "b").ok);
    REQUIRE(toggle(w, c, "b").ok);   // enables b (a stays off)
    CHECK(c.count() == 1);
    CHECK(wanted(w, c, "b"));
    REQUIRE(toggle(w, c, "a").ok);
    CHECK(c.count() == 0);
}

TEST_CASE("enabling a mod enables the installed mods it needs") {
    plan::World w = world();
    w.installed[1].enabled = false;
    Collected c;
    const Outcome o = toggle(w, c, "c");
    REQUIRE(o.ok);
    CHECK(o.also == std::vector<std::string>{"b"});
    CHECK(wanted(w, c, "c"));
    CHECK(wanted(w, c, "b"));
    CHECK(c.count() == 2);
}

TEST_CASE("the manager cannot be disabled or deleted") {
    const plan::World w = world();
    Collected c;
    CHECK_FALSE(toggle(w, c, "mod_manager").ok);
    CHECK_FALSE(remove(w, c, "mod_manager").ok);
    CHECK(c.count() == 0);
}

TEST_CASE("a mod that needs a missing dependency is refused with the reason") {
    plan::World w = world();
    w.installed.push_back(inst("d", false, {{"nothere", "*"}}));
    Collected c;
    const Outcome o = toggle(w, c, "d");
    CHECK_FALSE(o.ok);
    CHECK(o.reason.find("nothere") != std::string::npos);
    CHECK(c.count() == 0);
}

TEST_CASE("delete switches the mod off first and queues one Delete") {
    const plan::World w = world();
    Collected c;
    REQUIRE(remove(w, c, "a").ok);
    CHECK(c.del.count("a") == 1);
    std::map<std::string, Pkg> pk;
    pk["a"].version = "1.0.0";
    pk["a"].zip = true;
    const std::vector<Queued> q = ops(w, c, pk);
    REQUIRE(q.size() == 1);
    CHECK(q[0].kind == Queued::Delete);
    const std::vector<Queued> u = reverse(q, pk);
    REQUIRE(u.size() == 1);
    CHECK(u[0].kind == Queued::Install);
    CHECK(u[0].from == "kyubu/backup/a-1.0.0.zip");
    cancel_delete(c, "a");
    CHECK(c.del.empty());
}

TEST_CASE("ops and their reverse") {
    const plan::World w = world();
    Collected c;
    REQUIRE(toggle(w, c, "b").ok);
    REQUIRE(toggle(w, c, "c").ok);   // c is off: this enables it again, b is off so it enables b too
    const std::vector<Queued> q = ops(w, c, {});
    const std::vector<Queued> u = reverse(q, {});
    REQUIRE(q.size() == u.size());
    for (size_t i = 0; i < q.size(); ++i) {
        CHECK(q[i].id == u[i].id);
        CHECK((q[i].kind == Queued::Enable) == (u[i].kind == Queued::Disable));
    }
}

TEST_CASE("menu rows") {
    const plan::World w = world();
    Collected c;
    Pkg p;
    p.has_options = true;
    std::vector<Row> m = menu(w, c, "a", p, false);
    REQUIRE(m.size() == 4);
    CHECK(m[0].label == "Disable");
    CHECK(m[0].kind == 0);
    CHECK(m[1].kind == 1);   // every option has its default
    CHECK(m[2].kind == 2);
    CHECK(m[3].kind == 1);   // nothing to undo
    p.options_changed = true;
    m = menu(w, c, "a", p, true);
    CHECK(m[1].kind == 0);
    CHECK(m[3].kind == 0);
    m = menu(w, c, "mod_manager", p, false);
    CHECK(m[0].kind == 1);
    CHECK_FALSE(m[0].tip.empty());
    CHECK(m[2].kind == 1);
    m = menu(w, c, "c", p, false);
    CHECK(m[0].label == "Enable");
    REQUIRE(remove(w, c, "a").ok);
    m = menu(w, c, "a", p, false);
    CHECK(m[2].label == "Keep it");
    CHECK(m[0].kind == 1);
}

TEST_CASE("a mod that did not load cannot be enabled") {
    const plan::World w = world();
    Collected c;
    Pkg p;
    p.problem = true;
    const std::vector<Row> m = menu(w, c, "c", p, false);
    CHECK(m[0].kind == 1);
}

TEST_CASE("the undo record round trips") {
    std::vector<Queued> u;
    Queued a;
    a.kind = Queued::Disable;
    a.id = "a";
    u.push_back(a);
    Queued b;
    b.kind = Queued::Install;
    b.id = "b";
    b.version = "1.2.0";
    b.from = "kyubu/backup/b-1.2.0";
    u.push_back(b);
    const std::vector<OptionUndo> o = {{"a", "ring_alpha", "0.6"}, {"b", "name", "\"two words\""}};
    const std::string text = write_record(u, o);
    std::vector<Queued> u2;
    std::vector<OptionUndo> o2;
    read_record(text, u2, o2);
    REQUIRE(u2.size() == 2);
    CHECK(u2[0].kind == Queued::Disable);
    CHECK(u2[1].from == "kyubu/backup/b-1.2.0");
    REQUIRE(o2.size() == 2);
    CHECK(o2[1].value == "\"two words\"");
    read_record("garbage\r\noption x\r\n", u2, o2);
    CHECK(u2.empty());
    CHECK(o2.empty());
}

// --- Task 7.3: downloads ------------------------------------------------------------------------------------------------------

namespace {

plan::CatalogVersion cver(const char* v, std::map<std::string, std::string> needs = {}, long size = 100) {
    plan::CatalogVersion x;
    x.version = v;
    x.kyubu = "^1.1";
    x.needs = std::move(needs);
    x.size = size;
    return x;
}

// The test catalog's shape: cat_alpha 1.0.0 and 1.1.0 need cat_lib ^1.0; cat_beta; cat_lib.
plan::World shop() {
    plan::World w = world();
    w.catalog = {{"cat_alpha", {cver("1.0.0", {{"cat_lib", "^1.0"}}), cver("1.1.0", {{"cat_lib", "^1.0"}}, 506)}},
                 {"cat_beta", {cver("1.0.0")}},
                 {"cat_lib", {cver("1.0.0", {}, 300)}},
                 {"a", {cver("1.0.0"), cver("1.4.0", {{"b", "*"}})}}};
    return w;
}

}  // namespace

TEST_CASE("downloading a mod plans the dependency it is missing") {
    const plan::World w = shop();
    Collected c;
    std::vector<plan::Op> fetch;
    const Outcome o = download(w, c, "cat_alpha", fetch);
    REQUIRE(o.ok);
    REQUIRE(fetch.size() == 2);
    CHECK(fetch[0].id == "cat_lib");   // dependencies first
    CHECK(fetch[1].id == "cat_alpha");
    CHECK(fetch[1].version == "1.1.0");
    CHECK(fetch[1].size == 506);
    CHECK(o.also == std::vector<std::string>{"cat_lib"});
    CHECK(c.count() == 2);
    CHECK(c.dl["cat_alpha"].group == c.dl["cat_lib"].group);
    CHECK(c.dl["cat_alpha"].prev.empty());
    CHECK(c.dl["cat_alpha"].needs.count("cat_lib") == 1);
    // the queue: install from staging and switch on, both
    const std::vector<Queued> q = ops(w, c, {});
    REQUIRE(q.size() == 4);
    int installs = 0, enables = 0;
    for (const Queued& x : q) {
        if (x.kind == Queued::Install) { ++installs; CHECK(x.from == staged_from(x.id, x.version)); CHECK(from_staging(x)); }
        if (x.kind == Queued::Enable) ++enables;
    }
    CHECK(installs == 2);
    CHECK(enables == 2);
    CHECK(staged_from("cat_lib", "1.0.0") == "kyubu/staging/cat_lib-1.0.0.zip");
    // its undo deletes both, with no Disable
    const std::vector<Queued> u = reverse(q, {});
    REQUIRE(u.size() == 2);
    for (const Queued& x : u) CHECK(x.kind == Queued::Delete);
}

TEST_CASE("a second download reuses the collected dependency") {
    const plan::World w = shop();
    Collected c;
    std::vector<plan::Op> fetch;
    REQUIRE(download(w, c, "cat_lib", fetch).ok);
    REQUIRE(fetch.size() == 1);
    REQUIRE(download(w, c, "cat_alpha", fetch).ok);
    REQUIRE(fetch.size() == 1);
    CHECK(fetch[0].id == "cat_alpha");
    CHECK(c.dl["cat_alpha"].group != c.dl["cat_lib"].group);
    const Outcome again = download(w, c, "cat_alpha", fetch);
    CHECK_FALSE(again.ok);
    CHECK(again.reason == "cat_alpha is already downloaded.");
    CHECK(fetch.empty());
}

TEST_CASE("an update replaces the installed version and its undo installs the old one back") {
    const plan::World w = shop();
    Collected c;
    std::vector<plan::Op> fetch;
    REQUIRE(download(w, c, "a", fetch).ok);
    REQUIRE(fetch.size() == 1);   // b, which 1.4.0 needs, is installed and on
    CHECK(fetch[0].version == "1.4.0");
    CHECK(c.dl["a"].prev == "1.0.0");
    const std::map<std::string, Pkg> pkgs = {{"a", Pkg{"1.0.0", true}}};
    const std::vector<Queued> q = ops(w, c, pkgs);
    REQUIRE(q.size() == 1);   // a is on: no Enable
    CHECK(q[0].kind == Queued::Install);
    CHECK(q[0].prev == "1.0.0");
    CHECK(q[0].was_on);
    const std::vector<Queued> u = reverse(q, pkgs);
    REQUIRE(u.size() == 1);
    CHECK(u[0].kind == Queued::Install);
    CHECK(u[0].version == "1.0.0");
    CHECK(u[0].from == "kyubu/backup/a-1.0.0.zip");
}

TEST_CASE("updating a mod that is off switches it on, and the undo switches it off again") {
    plan::World w = shop();
    w.installed[0].enabled = false;   // a
    Collected c;
    std::vector<plan::Op> fetch;
    REQUIRE(download(w, c, "a", fetch).ok);
    const std::map<std::string, Pkg> pkgs = {{"a", Pkg{"1.0.0", false}}};
    const std::vector<Queued> q = ops(w, c, pkgs);
    REQUIRE(q.size() == 2);
    CHECK(q[1].kind == Queued::Enable);
    const std::vector<Queued> u = reverse(q, pkgs);
    REQUIRE(u.size() == 3);
    CHECK(u[0].kind == Queued::Install);
    CHECK(u[0].from == "kyubu/backup/a-1.0.0");   // a folder package
    CHECK(u[1].kind == Queued::Disable);
    CHECK(u[2].kind == Queued::Disable);          // the reverse of the Enable
}

TEST_CASE("downloads that are refused") {
    plan::World w = shop();
    Collected c;
    std::vector<plan::Op> fetch;
    CHECK(download(w, c, "b", fetch).reason == "no mod b in the catalog");
    w.installed[0].version = "2.0.0";   // a is newer than anything in the catalog
    CHECK(download(w, c, "a", fetch).reason == "a has nothing newer to download.");
    w.catalog[2].versions[0].kyubu = "^2.0";   // cat_lib fits no loader: cat_alpha cannot be planned
    const Outcome o = download(w, c, "cat_alpha", fetch);
    CHECK_FALSE(o.ok);
    CHECK(o.reason.find("cat_lib") != std::string::npos);
    CHECK(c.count() == 0);
    CHECK(fetch.empty());
}

TEST_CASE("a failed download drops its whole group") {
    const plan::World w = shop();
    Collected c;
    std::vector<plan::Op> fetch;
    REQUIRE(download(w, c, "cat_beta", fetch).ok);
    REQUIRE(download(w, c, "cat_alpha", fetch).ok);
    const int g = c.dl["cat_alpha"].group;
    std::vector<std::string> ids = drop_group(c, g);
    CHECK(ids == std::vector<std::string>{"cat_alpha", "cat_lib"});
    CHECK(c.dl.size() == 1);
    CHECK(c.dl.count("cat_beta") == 1);
    c.clear();
    CHECK(c.count() == 0);
}

TEST_CASE("a download takes the place of a collected delete or switch") {
    const plan::World w = shop();
    Collected c;
    REQUIRE(remove(w, c, "a").ok);
    std::vector<plan::Op> fetch;
    REQUIRE(download(w, c, "a", fetch).ok);
    CHECK(c.del.empty());
    CHECK(c.enable.count("a") == 0);
    CHECK(effective(w, c).installed[0].version == "1.4.0");
}

TEST_CASE("the undo record keeps a delete") {
    std::vector<Queued> u(1);
    u[0].kind = Queued::Delete;
    u[0].id = "cat_alpha";
    std::vector<Queued> u2;
    std::vector<OptionUndo> o2;
    read_record(write_record(u, {}), u2, o2);
    REQUIRE(u2.size() == 1);
    CHECK(u2[0].kind == Queued::Delete);
    CHECK(u2[0].id == "cat_alpha");
}
