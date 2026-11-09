// Task 7.2: the Install tab's list and details, the Verified / Unverified mark and the update of a My mods row (no game calls).
// Build like test_index.cpp: -DKYUBU_MM_TEST_KEY, the monocypher objects, -I loader/src for mods/semver.h, and loader/src/mods/semver.cpp.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "browse.h"

namespace {

cindex::Version ver(const char* v, const char* kyubu, const char* sha, long size, const char* changes = "") {
    cindex::Version x;
    x.version = v;
    x.kyubu = kyubu;
    x.sha256 = sha;
    x.size = size;
    x.changes = changes;
    return x;
}

cindex::Index sample() {
    cindex::Index ix;
    cindex::Mod a;
    a.id = "cat_alpha"; a.name = "Alpha"; a.authors = {"Ana", "Bo"}; a.description = "First mod.";
    a.versions = {ver("1.0.0", "^1.1", "aa", 507), ver("1.1.0", "^1.1", "bb", 2048, "Faster."), ver("2.0.0", "^2.0", "cc", 3000), ver("1.2.0-beta", "^1.1", "dd", 10)};
    a.versions[1].needs["cat_lib"] = "^1.0";
    cindex::Mod b;
    b.id = "cat_beta"; b.name = "Beta"; b.authors = {"Cy"};
    b.versions = {ver("1.0.0", "^1.1", "ee", 487)};
    cindex::Mod n;
    n.id = "cat_new"; n.name = "Newer"; n.versions = {ver("1.0.0", "^3.0", "ff", 5)};
    ix.mods = {a, b, n};
    return ix;
}

float width_of(const std::wstring& s) { return static_cast<float>(s.size()) * 10.0f; }

}  // namespace

TEST_CASE("the newest release that fits the loader") {
    const cindex::Index ix = sample();
    CHECK(browse::latest_fit(ix.mods[0], "1.2.0")->version == "1.1.0");   // 2.0.0 needs API 2, the beta is never offered
    CHECK(browse::latest_fit(ix.mods[0], "2.0.0")->version == "2.0.0");
    CHECK(browse::latest_fit(ix.mods[2], "1.2.0") == nullptr);
}

TEST_CASE("the mark follows the package hash (D5)") {
    const cindex::Index ix = sample();
    CHECK(browse::mark_of(&ix, "cat_alpha", "bb").mark == list::Mark::Verified);
    CHECK(browse::mark_of(&ix, "cat_alpha", "bb").reason.empty());
    const browse::Verdict changed = browse::mark_of(&ix, "cat_alpha", "zz");
    CHECK(changed.mark == list::Mark::Unverified);
    CHECK(changed.reason == L"Changed since it was verified, or from another source.");
    const browse::Verdict hand = browse::mark_of(&ix, "gold_sword", "bb");
    CHECK(hand.mark == list::Mark::Unverified);
    CHECK(hand.reason == L"Not from the catalog. Installed by hand.");
    CHECK(browse::mark_of(nullptr, "cat_alpha", "bb").mark == list::Mark::Unknown);
}

TEST_CASE("the hash is written as 64 lower-case hex digits") {
    unsigned char h[32];
    for (int i = 0; i < 32; ++i) h[i] = static_cast<unsigned char>(i * 8 + 1);
    const std::string s = browse::hex(h);
    CHECK(s.size() == 64);
    CHECK(s.substr(0, 6) == "010911");
    CHECK(s.substr(60) == "f1f9");
}

TEST_CASE("update_for offers only a newer release that fits") {
    const cindex::Index ix = sample();
    CHECK(browse::update_for(&ix, "cat_alpha", "1.0.0", "1.2.0") == "1.1.0");
    CHECK(browse::update_for(&ix, "cat_alpha", "1.1.0", "1.2.0").empty());
    CHECK(browse::update_for(&ix, "cat_alpha", "1.3.0", "1.2.0").empty());   // installed is newer
    CHECK(browse::update_for(&ix, "gold_sword", "1.0.0", "1.2.0").empty());
    CHECK(browse::update_for(&ix, "cat_alpha", "1.0.0", "2.0.0") == "2.0.0");
    CHECK(browse::update_for(nullptr, "cat_alpha", "1.0.0", "1.2.0").empty());
}

TEST_CASE("the Install list: offers, sizes and order") {
    const cindex::Index ix = sample();
    const std::vector<list::Mod> m = browse::catalog_mods(ix, {{"cat_alpha", "1.0.0"}, {"cat_beta", "1.0.0"}}, "1.2.0");
    REQUIRE(m.size() == 3);
    CHECK(m[0].offer == list::Offer::Update);
    CHECK(m[0].version == L"1.1.0");
    CHECK(m[0].update_to == L"1.1.0");
    CHECK(m[0].have == L"1.0.0");
    CHECK(m[0].size_text == L"2 KB");
    CHECK(m[0].authors == L"Ana, Bo");
    CHECK(m[1].offer == list::Offer::Installed);
    CHECK(m[1].size_text == L"487 B");
    CHECK(m[2].offer == list::Offer::None);   // nothing fits the loader
    CHECK(m[2].version == L"-");
    REQUIRE(m[0].vers.size() == 4);
    CHECK(m[0].vers[0].version == L"2.0.0");   // newest first
    CHECK(!m[0].vers[0].fits);
    CHECK(m[0].vers[2].needs == L"cat_lib ^1.0");
    const std::vector<list::Mod> g = browse::catalog_mods(ix, {}, "1.2.0");
    CHECK(g[0].offer == list::Offer::Get);
    CHECK(g[0].version == L"1.1.0");
}

