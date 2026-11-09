// Unit tests of the `kanim` commands (openspec add-ability-animation task 3.6). No game. The output of each command on the clips
// in tests/fixtures/kanim is compared with the golden files beside them (a golden file is the output the user reads, so a
// change to the wording is a change to the file).
#include "doctest.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "kanim_core.h"

using namespace kyubu::kanim;
using namespace kyubu::game::anim;

namespace {

const std::string kDir = KYUBU_SOURCE_DIR "/tests/fixtures/kanim/";

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "cannot read " << path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string clip_text(const char* name) { return slurp(kDir + name + ".anim.toml"); }
std::string golden(const char* name) { return slurp(kDir + "golden/" + name); }
std::string src(const char* name) { return std::string(name) + ".anim.toml"; }

}  // namespace

TEST_CASE("kanim check: a good clip, the text of an error and its JSON") {
    Options o;
    Result ok = check(clip_text("design_example"), src("design_example"), o);
    CHECK(ok.code == 0);
    CHECK(ok.text == golden("check_ok.txt"));

    Result bad = check(clip_text("bad"), src("bad"), o);
    CHECK(bad.code == 1);
    CHECK(bad.text == golden("check_bad.txt"));

    o.json = true;
    Result js = check(clip_text("bad"), src("bad"), o);
    CHECK(js.code == 1);
    CHECK(js.text == golden("check_bad.json"));
    // The spec's scenario: line, field path, message and a hint.
    CHECK(js.text.find("\"line\":8") != std::string::npos);
    CHECK(js.text.find("\"path\":\"track[0].keys[1].t\"") != std::string::npos);
    CHECK(js.text.find("\"hint\":\"") != std::string::npos);

    Result jok = check(clip_text("design_example"), src("design_example"), o);
    CHECK(jok.code == 0);
    CHECK(jok.text == "{\"ok\":true,\"errors\":[]}\n");
}

TEST_CASE("kanim describe: golden output") {
    Options o;
    o.ability_ms = 900;
    CHECK(describe(clip_text("design_example"), src("design_example"), o).text == golden("describe_design_example.txt"));
    CHECK(describe(clip_text("pops"), src("pops"), o).text == golden("describe_pops.txt"));
    CHECK(describe(clip_text("loop"), src("loop"), o).text == golden("describe_loop.txt"));
    // "ability" with no number given: the output says what it assumed.
    CHECK(describe(clip_text("design_example"), src("design_example"), Options{}).text ==
          golden("describe_design_example_no_ability.txt"));
}

TEST_CASE("kanim describe: the design's example names the range, the fastest motion and the marker hit at 50%") {
    Options o;
    o.ability_ms = 1000;
    std::string t = describe(clip_text("design_example"), src("design_example"), o).text;
    CHECK(t.find("hand_r rot (add): x -110..30, y 0..0, z 0..10 deg") != std::string::npos);
    CHECK(t.find("moving 140.36 deg from 0.35 to 0.5") != std::string::npos);   // the strike, the fast part
    CHECK(t.find("hit at 0.5 (500 ms)") != std::string::npos);
    CHECK(t.find("pops: none") != std::string::npos);   // blend_in and blend_out are set
}

TEST_CASE("kanim describe: pops, jumps and the duration against the ability's") {
    Options o;
    o.ability_ms = 900;
    std::string t = describe(clip_text("pops"), src("pops"), o).text;
    CHECK(t.find("start: body rot (add) begins 20 deg off the base pose") != std::string::npos);
    CHECK(t.find("end: body rot (add) holds 20 deg") != std::string::npos);
    CHECK(t.find("start: root pos (set)") != std::string::npos);
    CHECK(t.find("jump: 40 deg at 0.5 (400 ms)") != std::string::npos);   // two keys on one t
    CHECK(t.find("jump: 1.5 vox at 1 (800 ms)") != std::string::npos);    // a hold ease
    CHECK(t.find("the last pose is held for 100 ms") != std::string::npos);

    o.ability_ms = 600;
    t = describe(clip_text("pops"), src("pops"), o).text;
    CHECK(t.find("the last 200 ms are never drawn") != std::string::npos);
    o.ability_ms = 800;
    t = describe(clip_text("pops"), src("pops"), o).text;
    CHECK(t.find("against the ability's 800 ms: equal") != std::string::npos);

    // A loop: the seam and the end.
    o.ability_ms = 900;
    t = describe(clip_text("loop"), src("loop"), o).text;
    CHECK(t.find("seam: tail rot (add) jumps 5 deg at each lap") != std::string::npos);
    CHECK(t.find("the clip loops 1.8 times") != std::string::npos);
}

