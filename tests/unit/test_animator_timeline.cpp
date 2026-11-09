// Unit tests of the Animator's timeline (openspec add-ability-animation task 6.5): the rows, the time axis (snap, zoom, pan), the
// selection, and keys added, moved, deleted, copied and pasted in the clip's text. No window, no game.
#include "doctest.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "cliptext.h"
#include "model.h"
#include "timeline.h"

using namespace kanimator;

namespace {

anim::Clip clip_of(const std::string& text) {
    anim::ClipLoad c = anim::parse_clip(text, "test");
    for (const anim::ClipError& e : c.errors) MESSAGE(anim::format_error(e));
    REQUIRE(c.ok);
    return c.clip;
}

// Track 0 is a hand turned, a key to a line, with comments. Track 1 is the body moved, its keys on one line with no comma at the
// end. The hide and the marker are there to stay untouched.
const char* kHead =
    "# a clip\n"
    "duration = 1000   # ms\n"
    "\n"
    "[[track]]\n"
    "part = \"hand_r\"\n"
    "channel = \"rot\"\n"
    "mode = \"add\"\n"
    "keys = [\n"
    "  { t = 0.00, v = [0, 0, 0] },\n";
const char* kWindUp = "  { t = 0.35, v = [-110, 0, 10], ease = \"ease_out\" },   # wind-up\n";
const char* kStrike = "  { t = 0.50, v = [30, 0, 0],    ease = \"ease_in\" },    # strike\n";
const char* kLast = "  { t = 1.00, v = [0, 0, 0],     ease = \"smooth\" },\n";
const char* kBodyHead =
    "]\n"
    "\n"
    "[[track]]\n"
    "part = \"body\"\n"
    "channel = \"pos\"\n";
const char* kBodyKeys = "keys = [{ t = 0.0, v = [0, 0, 0] }, { t = 0.2, v = [0, 0, 2] }, { t = 1.0, v = [0, 0, 0] }]\n";
const char* kTail =
    "\n"
    "[[hide]]\n"
    "from = 0.0\n"
    "to = 0.2\n"
    "parts = [\"head\"]\n"
    "\n"
    "[[marker]]\n"
    "t = 0.50\n"
    "name = \"hit\"\n";

std::string clip_text() { return std::string(kHead) + kWindUp + kStrike + kLast + kBodyHead + kBodyKeys + kTail; }

std::string crlf(std::string s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

size_t count(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
    return n;
}

KeyEditResult ok(KeyEditResult r) {
    if (!r.ok) MESSAGE(r.error);
    REQUIRE(r.ok);
    return r;
}

const float kMs = 1000.0f;

}  // namespace

TEST_CASE("timeline: a row per part and channel, in the order of the parts") {
    const anim::Clip clip = clip_of(clip_text());
    const std::vector<TimelineRow> rows = timeline_rows(clip);
    REQUIRE(rows.size() == 2);
    CHECK(row_label(rows[0]) == "body.pos");
    CHECK(rows[0].edit_track == 1);
    CHECK(rows[0].keys.size() == 3);
    CHECK(row_label(rows[1]) == "hand_r.rot");
    CHECK(rows[1].edit_track == 0);
    REQUIRE(rows[1].keys.size() == 4);
    CHECK(rows[1].keys[1] == KeyRef{0, 1});
}

TEST_CASE("timeline: two tracks on one part and channel share a row, and the last one takes new keys") {
    const std::string text = clip_text() + "\n[[track]]\npart = \"weapon_r\"\nchannel = \"rot\"\nkeys = [{ t = 0.4, v = [5, 0, 0] }]\n";
    const anim::Clip clip = clip_of(text);
    const std::vector<TimelineRow> rows = timeline_rows(clip);
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].edit_track == 2);
    REQUIRE(rows[1].keys.size() == 5);
    CHECK(rows[1].keys[2] == KeyRef{2, 0});   // 0.4 lies between 0.35 and 0.5
    const KeyEditResult r = ok(add_key(text, anim::Part::HandR, anim::Channel::Rot, 700, kMs));
    REQUIRE(r.selection.size() == 1);
    CHECK(r.selection[0].track == 2);
}

