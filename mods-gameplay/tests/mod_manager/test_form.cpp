// Task 6.3: the settings form's model: kinds, steps, drafts, validation, rows and clicks (the drawing is only checked in the game).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "form.h"

namespace {
form::Field mk(const char* key, const char* type, const char* def, const char* min = "", const char* max = "", const char* choices = "",
               const char* group = "", const char* label = "", const char* desc = "", bool restart = false) {
    return form::make_field(key, type, label, group, desc, def, def, min, max, choices, restart);
}
bool allow(const form::Field& f, std::string& why) {
    if (f.type == form::Type::Int || f.type == form::Type::Float) {
        const double v = form::num(f.draft, 1e300);
        if (v > 1e200) { why = "not a number"; return false; }
        if (!f.min.empty() && v < form::num(f.min, v)) { why = "too small"; return false; }
        if (!f.max.empty() && v > form::num(f.max, v)) { why = "too large"; return false; }
    }
    return true;
}
}  // namespace

TEST_CASE("a control for each kind of option") {
    CHECK(mk("a", "bool", "true").kind() == form::Kind::Picker);
    CHECK(mk("a", "string", "\"fire\"", "", "", "fire, ice").kind() == form::Kind::Picker);
    CHECK(mk("a", "string", "\"x\"").kind() == form::Kind::Text);
    CHECK(mk("a", "int", "3", "1", "9").kind() == form::Kind::Slider);
    CHECK(mk("a", "float", "0.5", "0", "1").kind() == form::Kind::Slider);
    CHECK(mk("a", "int", "3").kind() == form::Kind::Number);
    CHECK(mk("a", "float", "3.0", "0").kind() == form::Kind::Number);
}

TEST_CASE("strings lose their quotes; the label falls back to the key in words") {
    form::Field f = mk("ring_alpha", "string", "\"a \\\"b\\\"\"");
    CHECK(f.value == "a \"b\"");
    CHECK(form::label_of(f) == "Ring alpha");
    f.label = "Ring";
    CHECK(form::label_of(f) == "Ring");
}

TEST_CASE("steps: toggle, wrap, clamp") {
    form::Field b = mk("b", "bool", "true");
    form::step(b, 1);
    CHECK(b.draft == "false");
    CHECK(form::shown(b) == "OFF");
    form::Field c = mk("c", "string", "\"fire\"", "", "", "fire, ice, wind");
    form::step(c, -1);
    CHECK(c.draft == "wind");
    form::step(c, 1);
    CHECK(c.draft == "fire");
    form::Field i = mk("i", "int", "9", "1", "9");
    form::step(i, 1);
    CHECK(i.draft == "9");
    form::step(i, -1);
    CHECK(i.draft == "8");
    form::Field fl = mk("f", "float", "0.5", "0", "1");
    form::step(fl, 1);
    CHECK(fl.draft == "0.55");
    form::Field n = mk("n", "int", "3");
    form::step(n, -1);
    CHECK(n.draft == "2");
}

TEST_CASE("the track sets the nearest value; floats keep two decimals") {
    form::Field i = mk("i", "int", "5", "0", "10");
    form::set_fraction(i, 0.26);
    CHECK(i.draft == "3");
    form::Field f = mk("f", "float", "0.5", "0", "2");
    form::set_fraction(f, 0.333);
    CHECK(f.draft == "0.67");
    CHECK(form::fraction(f) == doctest::Approx(0.335));
}

TEST_CASE("drafts: dirty by value, discard, reset, changes") {
    form::Form fm;
    fm.fields = {mk("a", "float", "8.0", "1", "32"), mk("b", "bool", "false")};
    fm.check = allow;
    CHECK(fm.changes() == 0);
    fm.fields[0].draft = "8.00";   // the same number
    CHECK(fm.changes() == 0);
    fm.arrow(1, 1);
    CHECK(fm.changes() == 1);
    fm.arrow(0, 1);
    CHECK(fm.changes() == 2);
    CHECK(form::differs_from_default(fm.fields[0]));
    fm.to_default(0);
    CHECK(fm.changes() == 1);
    fm.discard();
    CHECK(fm.changes() == 0);
}

TEST_CASE("typing: a click gives the keyboard, the first character replaces, a bad value blocks") {
    form::Form fm;
    fm.fields = {mk("a", "int", "5", "1", "9")};
    fm.check = allow;
    fm.focus(0);
    fm.type('x');   // not a digit
    CHECK(fm.fields[0].draft == "5");
    fm.type('7');
    CHECK(fm.fields[0].draft == "7");
    CHECK_FALSE(fm.invalid());
    fm.type('7');
    CHECK(fm.fields[0].draft == "77");
    CHECK(fm.invalid());
    CHECK(fm.fields[0].reason == "too large");
    fm.back();
    CHECK_FALSE(fm.invalid());
    fm.back();
    CHECK(fm.fields[0].draft.empty());
    CHECK(fm.invalid());
    fm.to_default(0);
    CHECK_FALSE(fm.invalid());
    CHECK(fm.editing == -1);
}