TEST_CASE("the Install list filters and counts") {
    const cindex::Index ix = sample();
    list::View v;
    v.catalog = true;
    v.set_mods(browse::catalog_mods(ix, {{"cat_alpha", "1.0.0"}, {"cat_beta", "1.0.0"}}, "1.2.0"));
    CHECK(v.shown.size() == 3);
    CHECK(v.count_text() == L"3 MODS, 2 INSTALLED");
    v.step_filter(1);
    CHECK(v.filter == list::Filter::Get);
    CHECK(v.shown.empty());   // the one mod nothing fits has no offer, so it is not "Get"
    v.step_filter(1);
    CHECK(v.filter == list::Filter::Updates);
    CHECK(v.shown.size() == 1);
    v.step_filter(1);
    CHECK(v.filter == list::Filter::Installed);
    CHECK(v.shown.size() == 2);
    v.step_filter(1);
    CHECK(v.filter == list::Filter::All);
    v.step_filter(-1);
    CHECK(v.filter == list::Filter::Installed);
    v.filter = list::Filter::All;
    v.rebuild();
    v.type_char('b'); v.type_char('o');   // an author
    CHECK(v.shown.size() == 1);
}

TEST_CASE("the My mods filters still cycle on their own") {
    list::View v;
    v.step_filter(1);
    CHECK(v.filter == list::Filter::Enabled);
    v.step_filter(-1); v.step_filter(-1);
    CHECK(v.filter == list::Filter::Updates);
}

TEST_CASE("the details of a catalog mod") {
    const cindex::Index ix = sample();
    const std::vector<list::Mod> m = browse::catalog_mods(ix, {{"cat_alpha", "1.0.0"}}, "1.2.0");
    const std::vector<list::InfoRow> rows = browse::info_rows(m[0], 400.0f, width_of);
    REQUIRE(rows.size() > 8);
    CHECK(rows[0].line.segs[0].text == L"by Ana, Bo");
    CHECK(rows[1].line.segs[0].text == L"Verified");
    CHECK(rows[1].line.segs[2].text == L"Installed 1.0.0, update to 1.1.0");
    bool versions = false, changes = false, needs = false, loader = false;
    for (const list::InfoRow& r : rows) {
        versions = versions || r.label == L"VERSIONS:";
        for (const list::Seg& s : r.line.segs) {
            changes = changes || s.text.find(L"Faster.") != std::wstring::npos;
            needs = needs || s.text.find(L"needs cat_lib ^1.0") != std::wstring::npos;
            loader = loader || s.text.find(L"needs a newer loader") != std::wstring::npos;
        }
    }
    CHECK(versions);
    CHECK(changes);
    CHECK(needs);
    CHECK(loader);
}

TEST_CASE("the Actions menu of a catalog mod") {
    const cindex::Index ix = sample();
    const std::vector<list::Mod> g = browse::catalog_mods(ix, {}, "1.2.0");
    const std::vector<list::Mod> i = browse::catalog_mods(ix, {{"cat_alpha", "1.0.0"}, {"cat_beta", "1.0.0"}}, "1.2.0");
    browse::Menu a = browse::menu_of(g[0], true);
    CHECK(a.download_ok);
    CHECK(a.download_label == L"Download 1.1.0");
    a = browse::menu_of(g[0], false);
    CHECK(!a.download_ok);
    CHECK(a.tip == L"The catalog is offline. Downloads wait for it.");
    CHECK(browse::menu_of(i[0], true).download_label == L"Update to 1.1.0");
    const browse::Menu done = browse::menu_of(i[1], true);
    CHECK(done.download_label == L"Installed");
    CHECK(!done.download_ok);
    CHECK(!browse::menu_of(g[2], true).download_ok);
}

TEST_CASE("sizes and banner") {
    CHECK(browse::size_text(0) == L"0 B");
    CHECK(browse::size_text(1536) == L"2 KB");
    CHECK(browse::size_text(3 * 1024 * 1024) == L"3.0 MB");
    CHECK(browse::banner(cindex::State::Online) == L"CATALOG: ONLINE");
    CHECK(browse::banner(cindex::State::Offline) == L"CATALOG: OFFLINE");
    CHECK(browse::utf8_wide("a\xC3\xA9") == std::wstring(L"a\x00E9"));
}
