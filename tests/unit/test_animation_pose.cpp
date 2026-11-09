// Unit tests of the pose provider (openspec add-ability-animation tasks 4.1, 4.2 and 5.1): the decisions of a draw (plan_borrow,
// plan_draw), the game's hide bits, the tracks written over a fake draw record and put back (apply_tracks, restore_pose), and the
// provider turning itself off on a build whose entries are unverified. The detour and the join site's hook need the game: the replays
// `anim-borrow` and `anim-tracks` run them.
#include "doctest.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "abilities.h"
#include "ability_bar.h"
#include "animation_pose.h"
#include "animations.h"
#include "api.h"
#include "game.h"
#include "loader.h"
#include "log.h"
#include "modmanager.h"
#include "world.h"
#include "registry.h"

using namespace kyubu;
using namespace kyubu::game;

namespace {

Registries& reset_registries() {
    Registries& g = Registries::get();
    g.~Registries();
    new (&g) Registries();
    g.init_builtin();
    ability_callbacks_clear();
    return g;
}

struct PoseMod {
    Mod mod;
    PoseMod() {
        mod.id = "demo";
        mod.state = ModState::Loaded;
        mod.manifest.sides = KY_SIDE_BOTH;
        mod.manifest.trust = "full";
        for (int s = 1; s <= 2; ++s) { mod.ctx[s].mod = &mod; mod.ctx[s].side = (KyubuSide)s; }
    }
};

// The client register phase, the provider available, no binding and no preview.
struct PoseState {
    KyubuPhase phase = L().phase.load();
    decltype(L().host.host) host = L().host.host;
    PoseState() {
        L().host.host = KY_HOST_CLIENT;
        L().phase = KY_PHASE_REGISTER;
        force_primitive_for_test(Prim::animation_pose, 1);
        force_pose_tracks_for_test(1);
        Animations::get().clear_bindings();
        Animations::get().clear_preview();
    }
    ~PoseState() {
        force_primitive_for_test(Prim::animation_pose, -1);
        force_pose_tracks_for_test(-1);
        Animations::get().clear_bindings();
        Animations::get().clear_preview();
        L().phase = phase;
        L().host.host = host;
    }
};

void make_clip(const char* name, const std::string& text) {
    std::string errors;
    REQUIRE_MESSAGE(Animations::get().define_from_text("demo", name, text, &errors), errors);
}

// A phase-1 clip: borrow pose 2 at twice the speed from 150 ms, hide the head over the second half.
const char* kBorrow =
    "duration = 500\n\n[borrow]\nmode = 2\nspeed = 2\noffset_ms = 150\n\n[[hide]]\nfrom = 0.5\nto = 1.0\nparts = [\"head\", \"weapons\"]\n";

// Defines `demo:slam` (800 ms), freezes, and gives its number.
uint32_t define_ability(PoseMod& m) {
    KyubuAbilityDef d = {};
    d.size = sizeof d;
    d.duration_ms = 800;
    d.max_rank = 1;
    static const uint32_t cooldown[2] = {1000, 1000};
    d.cooldown_ms = cooldown;
    REQUIRE(api_table()->ability_define(m.mod.handle(KY_SIDE_CLIENT), "demo:slam", &d) == KY_OK);
    Registries::get().freeze([](const std::string&) { return 0; });
    const RegEntry* e = Registries::get().get_registry("kyubu:abilities")->find("demo:slam");
    REQUIRE(e);
    return e->number;
}

bool logged(uint64_t since, const std::string& part) {
    for (auto& e : Log::get().since(since))
        if (e.text.find(part) != std::string::npos) return true;
    return false;
}

// A tracks-only clip: the weapon arm turns to 30 degrees on x over the clip.
const char* kTracks =
    "duration = 400\n\n[[track]]\npart = \"hand_r\"\nchannel = \"rot\"\nkeys = [ { t = 0, v = [0, 0, 0] }, { t = 1, v = [30, 0, 0] } ]\n";
// kBorrow with a track on the head.
const std::string kBoth = std::string(kBorrow) + "\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nkeys = [ { t = 0, v = [0, 0, 10] } ]\n";

// A fake draw record: every float is its own byte offset, so a test sees which floats a write touched.
struct FakeRecord {
    std::vector<float> f = std::vector<float>(kDrawRecordSize / 4);
    FakeRecord() {
        for (size_t i = 0; i < f.size(); ++i) f[i] = (float)(i * 4);
    }
    void* data() { return f.data(); }
    float at(size_t offset) const { return f[offset / 4]; }
    // How many floats differ from the untouched pattern.
    int changed() const {
        int n = 0;
        for (size_t i = 0; i < f.size(); ++i)
            if (f[i] != (float)(i * 4)) ++n;
        return n;
    }
};

anim::Track track(anim::Part part, anim::Channel channel, anim::TrackMode mode, float x, float y, float z) {
    anim::Track t;
    t.part = part;
    t.channel = channel;
    t.mode = mode;
    anim::Key k;
    k.v[0] = x;
    k.v[1] = y;
    k.v[2] = z;
    t.keys.push_back(k);
    return t;
}

anim::Timing at_ms(float elapsed, float duration) {
    anim::Timing t;
    t.elapsed_ms = elapsed;
    t.duration_ms = duration;
    return t;
}

}  // namespace

