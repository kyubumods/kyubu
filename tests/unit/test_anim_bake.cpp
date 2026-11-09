// Unit tests of `anim rig` and `anim bake`: the words, the model slots and the rig's pose.json, and the bake's thinning, its clip text (read
// back by the loader's own validator and evaluator) and the difference check (openspec add-ability-animation, task 7.4;
// mods-dev/anim_capture). No game and no Direct3D.
#include "doctest.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "capture_args.h"
#include "capture_sheet.h"
#include "json.hpp"

using namespace kyubu::anim_capture;
namespace anim = kyubu::game::anim;

TEST_CASE("anim bake: a pose id, with the default or a given duration") {
    const BakeArgs a = parse_bake_args("18");
    REQUIRE(a.ok);
    CHECK(a.pose == 18);
    CHECK(a.duration_ms == kBakeDefaultMs);
    const BakeArgs b = parse_bake_args("  0   1500.5 ");
    REQUIRE(b.ok);
    CHECK(b.pose == 0);
    CHECK(b.duration_ms == 1500.5);
    CHECK(parse_bake_args("73 8000").ok);
    CHECK(parse_bake_args("1 100").ok);
}

TEST_CASE("anim bake: wrong input names what is wrong") {
    for (const char* bad : {"", "  ", "1 2 3"}) {
        const BakeArgs a = parse_bake_args(bad);
        REQUIRE_MESSAGE(!a.ok, bad);
        CHECK_MESSAGE(a.error == kBakeUsage, bad);
    }
    BakeArgs a = parse_bake_args("walk");
    CHECK(!a.ok);
    CHECK(a.error.find("'walk' is not a pose id") != std::string::npos);
    a = parse_bake_args("74");
    CHECK(!a.ok);
    CHECK(a.error.find("0 to 73") != std::string::npos);
    CHECK(!parse_bake_args("-1").ok);
    CHECK(!parse_bake_args("1.5").ok);
    a = parse_bake_args("5 soon");
    CHECK(!a.ok);
    CHECK(a.error.find("not a duration") != std::string::npos);
    a = parse_bake_args("5 99");
    CHECK(!a.ok);
    CHECK(a.error.find("100 to 8000") != std::string::npos);
    CHECK(!parse_bake_args("5 8001").ok);
    CHECK(!parse_bake_args("5 nan").ok);
}

TEST_CASE("anim rig: takes no arguments") {
    CHECK(rig_args_error("").empty());
    CHECK(rig_args_error("   ").empty());
    CHECK(rig_args_error("front") == kRigUsage);
}

TEST_CASE("anim rig: the model slots resolve to indexes of the model table") {
    uint8_t rec[0x890] = {};
    const uint32_t table[] = {0x1000, 0x2000, 0x3000, 0x4000};
    auto put = [&](int offset, uint32_t v) { std::memcpy(rec + offset, &v, 4); };
    put(0xBC, 0x3000);   // head: table[2]
    put(0xC4, 0x1000);   // body: table[0]
    put(0xE0, 0x9999);   // weapon_a: a pointer that is not in the table
    FrameData f;
    read_model_slots(rec, table, 4, &f);
    REQUIRE(f.has_models);
    CHECK(f.model[0] == 2);                    // head
    CHECK(f.model[1] == kModelEmpty);          // hair
    CHECK(f.model[2] == 0);                    // body
    CHECK(f.model[9] == kModelNotInTable);     // weapon_a
    CHECK(f.model[5] == kModelEmpty);          // wings
}

