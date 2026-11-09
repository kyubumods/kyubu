// Task 6.1: the My mods list, its filters, search, selection, scroll and the lines of the details (no game calls).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "list.h"
#include "page.h"

namespace {

list::Mod make(const wchar_t* id, const wchar_t* name, list::State st, const wchar_t* authors = L"Kyubu tests") {
    list::Mod m;
    m.id = id;
    m.name = name;
    m.version = L"1.0.0";
    m.authors = authors;
    m.state = st;
    return m;
}

list::View sample() {
    list::View v;
    list::Mod blaster = make(L"blaster", L"Blaster", list::State::Failed, L"someone");
    blaster.reason = L"requires kyubu ^9.0";
    v.set_mods({make(L"emtweaks", L"EMTweaks", list::State::Loaded), make(L"gold_sword", L"Gold Sword", list::State::Disabled, L"Ana"),
                blaster, make(L"hand_made", L"Hand Made", list::State::New, L"me")});
    return v;
}

float width_of(const std::wstring& s) { return static_cast<float>(s.size()) * 10.0f; }

}  // namespace

TEST_CASE("order: new packages first, the rest as the loader lists them") {
    const list::View v = sample();
    REQUIRE(v.shown.size() == 4);
    CHECK(v.mods[v.shown[0]].id == L"hand_made");
    CHECK(v.mods[v.shown[1]].id == L"emtweaks");
    CHECK(v.mods[v.shown[3]].id == L"blaster");
}

TEST_CASE("filters") {
    list::View v = sample();
    v.filter = list::Filter::Enabled;
    v.rebuild();
    CHECK(v.shown.size() == 2);   // loaded and new
    v.filter = list::Filter::Disabled;
    v.rebuild();
    REQUIRE(v.shown.size() == 1);
    CHECK(v.mods[v.shown[0]].id == L"gold_sword");
    v.filter = list::Filter::Problems;
    v.rebuild();
    REQUIRE(v.shown.size() == 1);
    CHECK(v.mods[v.shown[0]].id == L"blaster");
    v.filter = list::Filter::Updates;
    v.rebuild();
    CHECK(v.shown.empty());
    CHECK(v.current() == nullptr);
    CHECK(v.selected.empty());
}

TEST_CASE("every problem state is in Problems, a crashed mod too") {
    for (list::State s : {list::State::Failed, list::State::Faulted, list::State::Skipped, list::State::Missing})
        CHECK(list::in_filter(make(L"a", L"A", s), list::Filter::Problems));
    for (list::State s : {list::State::Loaded, list::State::Disabled, list::State::New})
        CHECK_FALSE(list::in_filter(make(L"a", L"A", s), list::Filter::Problems));
}

TEST_CASE("the arrows step through the filters and wrap") {
    list::View v = sample();
    v.step_filter(-1);
    CHECK(v.filter == list::Filter::Updates);
    v.step_filter(1);
    CHECK(v.filter == list::Filter::All);
    for (int i = 0; i < list::kFilterCount; ++i) v.step_filter(1);
    CHECK(v.filter == list::Filter::All);
}

TEST_CASE("search matches name, id and author in any case, and ignores blanks around it") {
    list::View v = sample();
    for (wchar_t c : std::wstring(L"  GOLD ")) v.type_char(c);
    REQUIRE(v.shown.size() == 1);
    CHECK(v.mods[v.shown[0]].id == L"gold_sword");
    while (!v.query.empty()) v.backspace();
    for (wchar_t c : std::wstring(L"ana")) v.type_char(c);   // an author
    REQUIRE(v.shown.size() == 1);
    CHECK(v.mods[v.shown[0]].id == L"gold_sword");
    while (!v.query.empty()) v.backspace();
    for (wchar_t c : std::wstring(L"hand_")) v.type_char(c);   // an id
    REQUIRE(v.shown.size() == 1);
    CHECK(v.mods[v.shown[0]].id == L"hand_made");
    v.type_char(L'z');
    CHECK(v.shown.empty());
}