TEST_CASE("ability-animation: the game's hide bits are not the loader's") {
    // covers: ability-animation/Hidden groups and bad input: Hidden head (the mask the game reads)
    CHECK(game_hide_bits(anim::kHideHead) == 0x01);
    CHECK(game_hide_bits(anim::kHideHair) == 0x02);
    CHECK(game_hide_bits(anim::kHideShoulders) == 0x04);
    CHECK(game_hide_bits(anim::kHideBody) == 0x08);
    CHECK(game_hide_bits(anim::kHideWeapons) == 0x40);
    CHECK(game_hide_bits(0) == 0);
    CHECK(game_hide_bits(anim::kHideHead | anim::kHideBody | anim::kHideWeapons) == 0x49);
}

TEST_CASE("ability-animation: a borrow clip gives the pose id, the retimed clock and the hide bits") {
    // covers: ability-animation/The borrowed look (phase 1): A mod ability wears a vanilla pose; Retimed
    anim::Clip c;
    c.duration_ms = 500;
    c.borrow.present = true;
    c.borrow.mode = 2;
    BorrowPlan p;
    CHECK(plan_borrow(c, 500, 800, &p));   // speed 1, no offset: the mode time as it is
    CHECK(p.pose_id == 2);
    CHECK(p.time_ms == 500);
    CHECK(p.hide_bits == 0);
    c.borrow.speed = 2;
    c.borrow.offset_ms = 150;
    CHECK(plan_borrow(c, 500, 800, &p));   // the spec's retimed scenario: 150 + 500 * 2
    CHECK(p.time_ms == 1150);
    CHECK(plan_borrow(c, 0, 800, &p));
    CHECK(p.time_ms == 150);

    c.hides.push_back({0.5f, 1.0f, anim::kHideHead | anim::kHideWeapons, 0});
    CHECK(plan_borrow(c, 100, 800, &p));
    CHECK(p.hide_bits == 0);
    CHECK(plan_borrow(c, 300, 800, &p));
    CHECK(p.hide_bits == 0x41);

    // `duration = "ability"` takes the ability's 800 ms: 300 ms is then before half
    c.duration_from_ability = true;
    CHECK(plan_borrow(c, 300, 800, &p));
    CHECK(p.hide_bits == 0);
    CHECK(plan_borrow(c, 500, 800, &p));
    CHECK(p.hide_bits == 0x41);
}