TEST_CASE("anim rig: pose.json lists the slots that hold a model, and the appearance ids") {
    uint8_t rec[0x890] = {};
    const uint32_t table[] = {0x10, 0x20};
    const uint32_t head = 0x20, weapon = 0x77;
    std::memcpy(rec + 0xBC, &head, 4);
    std::memcpy(rec + 0xE0, &weapon, 4);
    FrameData f;
    read_model_slots(rec, table, 2, &f);
    const int16_t app[kAppearanceCount] = {1246, 1252, 430, 432, 1, -1, -1, -1};
    read_appearance(app, &f);
    f.view = "front";
    f.body_yaw = 180;
    const nlohmann::json j = nlohmann::json::parse(pose_json("rig", "", "cube-0x51D2CE9C", {f}));
    CHECK(j["kind"] == "rig");
    CHECK(j["clip"] == "");
    const nlohmann::json& fr = j["frames"][0];
    CHECK(fr["models"]["head"] == 1);
    CHECK(fr["models"]["weapon_a"].is_null());
    CHECK(!fr["models"].contains("hair"));
    CHECK(fr["appearance"]["head"] == 1246);
    CHECK(fr["appearance"]["feet"] == 432);
    CHECK(fr["appearance"]["tail"] == -1);
    // a capture frame has neither key
    FrameData plain;
    const nlohmann::json p = nlohmann::json::parse(pose_json("capture", "demo:x", "b", {plain}));
    CHECK(!p["frames"][0].contains("models"));
    CHECK(!p["frames"][0].contains("appearance"));
}

namespace {
// 60 draws a second over `ms`: a head that nods, a hand that lifts and the root that slides.
std::vector<FrameData> make_bake(double ms, double hz) {
    std::vector<FrameData> out;
    const double step = 1000.0 / hz;
    for (double t = 0; t <= ms + 1e-9; t += step) {
        FrameData f;
        f.index = int(out.size());
        f.t_ms = t;
        f.view = "live";
        f.pose_id = 18;
        f.mode_time_ms = uint32_t(t);
        f.rot[2][0] = float(40.0 * std::sin(t / 300.0));    // head
        f.pos[3][2] = float(14.0 + 3.0 * t / ms);           // hand_r z
        f.pos[0][0] = float(1.5 * std::sin(t / 700.0));     // root x
        f.pos[0][1] = float(-0.5 * t / ms);                 // root y
        f.rot[5][1] = float(5.0 * std::cos(t / 200.0));     // tail
        out.push_back(f);
    }
    return out;
}
}  // namespace

TEST_CASE("anim bake: thinning keeps the first and the last frame and the gap between the rest") {
    const std::vector<FrameData> f = make_bake(2000, 60);
    const std::vector<size_t> kept = thin_frames(f, 1000.0 / 30);
    REQUIRE(kept.size() > 2);
    CHECK(kept.front() == 0);
    CHECK(kept.back() == f.size() - 1);
    CHECK(kept.size() >= 55);
    CHECK(kept.size() <= 70);
    for (size_t i = 1; i < kept.size(); ++i) CHECK(f[kept[i]].t_ms - f[kept[i - 1]].t_ms >= 1000.0 / 30 - 0.5);
    // one frame, none
    CHECK(thin_frames({}, 33).empty());
    CHECK(thin_frames({f[0]}, 33) == std::vector<size_t>{0});
    // the last frame is never dropped, however close the one before is
    std::vector<FrameData> g = {f[0], f[1], f[2]};
    g[1].t_ms = 100;
    g[2].t_ms = 101;
    const std::vector<size_t> k2 = thin_frames(g, 33);
    CHECK(k2.back() == 2);
    CHECK(k2.front() == 0);
}

TEST_CASE("anim bake: the clip of the baked vectors is valid and plays the draws back") {
    const std::vector<FrameData> f = make_bake(2000, 60);
    const std::vector<size_t> kept = thin_frames(f, 1000.0 / 30);
    double duration = 0;
    const std::string text = bake_clip_text(f, kept, &duration);
    REQUIRE(!text.empty());
    CHECK(duration == doctest::Approx(f.back().t_ms));

    const anim::ClipLoad load = anim::parse_clip(text, "bake.anim.toml");
    for (const anim::ClipError& e : load.errors) MESSAGE(anim::format_error(e));
    REQUIRE(load.ok);
    CHECK(load.clip.tracks.size() == 11);    // pos of five parts (not the tail) and rot of six
    CHECK(!load.clip.borrow.present);
    CHECK(load.clip.duration_ms == doctest::Approx(float(duration)));
    for (const anim::Track& t : load.clip.tracks) {
        CHECK(t.mode == anim::TrackMode::Set);
        CHECK(t.keys.size() == kept.size());
    }

    // every draw of the bake, replayed at its own time, is within the mimic gate's tolerance, and far inside it for this smooth pose
    VectorDiff d;
    for (const FrameData& fr : f) {
        anim::Pose pose;
        anim::Timing t;
        t.elapsed_ms = float(fr.t_ms);
        t.duration_ms = float(duration);
        REQUIRE(anim::apply_clip(load.clip, t, pose).ok);
        FrameData back;
        for (int i = 0; i < kPartVecCount; ++i) {
            std::memcpy(back.pos[i], pose.parts[i].pos, 12);
            std::memcpy(back.rot[i], pose.parts[i].rot, 12);
        }
        accumulate_diff(back, fr, &d);
    }
    CHECK(d.frames == int(f.size()));
    CHECK(d.max_pos < 0.05);
    CHECK(d.max_rot < 0.2);
}

