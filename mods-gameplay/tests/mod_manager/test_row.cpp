// Task 5.4: where the moved Exit row goes (the draw itself is only checked in the game).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "row.h"

TEST_CASE("tall window: Exit stays in the widget at y 180") {
    const row::Layout l = row::exit_layout(1024, 768, 200.0f);
    CHECK_FALSE(l.corner);
    CHECK(l.x == 100.0f);
    CHECK(l.y == 180.0f);
    CHECK(l.flags == 1);
    CHECK(row::inside(l, 10.0f, 165.0f));
    CHECK_FALSE(row::inside(l, 10.0f, 195.0f));
    CHECK_FALSE(row::inside(l, 10.0f, 130.0f));   // the game's Mods row is not Exit
}

TEST_CASE("the limit is 480") {
    CHECK_FALSE(row::exit_layout(800, 480, 200.0f).corner);
    CHECK(row::exit_layout(800, 479, 200.0f).corner);
}

TEST_CASE("short window: Exit lands 10 px from the left edge and 30 px above the bottom, in window pixels") {
    const int w = 800, h = 400;
    const row::Layout l = row::exit_layout(w, h, 200.0f);
    REQUIRE(l.corner);
    CHECK(l.flags == 0);
    const float ox = (w - 200.0f) * 0.5f, oy = h * 0.5f + 50.0f;   // the widget's place in the window
    CHECK(ox + l.x == doctest::Approx(10.0f));
    CHECK(oy + l.y == doctest::Approx(370.0f));
    CHECK(row::inside(l, l.x + 5.0f, l.y - 5.0f));
    CHECK_FALSE(row::inside(l, l.x + 100.0f, l.y - 5.0f));
    CHECK(oy + l.y1 <= h);   // the band stays in the window
}