TEST_CASE("ability-animation: the clip's fraction follows the ability's duration, the borrowed clock does not") {
    // covers: ability-animation/The end and the duration: The ability sets the duration
    anim::Clip c;
    c.duration_from_ability = true;
    c.borrow.present = true;
    c.borrow.mode = 2;
    c.hides.push_back({0.25f, 0.75f, anim::kHideHead, 0});
    BorrowPlan a, b;
    // half the run of a 600 ms ability and of a 1,200 ms one: the same fraction, so the same hide mask
    CHECK(plan_borrow(c, 300, 600, &a));
    CHECK(plan_borrow(c, 600, 1200, &b));
    CHECK(a.hide_bits == 0x01);
    CHECK(b.hide_bits == a.hide_bits);
    // the same fractions at the start, at a quarter (the interval opens: from <= u) and at the end (it has closed)
    CHECK(plan_borrow(c, 0, 600, &a));
    CHECK(a.hide_bits == 0);
    CHECK(plan_borrow(c, 150, 600, &a));
    CHECK(plan_borrow(c, 300, 1200, &b));
    CHECK(a.hide_bits == 0x01);
    CHECK(b.hide_bits == 0x01);
    CHECK(plan_borrow(c, 600, 600, &a));
    CHECK(plan_borrow(c, 1200, 1200, &b));
    CHECK(a.hide_bits == 0);
    CHECK(b.hide_bits == 0);
    // retuning the ability does not move the borrowed pose's clock: it is the mode time in ms (vanilla poses run on their own clock)
    CHECK(plan_borrow(c, 300, 600, &a));
    CHECK(plan_borrow(c, 300, 1200, &b));
    CHECK(a.time_ms == 300);
    CHECK(b.time_ms == 300);
    // an ability with no duration known: nothing to place the hide interval on, so no group is hidden
    CHECK(plan_borrow(c, 300, 0, &a));
    CHECK(a.hide_bits == 0);
    // a fixed duration ignores the ability's
    c.duration_from_ability = false;
    c.duration_ms = 400;
    CHECK(plan_borrow(c, 200, 600, &a));
    CHECK(plan_borrow(c, 200, 1200, &b));
    CHECK(a.hide_bits == 0x01);
    CHECK(b.hide_bits == 0x01);
    // past the end: hold stays at 1 (an interval that ends at 1 would stay on), loop wraps to the start
    c.hides.clear();
    c.hides.push_back({0.5f, 1.0f, anim::kHideWeapons, 0});
    CHECK(plan_borrow(c, 4000, 600, &a));
    CHECK(a.hide_bits == 0x40);
    c.end = anim::End::Loop;
    CHECK(plan_borrow(c, 4000, 600, &a));   // 4000 mod 400 = 0: the start of a lap
    CHECK(a.hide_bits == 0);
    CHECK(plan_borrow(c, 4250, 600, &a));   // 250 of 400: past half
    CHECK(a.hide_bits == 0x40);
}

TEST_CASE("ability-animation: only a borrow clip with a pose id is worn") {
    anim::Clip c;
    c.duration_ms = 500;
    BorrowPlan p;
    p.pose_id = 99;
    CHECK_FALSE(plan_borrow(c, 100, 0, &p));   // no [borrow]: plan_draw gives a tracks-only clip pose 0
    CHECK(p.pose_id == 99);                    // the plan is untouched
    c.borrow.present = true;
    c.borrow.mode = kMaxPoseId;
    CHECK(plan_borrow(c, 100, 0, &p));
    c.borrow.mode = kMaxPoseId + 1;
    CHECK_FALSE(plan_borrow(c, 100, 0, &p));   // the validator takes 0..110, a record holds 0..73
    c.borrow.mode = 0;
    CHECK(plan_borrow(c, 100, 0, &p));         // pose 0 is the game's neutral pose, a legal borrow
    CHECK(p.pose_id == 0);
}

TEST_CASE("ability-animation: a draw wears the clip bound to its mod ability, and a vanilla mode is untouched") {
    // covers: ability-animation/The borrowed look (phase 1): A mod ability wears a vanilla pose
    // covers: ability-animation/Where a clip is drawn: Vanilla modes are untouched
    PoseState state;
    reset_registries();
    PoseMod demo;
    make_clip("slam", kBorrow);
    const uint32_t number = define_ability(demo);
    REQUIRE(number >= kProviderFirstMode);

    DrawFacts f;
    f.mode = number;
    f.record_time_ms = 500;
    BorrowPlan p;
    CHECK_FALSE(plan_draw(f, &p));   // no clip bound: the ability looks as add-ability-api leaves it
    Animations::get().bind("demo:slam", "demo:slam");
    CHECK(plan_draw(f, &p));
    CHECK(p.pose_id == 2);
    CHECK(p.time_ms == 1150);
    CHECK(p.hide_bits == 0x41);   // 500 ms of a 500 ms clip: the second half

    for (uint32_t mode : {0u, 1u, 57u, 110u, 111u}) {   // vanilla and the placeholder: never worn
        f.mode = mode;
        CHECK_FALSE(plan_draw(f, &p));
    }
    f.mode = number + 1;   // a number nobody defined
    CHECK_FALSE(plan_draw(f, &p));
    f.mode = 256;
    CHECK_FALSE(plan_draw(f, &p));
}

