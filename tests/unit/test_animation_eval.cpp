// Unit tests of the pure animation evaluator (openspec add-ability-animation task 3.1). No game.
#include "doctest.h"

#include <limits>
#include <string>

#include "animation_eval.h"

using namespace kyubu::game::anim;

namespace {

Key key(float t, float x, float y = 0, float z = 0, Ease e = Ease::Linear) {
    Key k;
    k.t = t;
    k.v[0] = x; k.v[1] = y; k.v[2] = z;
    k.ease = e;
    return k;
}

Track track(Part p, Channel ch, TrackMode m, std::vector<Key> keys) {
    Track t;
    t.part = p; t.channel = ch; t.mode = m; t.keys = std::move(keys);
    return t;
}

// A 1000 ms clip with one hand_r rotation track: 0 at 0, 100 at 0.5, 0 at 1.
Clip simple_clip(Ease arrive = Ease::Linear, TrackMode m = TrackMode::Add) {
    Clip c;
    c.duration_ms = 1000;
    c.tracks.push_back(track(Part::HandR, Channel::Rot, m,
                             {key(0, 0), key(0.5f, 100, 0, 0, arrive), key(1, 0)}));
    return c;
}

Timing at(float elapsed, float duration = 1000, float run = 0) {
    Timing t;
    t.elapsed_ms = elapsed; t.duration_ms = duration; t.run_ms = run;
    return t;
}

float rot_x(const Pose& p, Part part = Part::HandR) { return p.parts[static_cast<int>(part)].rot[0]; }

}  // namespace