TEST_CASE("typing ignores control characters and stops at the limit") {
    list::View v = sample();
    v.type_char(8);
    v.type_char(13);
    v.type_char(127);
    CHECK(v.query.empty());
    for (int i = 0; i < 100; ++i) v.type_char(L'a');
    CHECK(v.query.size() == list::kQueryMax);
    v.backspace();
    CHECK(v.query.size() == list::kQueryMax - 1);
}

TEST_CASE("the selection stays while its mod is shown and moves to the first row when not") {
    list::View v = sample();
    CHECK(v.selected == L"hand_made");   // the first row after the first rebuild
    v.select_row(2);
    CHECK(v.selected == L"gold_sword");
    v.filter = list::Filter::Problems;
    v.rebuild();
    CHECK(v.selected == L"blaster");
    v.filter = list::Filter::All;
    v.rebuild();
    CHECK(v.selected == L"blaster");   // kept: it is shown again
    v.select_row(99);
    CHECK(v.selected == L"blaster");   // out of range does nothing
}

TEST_CASE("a dependency chip shows its mod even when the filter hides it") {
    list::View v = sample();
    v.filter = list::Filter::Problems;
    v.rebuild();
    REQUIRE(v.jump_to(L"emtweaks"));
    CHECK(v.filter == list::Filter::All);
    CHECK(v.selected == L"emtweaks");
    CHECK_FALSE(v.jump_to(L"not_installed"));
    CHECK(v.selected == L"emtweaks");
}

TEST_CASE("scroll keeps the first row inside the list, and a jump brings its row into view") {
    list::View v;
    std::vector<list::Mod> mods;
    for (int i = 0; i < 20; ++i) mods.push_back(make((L"m" + std::to_wstring(i)).c_str(), (L"Mod " + std::to_wstring(i)).c_str(), list::State::Loaded));
    v.rows = 6;
    v.set_mods(mods);
    v.scroll_rows(-5);
    CHECK(v.first == 0);
    v.scroll_rows(100);
    CHECK(v.first == 14);   // 20 rows, 6 fit
    CHECK(v.jump_to(L"m2"));
    CHECK(v.first <= 2);
    CHECK(v.first + v.rows > 2);
    v.rows = 30;
    v.clamp();
    CHECK(v.first == 0);   // everything fits
}

TEST_CASE("the count line") {
    list::View v = sample();
    CHECK(v.count_text() == L"4 MODS, 2 ON");
    v.filter = list::Filter::Disabled;
    v.rebuild();
    CHECK(v.count_text() == L"1 OF 4 MODS, 2 ON");
    list::View one;
    one.set_mods({make(L"a", L"A", list::State::Loaded)});
    CHECK(one.count_text() == L"1 MOD, 1 ON");
}

TEST_CASE("the switch of a row: on, off, problem") {
    CHECK(std::wstring(list::switch_label(list::State::Loaded)) == L"[ON ]");
    CHECK(std::wstring(list::switch_label(list::State::Disabled)) == L"[OFF]");
    CHECK(std::wstring(list::switch_label(list::State::Failed)) == L"[ ! ]");
    CHECK(std::wstring(list::switch_label(list::State::Faulted)) == L"[ ! ]");
    CHECK(std::wstring(list::switch_label(list::State::Missing)) == L"[ ! ]");
}

TEST_CASE("reasons: a failed mod gives the loader's text, new and missing give fixed ones, a loaded mod none") {
    list::Mod m = make(L"a", L"A", list::State::Failed);
    m.reason = L"missing dependency b";
    CHECK(list::reason_text(m) == L"missing dependency b");
    m.state = list::State::New;
    m.reason.clear();
    CHECK(list::reason_text(m) == L"Loads after a restart.");
    m.state = list::State::Missing;
    CHECK(list::reason_text(m) == L"Removed from the folder.");
    m.state = list::State::Loaded;
    m.reason = L"ignored";
    CHECK(list::reason_text(m).empty());
}