TEST_CASE("timeline: the snap goes to the nearest step, stays in the clip, and reaches its end") {
    CHECK(snap_ms(354, 10, 1000) == doctest::Approx(350));
    CHECK(snap_ms(355.1f, 10, 1000) == doctest::Approx(360));
    CHECK(snap_ms(-5, 10, 1000) == doctest::Approx(0));
    CHECK(snap_ms(1004, 10, 1000) == doctest::Approx(1000));
    CHECK(snap_ms(354.4f, 0, 1000) == doctest::Approx(354.4f));   // no snap
    CHECK(snap_ms(1004, 0, 1000) == doctest::Approx(1000));
    CHECK(snap_ms(993, 10, 995) == doctest::Approx(995));         // the end is nearer than the grid
    CHECK(snap_ms(991, 10, 995) == doctest::Approx(990));
    CHECK(snap_ms(333, 25, 1000) == doctest::Approx(325));
}

TEST_CASE("timeline: the view maps time to pixels, zooms about the pointer and pans inside the clip") {
    TimelineView v;
    v.fit(100, 800, 1000);
    CHECK(v.px_per_ms() == doctest::Approx(0.8f));
    CHECK(v.x_of(0) == doctest::Approx(100));
    CHECK(v.x_of(500) == doctest::Approx(500));
    CHECK(v.ms_at(900) == doctest::Approx(1000));
    v.zoom_at(500, 2);
    CHECK(v.zoom == doctest::Approx(2));
    CHECK(v.ms_at(500) == doctest::Approx(500));   // the time under the pointer stays
    CHECK(v.start_ms == doctest::Approx(250));
    CHECK(v.visible_ms() == doctest::Approx(500));
    v.pan_px(-80);                                 // 1.6 px to a ms: 50 ms later
    CHECK(v.start_ms == doctest::Approx(300));
    v.pan_px(10000);
    CHECK(v.start_ms == doctest::Approx(0));
    v.pan_px(-10000);
    CHECK(v.start_ms == doctest::Approx(500));     // the end of the clip stays at the right edge
    v.zoom_at(100, 0.01f);
    CHECK(v.zoom == doctest::Approx(1));
    CHECK(v.start_ms == doctest::Approx(0));
    for (int i = 0; i < 20; ++i) v.zoom_at(300, 2);
    CHECK(v.zoom == doctest::Approx(kMaxTimelineZoom));
    CHECK(v.ms_at(300) == doctest::Approx(250).epsilon(0.001));
    // The ruler: labels 70 px apart or more.
    CHECK(tick_step_ms(0.8f, 70) == doctest::Approx(100));
    CHECK(tick_step_ms(1.6f, 70) == doctest::Approx(50));
    CHECK(tick_step_ms(51.2f, 70) == doctest::Approx(2));
    CHECK(tick_step_ms(200, 70) == doctest::Approx(1));
    CHECK(tick_step_ms(0, 70) > 0);
    // A clip with no length maps nothing and does not divide by zero.
    TimelineView none;
    none.fit(0, 500, 0);
    none.zoom_at(10, 2);
    none.pan_px(5);
    CHECK(none.ms_at(250) == 0);
    CHECK(std::isfinite(none.x_of(10)));
}

TEST_CASE("timeline: the key near a time, and the keys in a box") {
    const anim::Clip clip = clip_of(clip_text());
    const std::vector<TimelineRow> rows = timeline_rows(clip);
    CHECK(key_near(clip, rows[1], 355, 8, kMs) == KeyRef{0, 1});
    CHECK(key_near(clip, rows[1], 420, 8, kMs) == KeyRef{-1, -1});
    CHECK(key_near(clip, rows[1], 430, 80, kMs) == KeyRef{0, 2});   // the nearer of two in reach

    const Selection both = keys_in_box(clip, rows, 0, 1, 100, 600, kMs);
    REQUIRE(both.size() == 3);
    CHECK(both[0] == KeyRef{0, 1});
    CHECK(both[1] == KeyRef{0, 2});
    CHECK(both[2] == KeyRef{1, 1});
    CHECK(keys_in_box(clip, rows, 1, 0, 600, 100, kMs) == both);    // dragged from the other corner
    CHECK(keys_in_box(clip, rows, 1, 1, 100, 600, kMs).size() == 2);
    CHECK(keys_in_box(clip, rows, -3, 9, 0, 1000, kMs).size() == 7);
    CHECK(keys_in_box(clip, rows, 0, 1, 600, 900, kMs).empty());
}