TEST_CASE("anim bake: a creature with no tail model gets a clip with no tail track") {
    const std::vector<FrameData> f = make_bake(1000, 60);
    double duration = 0;
    const std::string text = bake_clip_text(f, thin_frames(f, 1000.0 / 30), &duration, false);
    const anim::ClipLoad load = anim::parse_clip(text, "bake");
    REQUIRE(load.ok);
    CHECK(load.clip.tracks.size() == 10);
    for (const anim::Track& t : load.clip.tracks) CHECK(t.part != anim::Part::Tail);
}

TEST_CASE("anim bake: a bake with one frame, or no time span, has no clip") {
    const std::vector<FrameData> f = make_bake(100, 60);
    double duration = -1;
    CHECK(bake_clip_text(f, {0}, &duration).empty());
    FrameData a = f[0], b = f[0];   // two draws at time 0
    CHECK(bake_clip_text({a, b}, {0, 1}, &duration).empty());
}

TEST_CASE("anim bake: a number that is not finite is written as 0 so the clip stays valid") {
    std::vector<FrameData> f = make_bake(500, 60);
    f[3].rot[2][0] = std::nanf("");
    f[4].pos[3][2] = INFINITY;
    double duration = 0;
    const std::string text = bake_clip_text(f, thin_frames(f, 1000.0 / 30), &duration);
    REQUIRE(!text.empty());
    CHECK(text.find("nan") == std::string::npos);
    CHECK(text.find("inf") == std::string::npos);
    CHECK(anim::parse_clip(text, "bake").ok);
}

TEST_CASE("anim bake: interpolation is linear between draws and held outside them") {
    std::vector<FrameData> f(3);
    f[0].t_ms = 0;
    f[1].t_ms = 100;
    f[2].t_ms = 300;
    f[0].rot[2][0] = 0;
    f[1].rot[2][0] = 10;
    f[2].rot[2][0] = 50;
    FrameData o;
    interp_vectors(f, 50, &o);
    CHECK(o.rot[2][0] == doctest::Approx(5));
    interp_vectors(f, 200, &o);
    CHECK(o.rot[2][0] == doctest::Approx(30));
    interp_vectors(f, 100, &o);
    CHECK(o.rot[2][0] == doctest::Approx(10));
    interp_vectors(f, 999, &o);
    CHECK(o.rot[2][0] == doctest::Approx(50));
    interp_vectors(f, -5, &o);
    CHECK(o.rot[2][0] == doctest::Approx(0));
    CHECK(o.t_ms == -5);
}

TEST_CASE("anim bake: the difference names the part that differs most") {
    FrameData a, b;
    b.rot[3][1] = 2.5f;     // hand_r y
    b.pos[2][2] = -0.75f;   // head z
    b.pos[0][2] = 9.0f;     // root z: the game reads x and y only
    VectorDiff d;
    accumulate_diff(a, b, &d);
    CHECK(d.frames == 1);
    CHECK(d.max_rot == doctest::Approx(2.5));
    CHECK(d.max_pos == doctest::Approx(0.75));
    CHECK(std::string(kPartVecs[d.rot_part].name) == "hand_r");
    CHECK(std::string(kPartVecs[d.pos_part].name) == "head");
    // a second frame only raises the maxima
    FrameData c;
    c.rot[3][1] = 1.0f;
    accumulate_diff(a, c, &d);
    CHECK(d.frames == 2);
    CHECK(d.max_rot == doctest::Approx(2.5));
}