TEST_CASE("ability-animation: a creature's own binding is worn by that creature alone") {
    // covers: add-skills-unleashed-multiplayer 8.1 (ability_animation_bind_for at the draw)
    PoseState state;
    reset_registries();
    PoseMod demo;
    make_clip("slam", kBorrow);
    const uint32_t number = define_ability(demo);
    REQUIRE(number >= kProviderFirstMode);
    DrawFacts f;
    f.mode = number;
    f.record_time_ms = 500;
    f.creature_id = 9;
    BorrowPlan p;
    Animations::get().bind_for("demo:slam", 9, "demo:slam");
    CHECK(plan_draw(f, &p));
    CHECK(p.pose_id == 2);
    f.creature_id = 8;
    CHECK_FALSE(plan_draw(f, &p));   // nothing bound for the ability itself
    Animations::get().clear_bindings();
}

TEST_CASE("ability-animation: a clip that went away gives the game's pose, and a tracks-only clip is worn on pose 0") {
    // covers: ability-registry/Rebinding and missing clips: A clip that does not exist (at the draw)
    PoseState state;
    reset_registries();
    PoseMod demo;
    make_clip("tracks", kTracks);
    const uint32_t number = define_ability(demo);
    ability_slots_resolve(true);   // publishes the stat rows: the ability's 800 ms
    DrawFacts f;
    f.mode = number;
    f.record_time_ms = 200;
    BorrowPlan p;
    p.pose_id = 9;
    Animations::get().bind("demo:slam", "demo:tracks");
    CHECK(plan_draw(f, &p));
    CHECK(p.pose_id == 0);        // "none": the game's neutral pose is the base
    CHECK(p.time_ms == 200);      // the record's own clock
    CHECK(p.hide_bits == 0);
    REQUIRE(p.tracks);
    CHECK(p.timing.elapsed_ms == 200);
    CHECK(p.timing.duration_ms == 400);
    CHECK(p.timing.run_ms == 800);   // blend_out counts back from the ability's end
    Animations::get().bind("demo:slam", "demo:gone");
    const uint64_t since = Log::get().sequence();
    CHECK_FALSE(plan_draw(f, &p));
    CHECK(logged(since, "demo:gone"));
    const uint64_t again = Log::get().sequence();
    CHECK_FALSE(plan_draw(f, &p));
    CHECK_FALSE(logged(again, "demo:gone"));   // once, not per draw
}

TEST_CASE("ability-animation: a clip with a borrow and tracks keeps the borrowed pose under its tracks") {
    PoseState state;
    reset_registries();
    PoseMod demo;
    make_clip("both", kBoth);
    const uint32_t number = define_ability(demo);
    Animations::get().bind("demo:slam", "demo:both");
    DrawFacts f;
    f.mode = number;
    f.record_time_ms = 100;
    BorrowPlan p;
    CHECK(plan_draw(f, &p));
    CHECK(p.pose_id == 2);
    CHECK(p.time_ms == 350);   // 150 + 100 * 2
    CHECK(p.tracks);
}

TEST_CASE("ability-animation: with no safe join site a clip with tracks is refused with the reason, and a borrow still works") {
    // covers: ability-animation/A safe site only: No safe site
    PoseState state;
    reset_registries();
    PoseMod demo;
    make_clip("tracks", kTracks);
    make_clip("slam", kBorrow);
    make_clip("both", kBoth);
    const uint32_t number = define_ability(demo);
    force_pose_tracks_for_test(0, "cannot hook draw_creature_join_site: unsafe hook site");
    DrawFacts f;
    f.mode = number;
    f.record_time_ms = 100;
    BorrowPlan p;

    Animations::get().bind("demo:slam", "demo:tracks");
    const uint64_t since = Log::get().sequence();
    CHECK_FALSE(plan_draw(f, &p));   // the game's own draw
    bool error = false;
    for (auto& e : Log::get().since(since))
        if (e.level == KY_LOG_ERROR && e.text.find("demo:tracks") != std::string::npos && e.text.find("unsafe hook site") != std::string::npos)
            error = true;
    CHECK(error);
    const uint64_t again = Log::get().sequence();
    CHECK_FALSE(plan_draw(f, &p));
    CHECK(Log::get().since(again).empty());   // once, not per draw

    Animations::get().bind("demo:slam", "demo:slam");   // a clip with only a [borrow] table
    const uint64_t borrow = Log::get().sequence();
    CHECK(plan_draw(f, &p));
    CHECK(p.pose_id == 2);
    CHECK_FALSE(p.tracks);
    CHECK(Log::get().since(borrow).empty());

    force_pose_tracks_for_test(0, "cannot hook draw_creature_join_site: unsafe hook site");   // the once-flags start again
    Animations::get().bind("demo:slam", "demo:both");
    const uint64_t both = Log::get().sequence();
    CHECK(plan_draw(f, &p));         // the borrowed look stays
    CHECK(p.pose_id == 2);
    CHECK_FALSE(p.tracks);
    CHECK(logged(both, "its borrowed pose is drawn"));

    std::string why;
    CHECK_FALSE(pose_tracks_on(&why));
    CHECK(why.find("unsafe hook site") != std::string::npos);
    force_pose_tracks_for_test(-1);
    CHECK_FALSE(pose_tracks_on(&why));   // no hook is installed in a unit test
    CHECK(why.find("draw_creature_join_site") != std::string::npos);
}