TEST_CASE("timeline: the selection keeps each key once, in order") {
    Selection sel;
    select_add(sel, {1, 2});
    select_add(sel, {0, 3});
    select_add(sel, {1, 2});
    REQUIRE(sel.size() == 2);
    CHECK(sel[0] == KeyRef{0, 3});
    CHECK(selected(sel, {1, 2}));
    select_toggle(sel, {1, 2});
    CHECK(!selected(sel, {1, 2}));
    select_toggle(sel, {0, 1});
    CHECK(sel[0] == KeyRef{0, 1});
}

// The spec's scenario "Move a key": the key at 350 ms dragged to 400 ms.
TEST_CASE("timeline: a key moved from 350 ms to 400 ms changes its t and nothing else") {
    const std::string text = clip_text();
    const KeyEditResult r = ok(move_keys(text, {{0, 1}}, 50, kMs));
    const std::string moved = "  { t = 0.4, v = [-110, 0, 10], ease = \"ease_out\" },   # wind-up\n";
    CHECK(r.text == std::string(kHead) + moved + kStrike + kLast + kBodyHead + kBodyKeys + kTail);
    CHECK(r.clip.tracks[0].keys[1].t == doctest::Approx(0.4f));
    CHECK(r.clip.tracks[0].keys[1].v[0] == doctest::Approx(-110));
    CHECK(r.clip.tracks[0].keys[1].ease == anim::Ease::EaseOut);
    REQUIRE(r.selection.size() == 1);
    CHECK(r.selection[0] == KeyRef{0, 1});
    CHECK(r.replaced == 0);

    // The view shows the new motion: the hand is where the key puts it at 400 ms, and was not before.
    float before[3], after[3];
    anim::sample_track(clip_of(text).tracks[0], 0.4f, before);
    anim::sample_track(r.clip.tracks[0], 0.4f, after);
    CHECK(after[0] == doctest::Approx(-110));
    CHECK(before[0] != doctest::Approx(-110));

    // The unsaved mark, and one undo step.
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "kyubu_timeline_move.anim.toml";
    {
        std::ofstream f(file, std::ios::binary);
        f << text;
    }
    ClipDoc doc;
    std::string err;
    REQUIRE(doc.open(file.string(), err));
    CHECK(!doc.dirty());
    doc.set_text(r.text);
    CHECK(doc.dirty());
    CHECK(doc.undo_depth() == 1);
    REQUIRE(doc.undo());
    CHECK(doc.text() == text);
    CHECK(!doc.dirty());
    std::filesystem::remove(file);
}

TEST_CASE("timeline: a key on a shared line moves in place too") {
    const KeyEditResult r = ok(move_keys(clip_text(), {{1, 1}}, 300, kMs));
    CHECK(count(r.text, "keys = [{ t = 0.0, v = [0, 0, 0] }, { t = 0.5, v = [0, 0, 2] }, { t = 1.0, v = [0, 0, 0] }]\n") == 1);
    CHECK(count(r.text, kWindUp) == 1);
}

TEST_CASE("timeline: a key that passes another moves to its place in the list") {
    const KeyEditResult r = ok(move_keys(clip_text(), {{0, 1}}, 200, kMs));
    const anim::Track& tr = r.clip.tracks[0];
    REQUIRE(tr.keys.size() == 4);
    CHECK(tr.keys[1].t == doctest::Approx(0.5f));
    CHECK(tr.keys[1].v[0] == doctest::Approx(30));
    CHECK(tr.keys[2].t == doctest::Approx(0.55f));
    CHECK(tr.keys[2].v[0] == doctest::Approx(-110));
    CHECK(tr.keys[2].v[2] == doctest::Approx(10));
    CHECK(tr.keys[2].ease == anim::Ease::EaseOut);
    REQUIRE(r.selection.size() == 1);
    CHECK(r.selection[0] == KeyRef{0, 2});
    const std::string moved = "  { t = 0.55, v = [-110, 0, 10], ease = \"ease_out\" },\n";
    CHECK(r.text == std::string(kHead) + kStrike + moved + kLast + kBodyHead + kBodyKeys + kTail);

    // And back the other way, before the first key's neighbour.
    const KeyEditResult back = ok(move_keys(r.text, r.selection, -300, kMs));
    REQUIRE(back.selection.size() == 1);
    CHECK(back.selection[0] == KeyRef{0, 1});
    CHECK(back.clip.tracks[0].keys[1].t == doctest::Approx(0.25f));
    CHECK(back.clip.tracks[0].keys[2].t == doctest::Approx(0.5f));
}