TEST_CASE("kanim describe: blends remove the pops; the fastest motion is exact") {
    // A track from 0 to 90 over a whole 1 s clip, linear: 90 deg/s everywhere; ease_out starts at twice that; smooth peaks at 1.5x.
    auto speed_of = [](const char* ease) {
        std::string text = std::string("duration = 1000\nblend_in = 50\nblend_out = 50\n[[track]]\npart = \"head\"\nchannel = \"rot\"\n"
                                       "keys = [{ t = 0, v = [0, 0, 0] }, { t = 1, v = [0, 90, 0], ease = \"") + ease + "\" }]\n";
        Result r = describe(text, "x.anim.toml", Options{});
        CHECK(r.code == 0);
        CHECK(r.text.find("pops: none") != std::string::npos);
        size_t a = r.text.find("fastest: ");
        REQUIRE(a != std::string::npos);
        return std::atof(r.text.c_str() + a + 9);
    };
    CHECK(speed_of("linear") == doctest::Approx(90));
    CHECK(speed_of("ease_out") == doctest::Approx(180));
    CHECK(speed_of("ease_in") == doctest::Approx(180));
    CHECK(speed_of("smooth") == doctest::Approx(135));
}

TEST_CASE("kanim describe: a clip that does not move, and a clip with no tracks") {
    Result r = describe("duration = 300\n[borrow]\nmode = 5\n", "borrow.anim.toml", Options{});
    CHECK(r.code == 0);
    CHECK(r.text.find("tracks: 0") != std::string::npos);
    CHECK(r.text.find("borrow: mode 5, speed 1, offset 0 ms") != std::string::npos);
}

TEST_CASE("kanim sample: golden output, equal to the evaluator") {
    Options o;
    o.ability_ms = 1000;
    o.times = {0.0f, 0.35f, 0.5f, 1.0f};
    CHECK(sample(clip_text("design_example"), src("design_example"), o).text == golden("sample_design_example.txt"));
    Options l;
    l.times = {0.25f, 0.75f};
    CHECK(sample(clip_text("loop"), src("loop"), l).text == golden("sample_loop.txt"));

    // The numbers are the evaluator's own: build the same pose by hand.
    ClipLoad r = parse_clip(clip_text("design_example"), "design_example.anim.toml");
    REQUIRE(r.ok);
    Pose pose;
    Timing t;
    t.elapsed_ms = 0.25f * 1000;
    t.duration_ms = 1000;
    t.run_ms = 1000;
    apply_clip(r.clip, t, pose);
    char want[64];
    std::snprintf(want, sizeof want, "hand_r: rot %.2f", static_cast<double>(pose.parts[static_cast<int>(Part::HandR)].rot[0]));
    Options one;
    one.ability_ms = 1000;
    one.times = {0.25f};
    CHECK(sample(clip_text("design_example"), src("design_example"), one).text.find(want) != std::string::npos);
}

TEST_CASE("kanim sample: no --t gives start, middle and end") {
    Result r = sample(clip_text("loop"), src("loop"), Options{});
    CHECK(r.text.find("t 0 (0 ms)") != std::string::npos);
    CHECK(r.text.find("t 0.5 (250 ms)") != std::string::npos);
    CHECK(r.text.find("t 1 (500 ms)") != std::string::npos);
}