TEST_CASE("ability-animation: tracks are written over the record's vectors and put back") {
    // covers: ability-animation/The pose override (phase 2): A new pose
    // covers: ability-animation/The pose override (phase 2): Only what a track touches
    FakeRecord rec;
    anim::Clip c;
    c.duration_ms = 400;
    c.tracks.push_back(track(anim::Part::HandR, anim::Channel::Rot, anim::TrackMode::Add, 30, 0, 0));
    PoseSave save;
    REQUIRE(apply_tracks(rec.data(), c, at_ms(100, 400), &save));
    // `weapon_r` is hand B: its euler is at +0x30. `add` is an offset on the game's value.
    CHECK(rec.at(0x30) == doctest::Approx(0x30 + 30));
    CHECK(rec.at(0x34) == doctest::Approx(0x34));
    CHECK(rec.at(0x38) == doctest::Approx(0x38));
    CHECK(rec.changed() == 1);   // every other part and channel keeps the game's value
    CHECK(save.rot_parts == 1u << (int)anim::Part::HandR);
    CHECK(save.pos_parts == 0);
    restore_pose(rec.data(), save);
    CHECK(rec.changed() == 0);   // the game eases on from its own values

    // Draw after draw the offset stays 30: nothing piles up in the record.
    for (int frame = 0; frame < 5; ++frame) {
        REQUIRE(apply_tracks(rec.data(), c, at_ms(100, 400), &save));
        CHECK(rec.at(0x30) == doctest::Approx(0x30 + 30));
        restore_pose(rec.data(), save);
    }
    CHECK(rec.changed() == 0);
}

TEST_CASE("ability-animation: each part's tracks reach its own vectors, set replaces and the blend weighs") {
    struct Row { anim::Part part; size_t pos, rot; int pos_n; };
    const Row rows[] = {{anim::Part::Root, 0xAC, 0x60, 2}, {anim::Part::Body, 0x6C, 0x54, 3}, {anim::Part::Head, 0x78, 0x84, 3},
                        {anim::Part::HandR, 0x18, 0x30, 3}, {anim::Part::HandL, 0x0C, 0x24, 3}, {anim::Part::Tail, 0, 0x90, 0}};
    for (const Row& r : rows) {
        CAPTURE(anim::part_name(r.part));
        FakeRecord rec;
        anim::Clip c;
        c.duration_ms = 400;
        c.tracks.push_back(track(r.part, anim::Channel::Rot, anim::TrackMode::Set, 1, 2, 3));
        c.tracks.push_back(track(r.part, anim::Channel::Pos, anim::TrackMode::Set, 7, 8, 9));
        PoseSave save;
        REQUIRE(apply_tracks(rec.data(), c, at_ms(0, 400), &save));
        CHECK(rec.at(r.rot) == 1);
        CHECK(rec.at(r.rot + 4) == 2);
        CHECK(rec.at(r.rot + 8) == 3);
        if (r.pos_n) {
            CHECK(rec.at(r.pos) == 7);
            CHECK(rec.at(r.pos + 4) == 8);
        }
        if (r.pos_n == 3) CHECK(rec.at(r.pos + 8) == 9);
        CHECK(rec.changed() == 3 + r.pos_n);   // the root's z and the tail's position have no vector: nothing is written for them
        restore_pose(rec.data(), save);
        CHECK(rec.changed() == 0);
    }

    // The blend: half-way into blend_in a `set` is half-way between the game's value and the clip's.
    FakeRecord rec;
    anim::Clip c;
    c.duration_ms = 400;
    c.blend_in_ms = 100;
    c.tracks.push_back(track(anim::Part::Head, anim::Channel::Rot, anim::TrackMode::Set, 0, 0, 0));
    PoseSave save;
    REQUIRE(apply_tracks(rec.data(), c, at_ms(50, 400), &save));
    CHECK(rec.at(0x84) == doctest::Approx(0x84 * 0.5));
    restore_pose(rec.data(), save);
    // The same draw again gives the same value: the weight is not eaten by the write of the draw before.
    REQUIRE(apply_tracks(rec.data(), c, at_ms(50, 400), &save));
    CHECK(rec.at(0x84) == doctest::Approx(0x84 * 0.5));
    restore_pose(rec.data(), save);
}