TEST_CASE("timeline: a moved key replaces the key it lands on") {
    const KeyEditResult r = ok(move_keys(clip_text(), {{0, 1}}, 150, kMs));
    const anim::Track& tr = r.clip.tracks[0];
    REQUIRE(tr.keys.size() == 3);
    CHECK(r.replaced == 1);
    CHECK(tr.keys[1].t == doctest::Approx(0.5f));
    CHECK(tr.keys[1].v[0] == doctest::Approx(-110));
    REQUIRE(r.selection.size() == 1);
    CHECK(r.selection[0] == KeyRef{0, 1});
    CHECK(count(r.text, "strike") == 0);
    CHECK(count(r.text, "wind-up") == 1);
}

TEST_CASE("timeline: a group keeps its distances and stops at the ends of the clip") {
    const anim::Clip clip = clip_of(clip_text());
    const Selection two = {{0, 1}, {0, 2}};
    CHECK(clamp_move_ms(clip, two, 900, kMs) == doctest::Approx(500));
    CHECK(clamp_move_ms(clip, two, -900, kMs) == doctest::Approx(-350));
    CHECK(clamp_move_ms(clip, two, 120, kMs) == doctest::Approx(120));
    CHECK(clamp_move_ms(clip, {}, 120, kMs) == 0);

    const KeyEditResult r = ok(move_keys(clip_text(), two, 900, kMs));
    const anim::Track& tr = r.clip.tracks[0];
    REQUIRE(tr.keys.size() == 3);   // the key at the end was landed on
    CHECK(r.replaced == 1);
    CHECK(tr.keys[1].t == doctest::Approx(0.85f));
    CHECK(tr.keys[2].t == doctest::Approx(1.0f));
    CHECK(tr.keys[2].v[0] == doctest::Approx(30));
    CHECK(r.selection == Selection{{0, 1}, {0, 2}});

    // Keys of two tracks move together.
    const KeyEditResult both = ok(move_keys(clip_text(), {{0, 1}, {1, 1}}, 100, kMs));
    CHECK(both.clip.tracks[0].keys[1].t == doctest::Approx(0.45f));
    CHECK(both.clip.tracks[1].keys[1].t == doctest::Approx(0.3f));
    CHECK(both.selection == Selection{{0, 1}, {1, 1}});
}

TEST_CASE("timeline: a move of nothing, of no key and of a key that is not there") {
    const std::string text = clip_text();
    const KeyEditResult same = ok(move_keys(text, {{0, 1}}, 0, kMs));
    CHECK(same.text == text);
    CHECK(same.selection == Selection{{0, 1}});
    const KeyEditResult held = ok(move_keys(text, {{0, 0}}, -200, kMs));   // the key at 0 cannot go lower
    CHECK(held.text == text);
    CHECK(!move_keys(text, {}, 50, kMs).ok);
    CHECK(!move_keys(text, {{0, 9}}, 50, kMs).ok);
    CHECK(!move_keys(text, {{5, 0}}, 50, kMs).ok);
    CHECK(!move_keys(text, {{0, 1}}, 50, 0).ok);
    const KeyEditResult bad = move_keys("duration = ", {{0, 1}}, 50, kMs);
    CHECK(!bad.ok);
    CHECK(bad.text == "duration = ");
}

TEST_CASE("timeline: a deleted key takes its line, or its comma") {
    const std::string text = clip_text();
    const KeyEditResult line = ok(delete_keys(text, {{0, 2}}));
    CHECK(line.text == std::string(kHead) + kWindUp + kLast + kBodyHead + kBodyKeys + kTail);
    CHECK(line.selection.empty());

    const KeyEditResult mid = ok(delete_keys(text, {{1, 1}}));
    CHECK(count(mid.text, "keys = [{ t = 0.0, v = [0, 0, 0] }, { t = 1.0, v = [0, 0, 0] }]\n") == 1);
    const KeyEditResult first = ok(delete_keys(text, {{1, 0}}));
    CHECK(count(first.text, "keys = [{ t = 0.2, v = [0, 0, 2] }, { t = 1.0, v = [0, 0, 0] }]\n") == 1);
    const KeyEditResult last = ok(delete_keys(text, {{1, 2}}));
    CHECK(count(last.text, "keys = [{ t = 0.0, v = [0, 0, 0] }, { t = 0.2, v = [0, 0, 2] }]\n") == 1);

    const KeyEditResult many = ok(delete_keys(text, {{0, 1}, {0, 2}, {1, 0}, {1, 1}}));
    CHECK(many.clip.tracks[0].keys.size() == 2);
    CHECK(many.clip.tracks[1].keys.size() == 1);
    CHECK(many.text == std::string(kHead) + kLast + kBodyHead + "keys = [{ t = 1.0, v = [0, 0, 0] }]\n" + kTail);
}