TEST_CASE("details lines: authors, mark and state, the reason, then the wrapped description") {
    list::Mod m = make(L"blaster", L"Blaster", list::State::Failed, L"someone, else");
    m.reason = L"requires kyubu ^9.0";
    m.description = L"A gun that goes pew and then some more words to wrap";
    const auto lines = list::details_lines(m, 200.0f, width_of);   // 20 characters per line
    REQUIRE(lines.size() >= 5);
    CHECK(lines[0].segs[0].text == L"by someone, else");
    REQUIRE(lines[1].segs.size() == 3);
    CHECK(lines[1].segs[0].text == L"Not checked");
    CHECK(lines[1].segs[2].text == L"Failed");
    CHECK(lines[1].segs[2].kind == 3);
    CHECK(lines[2].segs[0].text == L"requires kyubu ^9.0");
    CHECK(lines[2].segs[0].kind == 3);
    CHECK(lines[3].segs.empty());   // a gap before the description
    for (size_t i = 4; i < lines.size(); ++i) CHECK(lines[i].segs[0].text.size() <= 20);
}

TEST_CASE("details of a loaded mod have no reason line, and a mod without authors says so") {
    list::Mod m = make(L"a", L"A", list::State::Loaded, L"");
    m.mark = list::Mark::Verified;
    const auto lines = list::details_lines(m, 400.0f, width_of);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].segs[0].text == L"No author given");
    CHECK(lines[1].segs[0].text == L"Verified");
    CHECK(lines[1].segs[0].kind == 4);
}

TEST_CASE("wrap breaks at word gaps, honours newlines and cuts a word wider than the line") {
    const auto a = text::wrap(L"aaa bbb ccc", 70.0f, width_of);
    REQUIRE(a.size() == 2);
    CHECK(a[0] == L"aaa bbb");
    CHECK(a[1] == L"ccc");
    const auto b = text::wrap(L"one\ntwo", 500.0f, width_of);
    REQUIRE(b.size() == 2);
    const auto c = text::wrap(L"abcdefghijklmnopqrstuvwxyz", 100.0f, width_of);
    REQUIRE(c.size() == 1);
    CHECK(c[0].size() <= 10);
    CHECK(text::wrap(L"", 100.0f, width_of).size() == 1);
}

TEST_CASE("page layout: the list column's pieces stack inside the list and do not overlap") {
    for (int h : {600, 768, 1080}) {
        const page::Layout l = page::compute(1024, h, 200.0f, 67.0f, 160.0f, 4);
        CHECK(l.search.y >= l.list.y);
        CHECK(l.filter.y >= l.search.bottom());
        CHECK(l.count.y >= l.filter.bottom());
        CHECK(l.rows.y >= l.count.bottom());
        CHECK(l.rows.bottom() <= l.list.bottom());
        CHECK(l.search.right() <= l.list.right());
        CHECK(l.info.y >= l.actions.bottom());
        CHECK(l.info.right() <= l.details.right());
        CHECK(page::rows_visible(l) >= 3);
    }
}

TEST_CASE("page: rows under a point, and the search box's focus ends before the page does") {
    const page::Layout l = page::compute(1024, 768, 200.0f, 67.0f, 160.0f, 4);
    CHECK(page::row_at(l, l.rows.x + 5, l.rows.y + 1, 0, 4) == 0);
    CHECK(page::row_at(l, l.rows.x + 5, l.rows.y + page::kRowH * 2 + 1, 3, 10) == 5);   // scrolled by 3
    CHECK(page::row_at(l, l.rows.x + 5, l.rows.y + page::kRowH * 3 + 1, 0, 3) == -1);    // past the last mod
    CHECK(page::row_at(l, l.list.x, l.list.y, 0, 4) == -1);                               // the margin
    page::State s;
    s.open_page();
    s.typing = true;
    s.escape();
    CHECK_FALSE(s.typing);
    CHECK(s.open);
    s.escape();
    CHECK_FALSE(s.open);
}