TEST_CASE("text fields take UTF-8 and stop at the limit") {
    form::Form fm;
    fm.fields = {mk("s", "string", "\"ab\"")};
    fm.focus(0);
    fm.type(0xE9);
    CHECK(fm.fields[0].draft == "\xC3\xA9");
    fm.back();
    CHECK(fm.fields[0].draft.empty());
    for (int k = 0; k < 200; ++k) fm.type('z');
    CHECK(fm.fields[0].draft.size() == form::kTextMax);
}

TEST_CASE("rows: OPTIONS, ungrouped first, groups in order, notes") {
    form::Form fm;
    fm.fields = {mk("a", "bool", "true", "", "", "", "Look"), mk("b", "bool", "true", "", "", "", "", "", "short help"),
                 mk("c", "int", "1", "", "", "", "Feel", "", "", true), mk("d", "bool", "true", "", "", "", "Look")};
    fm.check = allow;
    auto r = form::rows(fm);
    REQUIRE(r.size() == 8);   // OPTIONS, b, note, LOOK, a, d, FEEL, c
    CHECK(r[0].text == "OPTIONS");
    CHECK(r[1].field == 1);
    CHECK(r[2].kind == form::Row::Note);
    CHECK(r[3].text == "LOOK");
    CHECK(r[4].field == 0);
    CHECK(r[5].field == 3);
    CHECK(r[6].text == "FEEL");
    fm.arrow(2, 1);
    r = form::rows(fm);
    REQUIRE(r.size() == 9);
    CHECK(r[8].tone == 2);   // "Takes effect at the next start."
    CHECK(r[8].field == 2);
    CHECK(form::rows(form::Form{}).empty());
}

TEST_CASE("clicks: arrows, track, value, reset") {
    form::Form fm;
    fm.fields = {mk("i", "int", "5", "0", "10"), mk("b", "bool", "true"), mk("s", "string", "\"x\"")};
    fm.check = allow;
    const form::Rect row = {0, 100, 600, 26};
    const form::Geometry g = form::geometry(row, form::Kind::Slider);
    CHECK(form::click(fm, 0, row, g.right.x + 5, 110, false));
    CHECK(fm.fields[0].draft == "6");
    CHECK(form::click(fm, 0, row, g.left.x + 5, 110, false));
    CHECK(fm.fields[0].draft == "5");
    CHECK(form::click(fm, 0, row, g.track.x + g.track.w * 0.9f, 110, false));
    CHECK(fm.fields[0].draft == "9");
    CHECK(form::click(fm, 0, row, g.track.x + 2, 110, true));   // Ctrl+click: the keyboard
    CHECK(fm.editing == 0);
    CHECK(form::click(fm, 0, row, g.reset.x + 5, 110, false));
    CHECK(fm.fields[0].draft == "5");
    CHECK_FALSE(form::click(fm, 0, row, row.x + 5, 110, false));   // the label
    const form::Geometry gb = form::geometry(row, form::Kind::Picker);
    CHECK_FALSE(form::click(fm, 1, row, gb.value.x + 5, 110, false));   // the middle of a picker does nothing
    CHECK(form::click(fm, 1, row, gb.right.x + 5, 110, false));
    CHECK(fm.fields[1].draft == "false");
    const form::Geometry gt = form::geometry(row, form::Kind::Text);
    CHECK(form::click(fm, 2, row, gt.value.x + 5, 110, false));
    CHECK(fm.editing == 2);
}

TEST_CASE("a tooltip takes at most half the lines and at least one") {
    CHECK(form::tip_lines(10, 2) == 2);
    CHECK(form::tip_lines(10, 9) == 5);
    CHECK(form::tip_lines(1, 3) == 1);
    CHECK(form::tip_lines(10, 0) == 1);
}

TEST_CASE("the hovered option moves above the tooltip only when the tooltip would cover it") {
    CHECK(form::tip_row_slot(2, 10, 3) == 2);   // lines 7 to 9 are covered
    CHECK(form::tip_row_slot(6, 10, 3) == 6);
    CHECK(form::tip_row_slot(7, 10, 3) == 6);
    CHECK(form::tip_row_slot(9, 10, 3) == 6);
    CHECK(form::tip_row_slot(0, 2, 2) == 0);    // a tiny column still shows the option
}