TEST_CASE("timeline: a track keeps one key") {
    const std::string text = clip_text();
    const KeyEditResult r = delete_keys(text, {{1, 0}, {1, 1}, {1, 2}});
    CHECK(!r.ok);
    CHECK(r.text == text);
    CHECK(r.error.find("body.pos") != std::string::npos);
    CHECK(!delete_keys(text, {}).ok);
    CHECK(!delete_keys(text, {{0, 7}}).ok);
}

TEST_CASE("timeline: a key added on a row has the track's value there") {
    const std::string text = clip_text();
    const KeyEditResult r = ok(add_key(text, anim::Part::HandR, anim::Channel::Rot, 250, kMs));
    const anim::Track& tr = r.clip.tracks[0];
    REQUIRE(tr.keys.size() == 5);
    REQUIRE(r.selection.size() == 1);
    CHECK(r.selection[0] == KeyRef{0, 1});
    CHECK(tr.keys[1].t == doctest::Approx(0.25f));
    CHECK(tr.keys[1].ease == anim::Ease::EaseOut);   // the ease of the key after it
    float was[3];
    anim::sample_track(clip_of(text).tracks[0], 0.25f, was);
    for (int a = 0; a < 3; ++a) CHECK(tr.keys[1].v[a] == doctest::Approx(was[a]).epsilon(0.001));
    CHECK(was[0] < -1);   // a value worth the check
    CHECK(count(r.text, kWindUp) == 1);
    CHECK(count(r.text, "\n  { t = 0.25, v = [") == 1);   // a line of its own, with the indent of its neighbour

    // On a shared line the key goes in line.
    const KeyEditResult in_line = ok(add_key(text, anim::Part::Body, anim::Channel::Pos, 600, kMs));
    CHECK(count(in_line.text, "{ t = 0.2, v = [0, 0, 2] }, { t = 0.6, v = [0, 0, 1] }, { t = 1.0, v = [0, 0, 0] }]\n") == 1);
    CHECK(in_line.selection == Selection{{1, 2}});

    // A key that is there already is selected; nothing changes.
    const KeyEditResult there = ok(add_key(text, anim::Part::HandR, anim::Channel::Rot, 350.2f, kMs));
    CHECK(there.text == text);
    CHECK(there.selection == Selection{{0, 1}});

    // A part with no track has no row to add on.
    const KeyEditResult none = add_key(text, anim::Part::Head, anim::Channel::Rot, 250, kMs);
    CHECK(!none.ok);
    CHECK(none.error.find("head.rot") != std::string::npos);
    CHECK(!add_key(text, anim::Part::HandR, anim::Channel::Rot, 250, 0).ok);
}

TEST_CASE("timeline: copied keys are pasted at the scrub time, each on its own row") {
    const std::string text = clip_text();
    const anim::Clip clip = clip_of(text);
    const KeyClipboard cb = copy_keys(clip, {{0, 1}, {0, 2}, {1, 1}}, kMs);
    REQUIRE(cb.keys.size() == 3);
    CHECK(cb.keys[0].part == anim::Part::Body);          // the earliest: 200 ms
    CHECK(cb.keys[0].offset_ms == doctest::Approx(0));
    CHECK(cb.keys[1].offset_ms == doctest::Approx(150));
    CHECK(cb.keys[2].offset_ms == doctest::Approx(300));
    CHECK(cb.keys[1].ease == anim::Ease::EaseOut);
    CHECK(copy_keys(clip, {}, kMs).empty());
    CHECK(copy_keys(clip, {{0, 9}}, kMs).empty());

    const KeyEditResult r = ok(paste_keys(text, cb, 600, kMs));
    REQUIRE(r.clip.tracks[0].keys.size() == 6);
    REQUIRE(r.clip.tracks[1].keys.size() == 4);
    CHECK(r.clip.tracks[1].keys[2].t == doctest::Approx(0.6f));
    CHECK(r.clip.tracks[1].keys[2].v[2] == doctest::Approx(2));
    CHECK(r.clip.tracks[0].keys[3].t == doctest::Approx(0.75f));
    CHECK(r.clip.tracks[0].keys[3].v[0] == doctest::Approx(-110));
    CHECK(r.clip.tracks[0].keys[3].ease == anim::Ease::EaseOut);
    CHECK(r.clip.tracks[0].keys[4].t == doctest::Approx(0.9f));
    CHECK(r.clip.tracks[0].keys[4].v[0] == doctest::Approx(30));
    CHECK(r.selection == Selection{{0, 3}, {0, 4}, {1, 2}});
    CHECK(r.dropped == 0);
    CHECK(r.replaced == 0);
    CHECK(count(r.text, kWindUp) == 1);   // the keys copied from are as they were
    CHECK(count(r.text, kStrike) == 1);
}