TEST_CASE("info rows: the details lines, then Needs and Needed by as chips; a missing needed mod is an error chip") {
    list::View v = sample();
    list::Mod m = make(L"skills", L"Skills", list::State::Loaded);
    list::Dep ok;
    ok.id = L"emtweaks";
    ok.req = L"^1.3";
    list::Dep gone;
    gone.id = L"nowhere";
    gone.req = L"*";
    list::Dep opt;
    opt.id = L"extra";
    opt.optional = true;
    m.needs = {ok, gone, opt};
    list::Dep user;
    user.id = L"blaster";
    m.needed_by = {user};
    const auto rows = list::info_rows(v, m, 2000.0f, 20.0f, width_of);
    std::vector<list::Chip> needs, by;
    int mode = 0;
    for (const auto& r : rows) {
        if (r.label == L"NEEDS:") mode = 1;
        else if (r.label == L"NEEDED BY:") mode = 2;
        else for (const auto& c : r.chips) (mode == 1 ? needs : by).push_back(c);
    }
    REQUIRE(needs.size() == 3);
    CHECK(needs[0].text == L"EMTweaks ^1.3");   // the installed mod's name and the requirement
    CHECK(needs[0].installed);
    CHECK(needs[0].kind == 0);
    CHECK(needs[1].text == L"nowhere (missing)");   // a "*" requirement is not shown
    CHECK(needs[1].kind == 3);
    CHECK_FALSE(needs[1].installed);
    CHECK(needs[2].text == L"extra (optional)");
    CHECK(needs[2].kind == 1);
    REQUIRE(by.size() == 1);
    CHECK(by[0].text == L"Blaster");   // no requirement in Needed by
}

TEST_CASE("info rows: chips wrap under the label, and an empty side reads none") {
    list::View v = sample();
    list::Mod m = make(L"a", L"A", list::State::Loaded);
    for (const wchar_t* id : {L"emtweaks", L"blaster", L"gold_sword", L"hand_made"}) {
        list::Dep d;
        d.id = id;
        m.needs.push_back(d);
    }
    const auto rows = list::info_rows(v, m, 260.0f, 10.0f, width_of);
    int chip_rows = 0;
    size_t chips = 0;
    bool none = false, label = false;
    for (const auto& r : rows) {
        if (r.label == L"NEEDS:") label = true;
        if (!r.chips.empty()) {
            ++chip_rows;
            chips += r.chips.size();
            float w = list::kChipIndent;
            for (const auto& c : r.chips) w += width_of(c.text) + 10.0f;
            if (r.chips.size() > 1) CHECK(w - 10.0f <= 260.0f);   // a row with more than one chip fits; one chip alone may not
        }
        if (!r.line.segs.empty() && r.line.segs[0].text == L"NEEDED BY: none") none = true;
    }
    CHECK(label);
    CHECK(chips == 4);
    CHECK(chip_rows >= 2);
    CHECK(none);
}

TEST_CASE("an Unverified mark shows its reason line") {
    list::Mod m;
    m.name = L"X";
    m.mark = list::Mark::Unverified;
    m.mark_reason = L"Not from the catalog. Installed by hand.";
    auto width_of = [](const std::wstring& t) { return static_cast<float>(t.size()) * 8.0f; };
    const auto lines = list::details_lines(m, 600.0f, width_of);
    bool found = false;
    for (const auto& l : lines)
        if (!l.segs.empty() && l.segs[0].text == m.mark_reason && l.segs[0].kind == 2) found = true;
    CHECK(found);
    m.mark = list::Mark::Verified;
    CHECK(list::details_lines(m, 600.0f, width_of).size() == lines.size() - 1);
}