TEST_CASE("kanim bake: golden output, and the baked clip evaluates the same") {
    Options o;
    CHECK(bake(clip_text("design_example"), src("design_example"), o).text == golden("bake_design_example.txt"));
    CHECK(bake(clip_text("pops"), src("pops"), o).text == golden("bake_pops.txt"));

    for (const char* name : {"design_example", "pops", "loop"}) {
        CAPTURE(name);
        ClipLoad a = parse_clip(clip_text(name), "a");
        REQUIRE(a.ok);
        Result baked = bake(clip_text(name), "a", o);
        ClipLoad b = parse_clip(baked.text, "baked");
        INFO(baked.text);
        REQUIRE(b.ok);
        CHECK(a.clip.duration_from_ability == b.clip.duration_from_ability);
        CHECK(a.clip.duration_ms == b.clip.duration_ms);
        CHECK(a.clip.blend_in_ms == b.clip.blend_in_ms);
        CHECK(a.clip.blend_out_ms == b.clip.blend_out_ms);
        CHECK(a.clip.markers.size() == b.clip.markers.size());
        CHECK(a.clip.hides.size() == b.clip.hides.size());
        CHECK(a.clip.borrow.present == b.clip.borrow.present);
        CHECK(a.clip.borrow.mode == b.clip.borrow.mode);
        // Every sampled pose is the same, bit for bit.
        for (int i = 0; i <= 200; ++i) {
            Timing t;
            t.duration_ms = 1000;
            t.run_ms = 1000;
            t.elapsed_ms = static_cast<float>(i) * 5.0f;
            Pose pa, pb;
            apply_clip(a.clip, t, pa);
            apply_clip(b.clip, t, pb);
            for (int p = 0; p < kPartCount; ++p)
                for (int n = 0; n < 3; ++n) {
                    CHECK(pa.parts[p].pos[n] == pb.parts[p].pos[n]);
                    CHECK(pa.parts[p].rot[n] == pb.parts[p].rot[n]);
                }
        }
        // Baking a baked clip changes nothing but the label in the first line.
        std::string again = bake(baked.text, "a", o).text;
        CHECK(again.substr(again.find('\n')) == baked.text.substr(baked.text.find('\n')));
    }
}

TEST_CASE("kanim bake: a float comes back as the same float") {
    // 0.35f has no short exact form; the baked text must read back to the same bits.
    std::string text = "duration = 333.3\n[[track]]\npart = \"head\"\nchannel = \"pos\"\n"
                       "keys = [{ t = 0.35, v = [0.1, 1e-3, 99.123] }]\n";
    ClipLoad a = parse_clip(text, "a");
    REQUIRE(a.ok);
    ClipLoad b = parse_clip(bake(text, "a", Options{}).text, "b");
    REQUIRE(b.ok);
    CHECK(b.clip.duration_ms == a.clip.duration_ms);
    for (int n = 0; n < 3; ++n) CHECK(b.clip.tracks[0].keys[0].v[n] == a.clip.tracks[0].keys[0].v[n]);
    CHECK(b.clip.tracks[0].keys[0].t == a.clip.tracks[0].keys[0].t);
}

TEST_CASE("kanim: an invalid clip fails every command with code 1 and prints its errors") {
    Options o;
    for (Result r : {describe(clip_text("bad"), "bad.anim.toml", o), sample(clip_text("bad"), "bad.anim.toml", o),
                     bake(clip_text("bad"), "bad.anim.toml", o)}) {
        CHECK(r.code == 1);
        CHECK(r.text == golden("check_bad.txt"));
    }
    o.json = true;
    CHECK(describe(clip_text("bad"), "bad.anim.toml", o).text == golden("check_bad.json"));
    // Text that is not a clip at all. (Not `[[[track`: toml++ asserts on a header like that without NDEBUG; see notes.md, task 3.2.)
    Result junk = check("[[track", "junk.anim.toml", Options{});
    CHECK(junk.code == 1);
    CHECK(check("", "empty.anim.toml", Options{}).code == 1);   // `duration` is required
}

TEST_CASE("kanim: ease_slope is the slope of apply_ease") {
    for (Ease e : {Ease::Linear, Ease::Smooth, Ease::EaseIn, Ease::EaseOut}) {
        for (int i = 1; i < 100; ++i) {
            double f = i / 100.0, h = 1e-3;
            double diff = (static_cast<double>(apply_ease(e, static_cast<float>(f + h))) -
                           static_cast<double>(apply_ease(e, static_cast<float>(f - h)))) / (2 * h);
            CHECK_MESSAGE(std::fabs(diff - ease_slope(e, f)) < 0.01, ease_name(e) << " at " << f);
        }
    }
    CHECK(ease_slope(Ease::Hold, 0.5) == 0);
}

TEST_CASE("kanim: --t parsing") {
    std::vector<float> v;
    CHECK(parse_times("0,0.25,0.5", v));
    CHECK(v == std::vector<float>{0.0f, 0.25f, 0.5f});
    CHECK(parse_times("1", v));
    CHECK(v == std::vector<float>{1.0f});
    for (const char* bad : {"", ",", "0,", ",0", "1.5", "-0.1", "a", "0.5x", "0,,1", "nan"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_times(bad, v));
    }
}