TEST_CASE("timeline: a pasted key past the end is dropped, and one on a key replaces it") {
    const std::string text = clip_text();
    const anim::Clip clip = clip_of(text);
    const KeyClipboard cb = copy_keys(clip, {{0, 1}, {0, 2}, {1, 1}}, kMs);
    const KeyEditResult one = ok(paste_keys(text, cb, 800, kMs));
    CHECK(one.dropped == 1);
    CHECK(one.selection.size() == 2);
    const KeyEditResult two = ok(paste_keys(text, cb, 950, kMs));
    CHECK(two.dropped == 2);
    const KeyEditResult all = paste_keys(text, cb, 1200, kMs);
    CHECK(!all.ok);
    CHECK(all.text == text);
    CHECK(!paste_keys(text, KeyClipboard{}, 100, kMs).ok);

    // The end of the clip is in the clip.
    const KeyEditResult end = ok(paste_keys(text, copy_keys(clip, {{0, 1}}, kMs), 1000, kMs));
    CHECK(end.replaced == 1);
    CHECK(end.clip.tracks[0].keys[3].v[0] == doctest::Approx(-110));

    const KeyEditResult on = ok(paste_keys(text, copy_keys(clip, {{0, 1}}, kMs), 500, kMs));
    CHECK(on.replaced == 1);
    REQUIRE(on.clip.tracks[0].keys.size() == 4);
    CHECK(on.clip.tracks[0].keys[2].v[0] == doctest::Approx(-110));
    CHECK(on.clip.tracks[0].keys[2].ease == anim::Ease::EaseOut);
    CHECK(on.selection == Selection{{0, 2}});
    CHECK(count(on.text, "  { t = 0.5, v = [-110, 0, 10], ease = \"ease_out\" },    # strike\n") == 1);
}

TEST_CASE("timeline: a paste into a clip with no such track makes one, and keeps the timing in a longer clip") {
    const anim::Clip clip = clip_of(clip_text());
    const KeyClipboard cb = copy_keys(clip, {{0, 1}, {0, 2}}, kMs);
    const std::string other = "duration = 2000\n\n[[track]]\npart = \"body\"\nchannel = \"pos\"\nkeys = [{ t = 0.0, v = [0, 0, 0] }]\n";
    const KeyEditResult r = ok(paste_keys(other, cb, 1000, 2000));
    CHECK(r.made_tracks == 1);
    REQUIRE(r.clip.tracks.size() == 2);
    const anim::Track& tr = r.clip.tracks[1];
    CHECK(tr.part == anim::Part::HandR);
    CHECK(tr.channel == anim::Channel::Rot);
    CHECK(tr.mode == anim::TrackMode::Add);
    REQUIRE(tr.keys.size() == 2);
    CHECK(tr.keys[0].t == doctest::Approx(0.5f));
    CHECK(tr.keys[1].t == doctest::Approx(0.575f));   // 150 ms later, as copied
    CHECK(r.selection == Selection{{1, 0}, {1, 1}});
    CHECK(r.text.compare(0, other.size(), other) == 0);   // the text before it is as it was

    // A file that writes its tracks as a list cannot take a `[[track]]` table.
    const std::string list = "duration = 1000\ntrack = [{ part = \"body\", channel = \"pos\", keys = [{ t = 0.0, v = [0, 0, 0] }] }]\n";
    clip_of(list);
    const KeyEditResult refused = paste_keys(list, cb, 100, kMs);
    CHECK(!refused.ok);
    CHECK(refused.text == list);
}