TEST_CASE("ability-animation: a clip with nothing to write leaves the record alone") {
    FakeRecord rec;
    PoseSave save;
    save.rot_parts = 77;
    anim::Clip none;
    none.duration_ms = 400;
    CHECK_FALSE(apply_tracks(rec.data(), none, at_ms(10, 400), &save));   // no tracks
    anim::Clip tail;
    tail.duration_ms = 400;
    tail.tracks.push_back(track(anim::Part::Tail, anim::Channel::Pos, anim::TrackMode::Set, 1, 2, 3));
    CHECK_FALSE(apply_tracks(rec.data(), tail, at_ms(10, 400), &save));   // the tail has no position vector
    anim::Clip timeless;
    timeless.tracks.push_back(track(anim::Part::Head, anim::Channel::Rot, anim::TrackMode::Set, 1, 2, 3));
    CHECK_FALSE(apply_tracks(rec.data(), timeless, at_ms(10, 0), &save));   // no duration: nothing to draw
    anim::Clip bad;
    bad.duration_ms = 400;
    bad.tracks.push_back(track(anim::Part::Head, anim::Channel::Rot, anim::TrackMode::Set, 1, std::nanf(""), 3));
    CHECK_FALSE(apply_tracks(rec.data(), bad, at_ms(10, 400), &save));   // not a number: never given to the game
    CHECK(rec.changed() == 0);
    CHECK(save.rot_parts == 77);   // the save is untouched
}

TEST_CASE("ability-animation: a part the creature lacks is skipped, the rest applies, and it is logged once per mode and part") {
    // covers: ability-animation/A part the creature lacks: A part the creature lacks
    FakeRecord rec;
    anim::Clip c;
    c.duration_ms = 400;
    c.tracks.push_back(track(anim::Part::HandR, anim::Channel::Rot, anim::TrackMode::Set, 1, 2, 3));
    c.tracks.push_back(track(anim::Part::Tail, anim::Channel::Rot, anim::TrackMode::Set, 4, 5, 6));
    const uint32_t none = 0;
    memcpy((uint8_t*)rec.data() + 0xD4, &none, sizeof none);   // the tail's model slot: 0, the game draws no tail
    PoseSave save;
    uint32_t lacking = 0;
    REQUIRE(apply_tracks(rec.data(), c, at_ms(0, 400), &save, &lacking));
    CHECK(lacking == 1u << (int)anim::Part::Tail);
    CHECK(rec.at(0x30) == 1);                        // the rest of the clip applies: hand B is written
    CHECK(rec.at(0x90) == doctest::Approx(0x90));   // the tail's euler keeps the game's value
    CHECK(save.rot_parts == 1u << (int)anim::Part::HandR);
    restore_pose(rec.data(), save);

    // A clip with only the lacking track writes nothing, and still reports the part.
    anim::Clip tail_only;
    tail_only.duration_ms = 400;
    tail_only.tracks.push_back(track(anim::Part::Tail, anim::Channel::Rot, anim::TrackMode::Set, 4, 5, 6));
    lacking = 0;
    CHECK_FALSE(apply_tracks(rec.data(), tail_only, at_ms(0, 400), &save, &lacking));
    CHECK(lacking == 1u << (int)anim::Part::Tail);

    // A zero model slot on a hidden part (the game hid it, so it is not lacking): the head track is written.
    const uint32_t zero = 0;
    memcpy((uint8_t*)rec.data() + 0xBC, &zero, sizeof zero);
    anim::Clip head;
    head.duration_ms = 400;
    head.tracks.push_back(track(anim::Part::Head, anim::Channel::Rot, anim::TrackMode::Set, 7, 8, 9));
    lacking = 99;
    CHECK(apply_tracks(rec.data(), head, at_ms(0, 400), &save, &lacking, 1u << (int)anim::Part::Head));
    CHECK(lacking == 0);
    CHECK(rec.at(0x84) == 7);
    restore_pose(rec.data(), save);

    // The log line is written once per mode and part, not once per draw.
    CHECK(first_lacking_part(250, anim::Part::Tail));
    CHECK_FALSE(first_lacking_part(250, anim::Part::Tail));
    CHECK(first_lacking_part(250, anim::Part::HandR));
    CHECK(first_lacking_part(251, anim::Part::Tail));
}