TEST_CASE("anim: easing curves") {
    for (Ease e : {Ease::Linear, Ease::Smooth, Ease::EaseIn, Ease::EaseOut}) {
        CHECK(apply_ease(e, 0) == doctest::Approx(0));
        CHECK(apply_ease(e, 1) == doctest::Approx(1));
    }
    CHECK(apply_ease(Ease::Linear, 0.25f) == doctest::Approx(0.25f));
    CHECK(apply_ease(Ease::Smooth, 0.5f) == doctest::Approx(0.5f));
    CHECK(apply_ease(Ease::Smooth, 0.25f) == doctest::Approx(0.15625f));
    CHECK(apply_ease(Ease::EaseIn, 0.5f) == doctest::Approx(0.25f));
    CHECK(apply_ease(Ease::EaseOut, 0.5f) == doctest::Approx(0.75f));
    CHECK(apply_ease(Ease::Hold, 0.99f) == 0.0f);
    CHECK(apply_ease(Ease::Hold, 1) == 1.0f);
    // out of range and NaN clamp
    CHECK(apply_ease(Ease::Linear, -3) == 0.0f);
    CHECK(apply_ease(Ease::Linear, 7) == 1.0f);
    CHECK(apply_ease(Ease::Smooth, std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

TEST_CASE("anim: names round trip") {
    for (int i = 0; i < kPartCount; ++i) {
        Part back;
        REQUIRE(part_from_name(part_name(static_cast<Part>(i)), back));
        CHECK(static_cast<int>(back) == i);
    }
    Part p;
    CHECK_FALSE(part_from_name("elbow", p));
    // A weapon follows its hand: its name is another name of the hand.
    REQUIRE(part_from_name("weapon_r", p));
    CHECK(p == Part::HandR);
    REQUIRE(part_from_name("weapon_l", p));
    CHECK(p == Part::HandL);
    Channel ch;
    CHECK(channel_from_name("pos", ch));
    CHECK(ch == Channel::Pos);
    CHECK_FALSE(channel_from_name("scale", ch));
    for (Ease e : {Ease::Linear, Ease::Smooth, Ease::EaseIn, Ease::EaseOut, Ease::Hold}) {
        Ease back;
        REQUIRE(ease_from_name(ease_name(e), back));
        CHECK(back == e);
    }
    Ease e;
    CHECK_FALSE(ease_from_name("bounce", e));
    uint32_t bit = 0;
    CHECK(hide_group_from_name("hair", bit));
    CHECK(bit == kHideHair);
    CHECK(std::string(hide_group_name(kHideWeapons)) == "weapons");
    CHECK_FALSE(hide_group_from_name("legs", bit));
}

TEST_CASE("anim: sampling a track, the ease belongs to the arriving key") {
    Track linear = simple_clip().tracks[0];
    float v[3];
    sample_track(linear, 0.25f, v);
    CHECK(v[0] == doctest::Approx(50));
    sample_track(linear, 0.5f, v);
    CHECK(v[0] == doctest::Approx(100));
    sample_track(linear, 0.75f, v);
    CHECK(v[0] == doctest::Approx(50));

    Track eo = simple_clip(Ease::EaseOut).tracks[0];   // the first segment decelerates into the key at 0.5
    sample_track(eo, 0.25f, v);
    CHECK(v[0] == doctest::Approx(75));
    sample_track(eo, 0.75f, v);                        // the second segment arrives at a linear key
    CHECK(v[0] == doctest::Approx(50));

    Track hold = simple_clip(Ease::Hold).tracks[0];    // holds 0 until the key at 0.5, then jumps
    sample_track(hold, 0.49f, v);
    CHECK(v[0] == doctest::Approx(0));
    sample_track(hold, 0.5f, v);
    CHECK(v[0] == doctest::Approx(100));
}

TEST_CASE("anim: sampling edges") {
    float v[3];
    Track empty;
    sample_track(empty, 0.5f, v);
    CHECK(v[0] == 0);
    CHECK(v[2] == 0);

    Track late = track(Part::Head, Channel::Rot, TrackMode::Add, {key(0.4f, 10, 20, 30), key(0.6f, 20, 40, 60)});
    sample_track(late, 0, v);            // before the first key: its value
    CHECK(v[0] == doctest::Approx(10));
    sample_track(late, 1, v);            // after the last key: its value
    CHECK(v[2] == doctest::Approx(60));
    sample_track(late, 0.5f, v);         // each axis alone
    CHECK(v[0] == doctest::Approx(15));
    CHECK(v[1] == doctest::Approx(30));
    CHECK(v[2] == doctest::Approx(45));

    Track one = track(Part::Head, Channel::Pos, TrackMode::Set, {key(0.5f, 7)});
    sample_track(one, 0, v);
    CHECK(v[0] == doctest::Approx(7));
    sample_track(one, 1, v);
    CHECK(v[0] == doctest::Approx(7));

    // two keys at the same t make a step: the later wins from that t on, and nothing divides by zero
    Track step = track(Part::Head, Channel::Pos, TrackMode::Set, {key(0, 0), key(0.5f, 1), key(0.5f, 5), key(1, 5)});
    sample_track(step, 0.4999f, v);
    CHECK(v[0] == doctest::Approx(1).epsilon(0.01));
    sample_track(step, 0.5f, v);
    CHECK(v[0] == doctest::Approx(5));

    sample_track(late, std::numeric_limits<float>::quiet_NaN(), v);   // NaN time reads as 0
    CHECK(v[0] == doctest::Approx(10));
}

TEST_CASE("anim: add and set over a fake base pose") {
    Pose base;
    base.parts[static_cast<int>(Part::HandR)].rot[0] = 40;
    base.parts[static_cast<int>(Part::HandR)].rot[1] = 5;
    base.parts[static_cast<int>(Part::Head)].rot[0] = 9;

    SUBCASE("add keeps the game's pose underneath") {
        Clip c = simple_clip(Ease::Linear, TrackMode::Add);
        Pose p = base;
        Applied r = apply_clip(c, at(500), p);
        REQUIRE(r.ok);
        CHECK(rot_x(p) == doctest::Approx(140));
        CHECK(p.parts[static_cast<int>(Part::HandR)].rot[1] == doctest::Approx(5));   // other axes: the sampled 0 added
        CHECK(rot_x(p, Part::Head) == doctest::Approx(9));                              // an untouched part
        CHECK(r.rot_parts == (1u << static_cast<int>(Part::HandR)));
        CHECK(r.pos_parts == 0);
    }
    SUBCASE("set replaces the vector, on every axis") {
        Clip c = simple_clip(Ease::Linear, TrackMode::Set);
        Pose p = base;
        apply_clip(c, at(500), p);
        CHECK(rot_x(p) == doctest::Approx(100));
        CHECK(p.parts[static_cast<int>(Part::HandR)].rot[1] == doctest::Approx(0));
    }
    SUBCASE("pos and rot are separate vectors") {
        Clip c;
        c.duration_ms = 100;
        c.tracks.push_back(track(Part::Root, Channel::Pos, TrackMode::Add, {key(0, 0, 2, 0), key(1, 0, 2, 0)}));
        Pose p = base;
        Applied r = apply_clip(c, at(50, 100), p);
        CHECK(p.parts[0].pos[1] == doctest::Approx(2));
        CHECK(p.parts[0].rot[1] == doctest::Approx(0));
        CHECK(r.pos_parts == 1u);
        CHECK(r.rot_parts == 0);
    }
    SUBCASE("two tracks on one part and channel stack in order") {
        Clip c = simple_clip(Ease::Linear, TrackMode::Add);
        c.tracks.push_back(track(Part::HandR, Channel::Rot, TrackMode::Add, {key(0, 1), key(1, 1)}));
        Pose p = base;
        apply_clip(c, at(500), p);
        CHECK(rot_x(p) == doctest::Approx(141));
    }
    SUBCASE("no tracks or no duration leave the pose alone") {
        Clip c;
        c.duration_ms = 1000;
        Pose p = base;
        CHECK_FALSE(apply_clip(c, at(500), p).ok);
        Clip d = simple_clip();
        CHECK_FALSE(apply_clip(d, at(500, 0), p).ok);
        CHECK_FALSE(apply_clip(d, at(500, -5), p).ok);
        CHECK(rot_x(p) == doctest::Approx(40));
    }
}

TEST_CASE("anim: blend in and blend out") {
    Clip c = simple_clip(Ease::Linear, TrackMode::Add);
    c.blend_in_ms = 100;
    c.blend_out_ms = 200;
    Pose base;

    CHECK(blend_weight(c, at(0)) == doctest::Approx(0));
    CHECK(blend_weight(c, at(50)) == doctest::Approx(0.5f));
    CHECK(blend_weight(c, at(100)) == doctest::Approx(1));
    CHECK(blend_weight(c, at(500)) == doctest::Approx(1));
    CHECK(blend_weight(c, at(900)) == doctest::Approx(0.5f));      // 100 ms of 200 left
    CHECK(blend_weight(c, at(1000)) == doctest::Approx(0));
    CHECK(blend_weight(c, at(1500)) == doctest::Approx(0));        // held past the end: faded out

    // the clip starts and ends on the game's pose: no pop
    Pose p = base;
    p.parts[static_cast<int>(Part::HandR)].rot[0] = 30;
    apply_clip(c, at(0), p);
    CHECK(rot_x(p) == doctest::Approx(30));

    // a weight halfway moves the pose halfway to the clip's
    Clip s = simple_clip(Ease::Linear, TrackMode::Set);
    s.blend_in_ms = 100;
    Pose q;
    q.parts[static_cast<int>(Part::HandR)].rot[0] = 40;
    apply_clip(s, at(50), q);        // the sample at 50 ms is 10; halfway between 40 and 10
    CHECK(rot_x(q) == doctest::Approx(25));

    // blend_out counts back from the end of the run, which may be later than the clip's end
    Clip h = simple_clip();
    h.blend_out_ms = 100;
    CHECK(blend_weight(h, at(1000, 1000, 1500)) == doctest::Approx(1));    // the ability goes on: the pose holds
    CHECK(blend_weight(h, at(1450, 1000, 1500)) == doctest::Approx(0.5f));
    CHECK(blend_weight(h, at(1500, 1000, 1500)) == doctest::Approx(0));

    // no ramps: weight is 1 all along, even past the end
    Clip n = simple_clip();
    CHECK(blend_weight(n, at(0)) == doctest::Approx(1));
    CHECK(blend_weight(n, at(5000)) == doctest::Approx(1));
}

TEST_CASE("anim: end hold and loop") {
    Pose base;
    SUBCASE("hold stays at the last pose") {
        Clip c = simple_clip();
        c.tracks[0].keys.back().v[0] = 60;   // the last key is not zero, so the hold is visible
        Pose p = base;
        apply_clip(c, at(1000), p);
        CHECK(rot_x(p) == doctest::Approx(60));
        Pose p2 = base;
        apply_clip(c, at(99999), p2);
        CHECK(rot_x(p2) == doctest::Approx(60));
    }
    SUBCASE("loop wraps") {
        Clip c = simple_clip();
        c.end = End::Loop;
        Pose p = base;
        apply_clip(c, at(1250), p);          // the same as 250
        CHECK(rot_x(p) == doctest::Approx(50));
        Pose p2 = base;
        apply_clip(c, at(3500), p2);         // the same as 500
        CHECK(rot_x(p2) == doctest::Approx(100));
        Pose p3 = base;
        apply_clip(c, at(2000), p3);         // a whole lap is the start again
        CHECK(rot_x(p3) == doctest::Approx(0));
    }
    SUBCASE("loop does not blend out unless the run end is given") {
        Clip c = simple_clip();
        c.end = End::Loop;
        c.blend_out_ms = 100;
        CHECK(blend_weight(c, at(990)) == doctest::Approx(1));
        CHECK(blend_weight(c, at(4950, 1000, 5000)) == doctest::Approx(0.5f));
    }
    SUBCASE("a negative or NaN time is the start") {
        Clip c = simple_clip();
        Pose p = base;
        apply_clip(c, at(-40), p);
        CHECK(rot_x(p) == doctest::Approx(0));
        Pose p2 = base;
        apply_clip(c, at(std::numeric_limits<float>::quiet_NaN()), p2);
        CHECK(rot_x(p2) == doctest::Approx(0));
    }
}

TEST_CASE("anim: the duration may follow the ability") {
    Clip c;
    c.duration_ms = 700;
    CHECK(resolve_duration(c, 1200) == doctest::Approx(700));
    c.duration_from_ability = true;
    CHECK(resolve_duration(c, 1200) == doctest::Approx(1200));
    CHECK(resolve_duration(c, 0) == 0);      // an open-ended ability: the validator wants a number
    CHECK(resolve_duration(c, -4) == 0);

    // retuning the ability retimes the clip: the same pose at the same fraction
    c.tracks.push_back(track(Part::Head, Channel::Rot, TrackMode::Set, {key(0, 0), key(1, 90)}));
    Pose a, b;
    apply_clip(c, at(300, resolve_duration(c, 600)), a);
    apply_clip(c, at(600, resolve_duration(c, 1200)), b);
    CHECK(rot_x(a, Part::Head) == doctest::Approx(45));
    CHECK(rot_x(b, Part::Head) == doctest::Approx(45));
}

TEST_CASE("anim: hide intervals") {
    Clip c;
    c.duration_ms = 1000;
    c.hides.push_back({0.0f, 0.2f, kHideHead, 0});
    c.hides.push_back({0.5f, 1.0f, kHideWeapons | kHideHair, 0});
    CHECK(hide_mask(c, at(0)) == kHideHead);
    CHECK(hide_mask(c, at(199)) == kHideHead);
    CHECK(hide_mask(c, at(200)) == 0);                              // half open: to is out
    CHECK(hide_mask(c, at(500)) == (kHideWeapons | kHideHair));
    CHECK(hide_mask(c, at(999)) == (kHideWeapons | kHideHair));
    CHECK(hide_mask(c, at(5000)) == (kHideWeapons | kHideHair));    // an interval to 1 stays on while the clip holds
    CHECK(hide_mask(c, at(0, 0)) == 0);                             // no duration

    Clip l = c;
    l.end = End::Loop;
    CHECK(hide_mask(l, at(1100)) == kHideHead);                     // the loop wraps
    CHECK(hide_mask(l, at(1300)) == 0);
}

TEST_CASE("anim: markers crossed between two steps") {
    Clip c;
    c.duration_ms = 1000;
    c.markers.push_back({0.5f, "hit", 0});
    c.markers.push_back({0.25f, "wind", 0});
    c.markers.push_back({1.0f, "end", 0});

    SUBCASE("each step reports the markers inside (from, to]") {
        CHECK(markers_crossed(c, 0, 200, 1000).empty());
        auto a = markers_crossed(c, 200, 300, 1000);
        REQUIRE(a.size() == 1);
        CHECK(c.markers[a[0]].name == "wind");
        auto b = markers_crossed(c, 300, 500, 1000);                // to is included
        REQUIRE(b.size() == 1);
        CHECK(c.markers[b[0]].name == "hit");
        CHECK(markers_crossed(c, 500, 900, 1000).empty());         // from is not
    }
    SUBCASE("in the order crossed, not the order written") {
        auto a = markers_crossed(c, 0, 1000, 1000);
        REQUIRE(a.size() == 3);
        CHECK(c.markers[a[0]].name == "wind");
        CHECK(c.markers[a[1]].name == "hit");
        CHECK(c.markers[a[2]].name == "end");
    }
    SUBCASE("a held clip crosses each marker once") {
        CHECK(markers_crossed(c, 1000, 3000, 1000).empty());
    }
    SUBCASE("a looping clip crosses them once per lap") {
        c.end = End::Loop;
        auto a = markers_crossed(c, 900, 1300, 1000);                // the end of lap 0 and the wind-up of lap 1
        REQUIRE(a.size() == 2);
        CHECK(c.markers[a[0]].name == "end");
        CHECK(c.markers[a[1]].name == "wind");
        CHECK(markers_crossed(c, 0, 2000, 1000).size() == 6);       // two laps of three
    }
    SUBCASE("a backwards or empty range, or no duration, reports nothing") {
        CHECK(markers_crossed(c, 500, 500, 1000).empty());
        CHECK(markers_crossed(c, 600, 100, 1000).empty());
        CHECK(markers_crossed(c, 0, 1000, 0).empty());
    }
}

TEST_CASE("anim: the borrowed mode's clock") {
    Clip c;
    CHECK(borrowed_mode_time(c, 500) == 0);                         // no borrow
    c.borrow.present = true;
    c.borrow.mode = 57;
    CHECK(borrowed_mode_time(c, 500) == doctest::Approx(500));
    c.borrow.speed = 2;
    CHECK(borrowed_mode_time(c, 500) == doctest::Approx(1000));
    c.borrow.offset_ms = 150;
    CHECK(borrowed_mode_time(c, 500) == doctest::Approx(1150));
    c.borrow.offset_ms = -2000;
    CHECK(borrowed_mode_time(c, 500) == 0);                         // never below 0
    CHECK(borrowed_mode_time(c, -50) == 0);
}
