// Task 5.5: the page shell's layout and state machine (the drawing is only checked in the game).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "page.h"

namespace {
bool inside(const page::Rect& outer, const page::Rect& r) {
    return r.x >= outer.x - 0.01f && r.y >= outer.y - 0.01f && r.right() <= outer.right() + 0.01f && r.bottom() <= outer.bottom() + 0.01f;
}
bool overlap(const page::Rect& a, const page::Rect& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}
}  // namespace

TEST_CASE("layout at 1024 x 600 and 1920 x 1080: every piece inside its parent, none overlapping") {
    for (const auto size : {std::pair<int, int>{1024, 600}, std::pair<int, int>{1920, 1080}}) {
        const page::Layout l = page::compute(size.first, size.second, 100.0f, 30.0f, 200.0f, 4);
        CAPTURE(size.first);
        CHECK(l.tall_enough);
        CHECK(inside({0, 0, static_cast<float>(size.first), static_cast<float>(size.second)}, l.frame));
        CHECK(l.back.bottom() < l.frame.y);
        for (const page::Rect& r : {l.tab[0], l.tab[1], l.list, l.details, l.bar}) CHECK(inside(l.frame, r));
        CHECK(inside(l.bar, l.discard));
        CHECK(inside(l.bar, l.apply));
        CHECK_FALSE(overlap(l.discard, l.apply));
        CHECK_FALSE(overlap(l.list, l.details));
        CHECK_FALSE(overlap(l.list, l.bar));
        CHECK_FALSE(overlap(l.tab[0], l.tab[1]));
        CHECK_FALSE(overlap(l.tab[1], l.list));
        CHECK(inside(l.details, l.actions));
        CHECK(l.menu.y >= l.actions.bottom());
        CHECK(inside(l.frame, l.menu));
        CHECK(inside({0, 0, static_cast<float>(size.first), static_cast<float>(size.second)}, l.confirm));
        CHECK(inside(l.confirm, l.yes));
        CHECK(inside(l.confirm, l.no));
        CHECK_FALSE(overlap(l.yes, l.no));
        CHECK(l.title_x == doctest::Approx(size.first * 0.5f));
    }
}

TEST_CASE("a long item name keeps the Actions button inside the details column") {
    const page::Layout l = page::compute(1024, 600, 100.0f, 30.0f, 5000.0f, 4);
    CHECK(inside(l.details, l.actions));
}

TEST_CASE("a very short window is flagged") {
    CHECK_FALSE(page::compute(1024, 300, 100.0f, 30.0f, 100.0f, 4).tall_enough);
}

TEST_CASE("menu rows are found by their place") {
    const page::Layout l = page::compute(1024, 600, 100.0f, 30.0f, 200.0f, 4);
    const float x = l.menu.x + 20.0f;
    CHECK(page::menu_row_at(l, 4, x, l.menu.y + page::kMenuPad + 5.0f) == 0);
    CHECK(page::menu_row_at(l, 4, x, l.menu.y + page::kMenuPad + page::kMenuRowH * 3 + 5.0f) == 3);
    CHECK(page::menu_row_at(l, 4, x, l.menu.y + 2.0f) == -1);   // the top padding
    CHECK(page::menu_row_at(l, 4, x, l.menu.bottom() - 2.0f) == -1);
    CHECK(page::menu_row_at(l, 4, l.menu.x - 5.0f, l.menu.y + 20.0f) == -1);
}

TEST_CASE("BACK closes at once when nothing is pending, asks first when something is") {
    page::State s;
    s.open_page();
    s.back_click();
    CHECK_FALSE(s.open);
    s.open_page();
    s.pending = 3;
    s.back_click();
    CHECK(s.open);
    CHECK(s.confirm);
    s.confirm_no();
    CHECK(s.open);
    CHECK(s.pending == 3);
    s.back_click();
    s.confirm_yes();
    CHECK_FALSE(s.open);
    CHECK(s.pending == 0);
}

TEST_CASE("Escape closes the topmost thing only") {
    page::State s;
    s.open_page();
    s.pending = 2;
    s.actions_click();
    REQUIRE(s.menu);
    s.escape();   // the menu only
    CHECK_FALSE(s.menu);
    CHECK(s.open);
    CHECK_FALSE(s.confirm);
    s.escape();   // pending: the confirmation
    CHECK(s.confirm);
    CHECK(s.open);
    s.escape();   // closes the confirmation, not the page
    CHECK_FALSE(s.confirm);
    CHECK(s.open);
    s.pending = 0;
    s.escape();
    CHECK_FALSE(s.open);
}

TEST_CASE("while the confirmation shows, tabs, the menu and BACK do nothing") {
    page::State s;
    s.open_page();
    s.pending = 1;
    s.back_click();
    REQUIRE(s.confirm);
    s.tab_click(1);
    s.actions_click();
    s.back_click();
    CHECK(s.tab == 0);
    CHECK_FALSE(s.menu);
    CHECK(s.confirm);
}

TEST_CASE("a tab click closes the menu; the page opens on My mods with nothing open") {
    page::State s;
    s.open_page();
    s.actions_click();
    s.tab_click(1);
    CHECK(s.tab == 1);
    CHECK_FALSE(s.menu);
    s.open = false;
    s.open_page();
    CHECK(s.tab == 0);
}

TEST_CASE("the apply button's label follows D2 and D14") {
    CHECK(std::wstring(page::State::apply_label(false)) == L"APPLY");
    CHECK(std::wstring(page::State::apply_label(true)) == L"APPLY & RESTART");
}