TEST_CASE("ability-animation: a client without the mod draws the game's own look and logs nothing") {
    // covers: ability-animation/Multiplayer and saves: A client without the mod
    // The other player's mode is a number this client's registry does not hold (112 and up, no mod loaded here): the game draws pose 0
    // for it (mapping 2.2) and the provider must not interfere, throw or log.
    PoseState state;
    reset_registries();
    DrawFacts f;
    BorrowPlan p;
    const uint64_t since = Log::get().sequence();
    for (uint32_t mode = kProviderFirstMode; mode <= 255; ++mode) {
        f.mode = mode;
        f.record_time_ms = 400;
        CHECK_FALSE(plan_draw(f, &p));
    }
    CHECK(Log::get().since(since).empty());
}

TEST_CASE("animation-editor: the preview clock poses the local player only") {
    // covers: animation-editor/Preview on the local player: Scrub
    PoseState state;
    reset_registries();
    PoseMod demo;
    make_clip("slam", kBorrow);
    DrawFacts f;
    f.mode = 0;
    BorrowPlan p;
    f.is_local = true;
    CHECK_FALSE(plan_draw(f, &p));   // clock off
    std::string why;
    REQUIRE(Animations::get().set_preview("demo:slam", 200, &why));
    CHECK(plan_draw(f, &p));
    CHECK(p.pose_id == 2);
    CHECK(p.time_ms == 550);   // 150 + 200 * 2
    // A tracks-only clip on the clock: pose 0 under the tracks, whatever vanilla mode the player is in, at the clock's time.
    make_clip("tracks", kTracks);
    REQUIRE(Animations::get().set_preview("demo:tracks", 300, &why));
    f.mode = 57;
    f.record_time_ms = 40;
    CHECK(plan_draw(f, &p));
    CHECK(p.pose_id == 0);
    CHECK(p.time_ms == 40);
    REQUIRE(p.tracks);
    CHECK(p.timing.elapsed_ms == 300);
    // A tracks-only clip whose duration is the ability's has no clip time on the clock: the game's own draw, and one line says why.
    make_clip("timeless", "duration = \"ability\"\n\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nkeys = [ { t = 0, v = [0, 0, 10] } ]\n");
    REQUIRE(Animations::get().set_preview("demo:timeless", 300, &why));
    const uint64_t timeless = Log::get().sequence();
    CHECK_FALSE(plan_draw(f, &p));
    CHECK(logged(timeless, "has no duration here"));
    f.mode = 0;
    REQUIRE(Animations::get().set_preview("demo:slam", 200, &why));
    f.is_local = false;
    CHECK_FALSE(plan_draw(f, &p));   // another creature, and a preview screen's, keep the game's pose
    f.is_local = true;
    Animations::get().clear_preview();
    CHECK_FALSE(plan_draw(f, &p));   // "Preview off"
}

TEST_CASE("ability-animation: a borrow with tracks and no duration on the clock keeps the borrowed pose, drops the tracks, and logs once") {
    PoseState state;
    reset_registries();
    PoseMod demo;
    make_clip("bordur", "duration = \"ability\"\n\n[borrow]\nmode = 2\n\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nkeys = [ { t = 0, v = [0, 0, 10] } ]\n");
    std::string why;
    REQUIRE(Animations::get().set_preview("demo:bordur", 300, &why));
    forget_logs_for_test();   // an earlier case may have logged the preview's key already
    const uint64_t since = Log::get().sequence();
    DrawFacts f;
    f.mode = 57;
    f.is_local = true;
    BorrowPlan p;
    CHECK(plan_draw(f, &p));
    CHECK(p.pose_id == 2);
    CHECK_FALSE(p.tracks);   // no clip time on the clock, so no tracks
    CHECK(logged(since, "its tracks are not drawn"));
    CHECK(logged(since, "its borrowed pose is drawn"));
    const uint64_t again = Log::get().sequence();
    CHECK(plan_draw(f, &p));
    CHECK_FALSE(logged(again, "has no duration here"));   // once, not per draw
    // The once-key is the clip and its text, not the preview as a whole: a clip redefined through the editor (a new hash) logs again.
    make_clip("bordur", "duration = \"ability\"\n\n[borrow]\nmode = 3\n\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nkeys = [ { t = 0, v = [0, 0, 10] } ]\n");
    REQUIRE(Animations::get().set_preview("demo:bordur", 300, &why));
    const uint64_t redefined = Log::get().sequence();
    CHECK(plan_draw(f, &p));
    CHECK(p.pose_id == 3);
    CHECK(logged(redefined, "its tracks are not drawn"));
    Animations::get().clear_preview();
}