TEST_CASE("timeline: a file with CRLF line ends keeps them") {
    const std::string text = crlf(clip_text());
    clip_of(text);
    auto all_crlf = [](const std::string& s) { return count(s, "\n") == count(s, "\r\n"); };
    const KeyEditResult moved = ok(move_keys(text, {{0, 1}}, 200, kMs));   // out and in again
    CHECK(all_crlf(moved.text));
    CHECK(moved.clip.tracks[0].keys[2].t == doctest::Approx(0.55f));
    CHECK(count(moved.text, "\n") == count(text, "\n"));
    const KeyEditResult gone = ok(delete_keys(text, {{0, 2}}));
    CHECK(all_crlf(gone.text));
    CHECK(count(gone.text, "\n") == count(text, "\n") - 1);
    const KeyEditResult added = ok(add_key(text, anim::Part::HandR, anim::Channel::Rot, 700, kMs));
    CHECK(all_crlf(added.text));
    const anim::Clip clip = clip_of(text);
    const KeyEditResult pasted = ok(paste_keys("duration = 1000\r\n", copy_keys(clip, {{0, 1}}, kMs), 100, kMs));   // a new track
    CHECK(pasted.made_tracks == 1);
    CHECK(all_crlf(pasted.text));
}

TEST_CASE("timeline: keys written as [[track.keys]] tables move, and are not deleted or added to") {
    const std::string text =
        "duration = 1000\n\n[[track]]\npart = \"head\"\nchannel = \"rot\"\n\n[[track.keys]]\nt = 0\nv = [0, 0, 0]\n\n"
        "[[track.keys]]\nt = 0.5   # the nod\nv = [10, 0, 0]\n";
    clip_of(text);
    const KeyEditResult r = ok(move_keys(text, {{0, 1}}, 100, kMs));
    CHECK(count(r.text, "t = 0.6   # the nod\n") == 1);
    const KeyEditResult first = ok(move_keys(text, {{0, 0}}, 100, kMs));   // a whole number becomes a time
    CHECK(count(first.text, "t = 0.1\n") == 1);
    const KeyEditResult del = delete_keys(text, {{0, 1}});
    CHECK(!del.ok);
    CHECK(del.error.find("[[track.keys]]") != std::string::npos);
    CHECK(!add_key(text, anim::Part::Head, anim::Channel::Rot, 250, kMs).ok);
}

TEST_CASE("cliptext: one key replaced, and a key removed from the end of a list with a comma") {
    const std::string text = clip_text();
    std::string out, err;
    anim::Key k;
    k.t = 0.3f;
    k.v[1] = 12.5f;
    k.ease = anim::Ease::Hold;
    REQUIRE(replace_key(text, 1, 1, k, out, err));
    CHECK(count(out, "keys = [{ t = 0.0, v = [0, 0, 0] }, { t = 0.3, v = [0, 12.5, 0], ease = \"hold\" }, { t = 1.0, v = [0, 0, 0] }]\n") == 1);
    REQUIRE(remove_key(text, 0, 3, out, err));
    CHECK(out == std::string(kHead) + kWindUp + kStrike + kBodyHead + kBodyKeys + kTail);
    CHECK(!remove_key(text, 0, 4, out, err));
    CHECK(!remove_key(text, 7, 0, out, err));
    CHECK(!replace_key(text, 0, -1, k, out, err));
    CHECK(!set_key_times(text, {{0, 1, 0.4f}, {0, 1, 0.5f}}, out, err));   // the same key twice
    CHECK(text_eol(text) == "\n");
    CHECK(text_eol(crlf(text)) == "\r\n");
    CHECK(fmt_time(1) == "1.0");
    CHECK(fmt_time(0.016666f) == "0.01667");
    CHECK(fmt_value(-0.0004) == "0");
}

TEST_CASE("timeline: a generator track has no keys, and a key is not added into it") {
    // covers: animation-editor/Procedural tracks (a track of a generator shows in the timeline as a row with no keys)
    const std::string text = R"(duration = 1000
[[track]]
part = "hand_r"
channel = "rot"
osc = { amp = [5, 0, 0], freq = 2 }
)";
    anim::ClipLoad cl = anim::parse_clip(text, "gen");
    REQUIRE(cl.ok);
    const std::vector<TimelineRow> rows = timeline_rows(cl.clip);
    for (const TimelineRow& row : rows) CHECK(row.keys.empty());
    KeyEditResult r = add_key(text, anim::Part::HandR, anim::Channel::Rot, 500, 1000);
    CHECK_FALSE(r.ok);
    CHECK(r.text == text);
}