TEST_CASE("ability-animation: the provider turns itself off with a log line on an unverified build") {
    // covers: ability-animation/The provider on a build and on the server: Unverified build
    PoseState state;
    force_primitive_for_test(Prim::animation_pose, 0);
    const uint64_t since = Log::get().sequence();
    install_animation_pose_hook();
    CHECK_FALSE(animation_pose_hook_installed());
    CHECK(logged(since, "the pose provider is off"));
    std::string why;
    CHECK_FALSE(game::available(Prim::animation_pose, &why));
    // the `game` report names the primitive, so a user sees it too
    bool listed = false;
    for (auto& line : report())
        if (line.find("primitive animation_pose: unavailable") != std::string::npos) listed = true;
    CHECK(listed);
}

namespace {
uint32_t rank_of_test(uint32_t number) { return number == 120 ? 3u : 1u; }
}  // namespace

TEST_CASE("ability-animation: the signals of a draw come from the creature's own data (task 8.3)") {
    // covers: animation-editor/Procedural tracks: An oscillation (the signals an `expr` reads)
    cube_CreatureData data;
    std::memset(&data, 0, sizeof data);
    data.charged_mp = 0.75f;
    data.velocity.x = 3.0f;
    data.velocity.y = 4.0f;
    data.velocity.z = 99.0f;   // a fall is not a run: the ground plane only
    set_rank_source_for_test(rank_of_test);

    anim::Signals s = creature_signals(data, true, 120);
    CHECK(s.charge == doctest::Approx(0.75f));
    CHECK(s.speed == doctest::Approx(5.0f));
    CHECK(s.rank == 3);

    // The rank is the local player's, in a mod ability: another creature, a vanilla mode and a preview have 1.
    CHECK(creature_signals(data, false, 120).rank == 1);
    CHECK(creature_signals(data, true, 59).rank == 1);
    CHECK(creature_signals(data, true, 0).rank == 1);
    CHECK(creature_signals(data, true, 119).rank == 1);   // a mod ability the source gives 1

    // A negative or not-a-number value reads 0: it never reaches an expression.
    data.charged_mp = -2.0f;
    data.velocity.x = std::numeric_limits<float>::quiet_NaN();
    anim::Signals bad = creature_signals(data, true, 120);
    CHECK(bad.charge == 0);
    CHECK(bad.speed == 0);
    set_rank_source_for_test(nullptr);
}

TEST_CASE("ability-animation: an expr track reads the draw's signals and writes the record, and puts it back (task 8.3)") {
    FakeRecord rec;
    anim::ClipLoad r = anim::parse_clip(R"(duration = 400
[[track]]
part = "hand_r"
channel = "rot"
expr = ["charge * 40", "speed", "rank"]
)", "charge.anim.toml");
    REQUIRE(r.ok);
    anim::Timing t = at_ms(100, 400);
    t.signals.charge = 0.5f;
    t.signals.speed = 6;
    t.signals.rank = 2;
    PoseSave save;
    REQUIRE(apply_tracks(rec.data(), r.clip, t, &save));
    CHECK(rec.at(0x30) == doctest::Approx(0x30 + 20));
    CHECK(rec.at(0x34) == doctest::Approx(0x34 + 6));
    CHECK(rec.at(0x38) == doctest::Approx(0x38 + 2));
    restore_pose(rec.data(), save);
    CHECK(rec.changed() == 0);

    // The charge moves the pose: more charge, a larger turn.
    t.signals.charge = 1.0f;
    REQUIRE(apply_tracks(rec.data(), r.clip, t, &save));
    CHECK(rec.at(0x30) == doctest::Approx(0x30 + 40));
    restore_pose(rec.data(), save);

    // A keys clip reads no signal, and the provider asks only for a clip that does.
    CHECK(anim::clip_expr_uses(r.clip) == (anim::kUsesCharge | anim::kUsesSpeed | anim::kUsesRank));
    anim::ClipLoad k = anim::parse_clip(kTracks, "keys.anim.toml");
    REQUIRE(k.ok);
    CHECK(anim::clip_expr_uses(k.clip) == 0);
}
