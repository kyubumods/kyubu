// Unit tests of the clip loader and validator (openspec add-ability-animation task 3.2). No game, no files.
#include "doctest.h"

#include <string>
#include <vector>

#include "animation_load.h"
#include "json.hpp"

using namespace kyubu::game::anim;

namespace {

const char* const kSource = "slam.anim.toml";

ClipLoad load(const std::string& text) { return parse_clip(text, kSource); }

// The example of design D2, as written there.
const char* const kDesignExample = R"(duration = "ability"          # or a number of ms
end = "hold"
blend_in = 60
blend_out = 120

[[track]]
part = "weapon_r"
channel = "rot"
mode = "add"
keys = [
  { t = 0.00, v = [0, 0, 0] },
  { t = 0.35, v = [-110, 0, 10], ease = "ease_out" },   # wind-up
  { t = 0.50, v = [30, 0, 0],    ease = "ease_in" },    # strike
  { t = 1.00, v = [0, 0, 0],     ease = "smooth" },
]

[[hide]]
from = 0.0
to = 0.2
parts = ["head"]

[[marker]]
t = 0.50
name = "hit"
)";

// A clip of 500 ms with one more table. Line 1 is the duration and line 2 the header, so `body` starts at line 3.
std::string in_table(const char* header, const std::string& body) {
    return std::string("duration = 500\n") + header + "\n" + body;
}

// A weapon_r track on lines 2 to 4, `keys = [` on line 5. `keys` holds one key a line, so the first key is line 6.
std::string with_keys(const std::string& keys, const char* channel = "rot") {
    return in_table("[[track]]", std::string("part = \"weapon_r\"\nchannel = \"") + channel + "\"\nkeys = [\n" + keys + "]\n");
}

std::string repeat(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

const char* const kOneKey = "  { t = 0, v = [0, 0, 0] },\n";
const char* const kOneTrackKeys = "keys = [{ t = 0, v = [0, 0, 0] }]\n";
const char* const kOneTrack = "[[track]]\npart = \"head\"\nchannel = \"rot\"\nkeys = [{ t = 0, v = [0, 0, 0] }]\n";
const char* const kOneHide = "[[hide]]\nfrom = 0\nto = 1\nparts = [\"head\"]\n";
const char* const kOneMarker = "[[marker]]\nt = 0.5\nname = \"hit\"\n";

// The one error of a bad clip, checked against the rules that every rejection follows.
ClipError one_error(const std::string& text) {
    ClipLoad r = load(text);
    CHECK_FALSE(r.ok);
    CHECK(r.clip.tracks.empty());
    CHECK(r.clip.hides.empty());
    CHECK(r.clip.markers.empty());
    CHECK_FALSE(r.clip.borrow.present);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].source == kSource);
    CHECK_FALSE(r.errors[0].message.empty());
    CHECK_FALSE(r.errors[0].hint.empty());
    return r.errors[0];
}

struct Bad {
    const char* what;
    std::string text;
    const char* code;
    const char* path;
    int line;
};

}  // namespace

TEST_CASE("anim load: the design's example loads with no error") {
    ClipLoad r = load(kDesignExample);
    REQUIRE(r.errors.empty());
    REQUIRE(r.ok);
    const Clip& c = r.clip;
    CHECK(c.duration_from_ability);
    CHECK(c.end == End::Hold);
    CHECK(c.blend_in_ms == 60);
    CHECK(c.blend_out_ms == 120);
    CHECK_FALSE(c.borrow.present);

    REQUIRE(c.tracks.size() == 1);
    const Track& tr = c.tracks[0];
    CHECK(tr.part == Part::HandR);   // `weapon_r` is another name of the hand
    CHECK(tr.channel == Channel::Rot);
    CHECK(tr.mode == TrackMode::Add);
    CHECK(tr.line == 6);
    REQUIRE(tr.keys.size() == 4);
    CHECK(tr.keys[0].t == 0);
    CHECK(tr.keys[0].ease == Ease::Linear);
    CHECK(tr.keys[1].t == doctest::Approx(0.35));
    CHECK(tr.keys[1].v[0] == -110);
    CHECK(tr.keys[1].v[1] == 0);
    CHECK(tr.keys[1].v[2] == 10);
    CHECK(tr.keys[1].ease == Ease::EaseOut);
    CHECK(tr.keys[1].line == 12);
    CHECK(tr.keys[2].ease == Ease::EaseIn);
    CHECK(tr.keys[3].t == 1);
    CHECK(tr.keys[3].ease == Ease::Smooth);

    REQUIRE(c.hides.size() == 1);
    CHECK(c.hides[0].from == 0);
    CHECK(c.hides[0].to == doctest::Approx(0.2));
    CHECK(c.hides[0].groups == kHideHead);
    CHECK(c.hides[0].line == 17);
    REQUIRE(c.markers.size() == 1);
    CHECK(c.markers[0].t == 0.5f);
    CHECK(c.markers[0].name == "hit");
    CHECK(c.markers[0].line == 22);

    // The loaded clip runs in the evaluator: on an ability of 1,000 ms the wind-up key is reached at 350 ms.
    Timing at350;
    at350.elapsed_ms = 350;
    at350.duration_ms = resolve_duration(c, 1000);
    Pose pose;
    CHECK(apply_clip(c, at350, pose).ok);
    CHECK(pose.parts[static_cast<int>(Part::HandR)].rot[0] == doctest::Approx(-110).epsilon(0.001));
    Timing at100 = at350;
    at100.elapsed_ms = 100;
    CHECK(hide_mask(c, at100) == kHideHead);
    std::vector<int> crossed = markers_crossed(c, 400, 600, 1000);
    REQUIRE(crossed.size() == 1);
    CHECK(crossed[0] == 0);
}

TEST_CASE("anim load: defaults, whole numbers and the other words") {
    ClipLoad r = load("duration = 500\n[[track]]\npart = \"head\"\nchannel = \"pos\"\nkeys = [{ t = 1, v = [1, 2.5, -3] }]\n");
    REQUIRE(r.ok);
    CHECK_FALSE(r.clip.duration_from_ability);
    CHECK(r.clip.duration_ms == 500);
    CHECK(r.clip.end == End::Hold);
    CHECK(r.clip.blend_in_ms == 0);
    CHECK(r.clip.blend_out_ms == 0);
    REQUIRE(r.clip.tracks.size() == 1);
    CHECK(r.clip.tracks[0].part == Part::Head);
    CHECK(r.clip.tracks[0].channel == Channel::Pos);
    CHECK(r.clip.tracks[0].mode == TrackMode::Add);
    REQUIRE(r.clip.tracks[0].keys.size() == 1);
    const Key& k = r.clip.tracks[0].keys[0];
    CHECK(k.t == 1);
    CHECK(k.v[0] == 1);
    CHECK(k.v[1] == 2.5f);
    CHECK(k.v[2] == -3);
    CHECK(k.ease == Ease::Linear);
    CHECK(k.line == 5);

    r = load("duration = 250.5\nend = \"loop\"\n[[track]]\npart = \"root\"\nchannel = \"rot\"\nmode = \"set\"\n"
             "keys = [{ t = 0, v = [0, 0, 0], ease = \"hold\" }]\n");
    REQUIRE(r.ok);
    CHECK(r.clip.duration_ms == 250.5f);
    CHECK(r.clip.end == End::Loop);
    CHECK(r.clip.tracks[0].mode == TrackMode::Set);
    CHECK(r.clip.tracks[0].keys[0].ease == Ease::Hold);

    // A clip with no tracks is valid: it changes nothing (spec "An empty clip").
    r = load("duration = 500\n");
    CHECK(r.ok);
    CHECK(r.clip.tracks.empty());
}

TEST_CASE("anim load: one bad file per message") {
    const std::string track_head = "part = \"head\"\nchannel = \"rot\"\n";
    const std::string one_key_line = "keys = [{ t = 0, v = [0, 0, 0] }]\n";
    const std::string hide_head = "from = 0\nto = 1\n";
    const std::vector<Bad> cases = {
        {"a syntax error", "duration = \n", "toml_syntax", "", 1},
        {"too much text", "duration = 500\n#" + std::string(kMaxTextBytes, 'x') + "\n", "too_large", "", 0},

        {"an unknown key at the top", "duration = 500\nspeed = 2\n", "unknown_key", "speed", 2},
        {"an unknown key in a track", in_table("[[track]]", track_head + one_key_line + "loop = true\n"), "unknown_key",
         "track[0].loop", 6},
        {"an unknown key in a key", with_keys("  { t = 0, v = [0, 0, 0], easing = \"smooth\" },\n"), "unknown_key",
         "track[0].keys[0].easing", 6},
        {"an unknown key in a hide", in_table("[[hide]]", hide_head + "parts = [\"head\"]\ngroups = 1\n"), "unknown_key",
         "hide[0].groups", 6},
        {"an unknown key in a marker", in_table("[[marker]]", "t = 0.5\nname = \"hit\"\nlabel = \"x\"\n"), "unknown_key",
         "marker[0].label", 5},
        {"an unknown key in borrow", in_table("[borrow]", "mode = 57\nrate = 2\n"), "unknown_key", "borrow.rate", 4},

        {"no duration", "end = \"hold\"\n", "missing_field", "duration", 0},
        {"no part", in_table("[[track]]", "channel = \"rot\"\n" + one_key_line), "missing_field", "track[0].part", 2},
        {"no channel", in_table("[[track]]", "part = \"head\"\n" + one_key_line), "missing_field", "track[0].channel", 2},
        {"no keys", in_table("[[track]]", track_head), "missing_field", "track[0].keys", 2},
        {"a key with no t", with_keys("  { v = [0, 0, 0] },\n"), "missing_field", "track[0].keys[0].t", 6},
        {"a key with no v", with_keys("  { t = 0 },\n"), "missing_field", "track[0].keys[0].v", 6},
        {"a hide with no from", in_table("[[hide]]", "to = 1\nparts = [\"head\"]\n"), "missing_field", "hide[0].from", 2},
        {"a hide with no to", in_table("[[hide]]", "from = 0\nparts = [\"head\"]\n"), "missing_field", "hide[0].to", 2},
        {"a hide with no parts", in_table("[[hide]]", hide_head), "missing_field", "hide[0].parts", 2},
        {"a marker with no t", in_table("[[marker]]", "name = \"hit\"\n"), "missing_field", "marker[0].t", 2},
        {"a marker with no name", in_table("[[marker]]", "t = 0.5\n"), "missing_field", "marker[0].name", 2},
        {"a borrow with no mode", in_table("[borrow]", "speed = 2\n"), "missing_field", "borrow.mode", 2},

        {"a boolean duration", "duration = true\n", "bad_type", "duration", 1},
        {"a number for end", "duration = 500\nend = 3\n", "bad_type", "end", 2},
        {"a string for blend_in", "duration = 500\nblend_in = \"fast\"\n", "bad_type", "blend_in", 2},
        {"a number for track", "duration = 500\ntrack = 5\n", "bad_type", "track", 2},
        {"one bracket on [track]", in_table("[track]", track_head + one_key_line), "bad_type", "track", 2},
        {"a number for part", in_table("[[track]]", "part = 3\nchannel = \"rot\"\n" + one_key_line), "bad_type", "track[0].part", 3},
        {"a number for keys", in_table("[[track]]", track_head + "keys = 4\n"), "bad_type", "track[0].keys", 5},
        {"a key that is a number", with_keys("  1,\n"), "bad_type", "track[0].keys[0]", 6},
        {"a string for t", with_keys("  { t = \"0\", v = [0, 0, 0] },\n"), "bad_type", "track[0].keys[0].t", 6},
        {"a number for v", with_keys("  { t = 0, v = 5 },\n"), "bad_type", "track[0].keys[0].v", 6},
        {"a string in v", with_keys("  { t = 0, v = [0, \"up\", 0] },\n"), "bad_type", "track[0].keys[0].v[1]", 6},
        {"a string for parts", in_table("[[hide]]", hide_head + "parts = \"head\"\n"), "bad_type", "hide[0].parts", 5},
        {"a number in parts", in_table("[[hide]]", hide_head + "parts = [\"head\", 3]\n"), "bad_type", "hide[0].parts[1]", 5},
        {"a number for a marker's name", in_table("[[marker]]", "t = 0.5\nname = 5\n"), "bad_type", "marker[0].name", 4},
        {"a number for borrow", "duration = 500\nborrow = 3\n", "bad_type", "borrow", 2},
        {"a fraction for the borrowed mode", in_table("[borrow]", "mode = 5.5\n"), "bad_type", "borrow.mode", 3},

        {"a v of two numbers", with_keys("  { t = 0, v = [0, 0] },\n"), "bad_length", "track[0].keys[0].v", 6},

        {"an infinite duration", "duration = inf\n", "not_finite", "duration", 1},
        {"a NaN t", with_keys("  { t = nan, v = [0, 0, 0] },\n"), "not_finite", "track[0].keys[0].t", 6},
        {"a NaN in v", with_keys("  { t = 0, v = [0, 0, nan] },\n"), "not_finite", "track[0].keys[0].v[2]", 6},
        {"an infinity in v", with_keys("  { t = 0, v = [-inf, 0, 0] },\n"), "not_finite", "track[0].keys[0].v[0]", 6},
        {"a NaN blend_out", "duration = 500\nblend_out = nan\n", "not_finite", "blend_out", 2},
        {"an infinite borrow speed", in_table("[borrow]", "mode = 5\nspeed = inf\n"), "not_finite", "borrow.speed", 4},

        {"a duration of 0", "duration = 0\n", "out_of_range", "duration", 1},
        {"a negative duration", "duration = -5\n", "out_of_range", "duration", 1},
        {"a duration over the bound", "duration = 600001\n", "out_of_range", "duration", 1},
        {"a negative blend_in", "duration = 500\nblend_in = -1\n", "out_of_range", "blend_in", 2},
        {"a blend_out over the bound", "duration = 500\nblend_out = 60001\n", "out_of_range", "blend_out", 2},
        {"a t above 1", with_keys(std::string(kOneKey) + "  { t = 1.4, v = [0, 0, 0] },\n"), "out_of_range", "track[0].keys[1].t",
         7},
        {"a t below 0", with_keys("  { t = -0.1, v = [0, 0, 0] },\n"), "out_of_range", "track[0].keys[0].t", 6},
        {"a rotation beyond the bound", with_keys("  { t = 0, v = [0, -721, 0] },\n"), "out_of_range", "track[0].keys[0].v[1]", 6},
        {"an offset beyond the bound", with_keys("  { t = 0, v = [100.5, 0, 0] },\n", "pos"), "out_of_range",
         "track[0].keys[0].v[0]", 6},
        {"a hide that starts before the clip", in_table("[[hide]]", "from = -0.1\nto = 1\nparts = [\"head\"]\n"), "out_of_range",
         "hide[0].from", 3},
        {"a hide that ends after the clip", in_table("[[hide]]", "from = 0\nto = 1.5\nparts = [\"head\"]\n"), "out_of_range",
         "hide[0].to", 4},
        {"a marker outside the clip", in_table("[[marker]]", "t = 1.2\nname = \"hit\"\n"), "out_of_range", "marker[0].t", 3},
        {"a borrowed mode above 110", in_table("[borrow]", "mode = 111\n"), "out_of_range", "borrow.mode", 3},
        {"a negative borrowed mode", in_table("[borrow]", "mode = -1\n"), "out_of_range", "borrow.mode", 3},
        {"a borrow speed over the bound", in_table("[borrow]", "mode = 5\nspeed = 11\n"), "out_of_range", "borrow.speed", 4},
        {"a borrow offset over the bound", in_table("[borrow]", "mode = 5\noffset_ms = 600001\n"), "out_of_range",
         "borrow.offset_ms", 4},

        {"a word for the duration", "duration = \"forever\"\n", "unknown_duration", "duration", 1},
        {"an unknown end", "duration = 500\nend = \"stop\"\n", "unknown_end", "end", 2},
        {"an unknown part", in_table("[[track]]", "part = \"elbow\"\nchannel = \"rot\"\n" + one_key_line), "unknown_part",
         "track[0].part", 3},
        {"an unknown channel", in_table("[[track]]", "part = \"head\"\nchannel = \"scale\"\n" + one_key_line), "unknown_channel",
         "track[0].channel", 4},
        {"an unknown mode", in_table("[[track]]", track_head + "mode = \"mul\"\n" + one_key_line), "unknown_mode", "track[0].mode",
         5},
        {"an unknown ease", with_keys("  { t = 0, v = [0, 0, 0], ease = \"bounce\" },\n"), "unknown_ease", "track[0].keys[0].ease",
         6},
        {"an unknown group", in_table("[[hide]]", hide_head + "parts = [\"head\", \"legs\"]\n"), "unknown_group", "hide[0].parts[1]",
         5},

        {"keys out of order", with_keys("  { t = 0.5, v = [0, 0, 0] },\n  { t = 0.3, v = [0, 0, 0] },\n"), "keys_out_of_order",
         "track[0].keys[1].t", 7},
        {"a track with no key", in_table("[[track]]", track_head + "keys = []\n"), "no_keys", "track[0].keys", 5},
        {"a hide with no group", in_table("[[hide]]", hide_head + "parts = []\n"), "no_parts", "hide[0].parts", 5},
        {"a hide of no length", in_table("[[hide]]", "from = 0.5\nto = 0.5\nparts = [\"head\"]\n"), "empty_interval", "hide[0].to",
         4},
        {"a hide that ends before it starts", in_table("[[hide]]", "from = 0.5\nto = 0.2\nparts = [\"head\"]\n"), "empty_interval",
         "hide[0].to", 4},
        {"a marker with an empty name", in_table("[[marker]]", "t = 0.5\nname = \"\"\n"), "bad_name", "marker[0].name", 4},
        {"a marker with a long name", in_table("[[marker]]", "t = 0.5\nname = \"" + std::string(kMaxMarkerName + 1, 'a') + "\"\n"),
         "bad_name", "marker[0].name", 4},

        {"more keys than the cap", with_keys(repeat(kOneKey, kMaxKeysPerTrack + 1)), "too_many_keys", "track[0].keys", 5},
        {"more tracks than the cap", "duration = 500\n" + repeat(kOneTrack, kMaxTracks + 1), "too_many_tracks", "track", 2},
        {"more hides than the cap", "duration = 500\n" + repeat(kOneHide, kMaxHides + 1), "too_many_hides", "hide", 2},
        {"more markers than the cap", "duration = 500\n" + repeat(kOneMarker, kMaxMarkers + 1), "too_many_markers", "marker", 2},
    };
    for (const Bad& b : cases) {
        CAPTURE(b.what);
        ClipError e = one_error(b.text);
        CHECK(e.code == b.code);
        CHECK(e.path == b.path);
        CHECK(e.line == b.line);
    }
}

TEST_CASE("anim load: a bad key names the file, the line and the field path") {
    // The design's example with the wind-up key moved to t = 1.4 (line 12).
    std::string text = kDesignExample;
    size_t at = text.find("t = 0.35");
    REQUIRE(at != std::string::npos);
    text.replace(at, 8, "t = 1.40");
    ClipError e = one_error(text);
    CHECK(e.code == "out_of_range");
    CHECK(format_error(e) == "slam.anim.toml:12: track[0].keys[1].t: `t` is 1.4; it must be from 0 to 1");
}

TEST_CASE("anim load: an unknown name lists the known ones") {
    ClipError e = one_error(in_table("[[track]]", "part = \"elbow\"\nchannel = \"rot\"\nkeys = [{ t = 0, v = [0, 0, 0] }]\n"));
    CHECK(e.message.find("\"elbow\"") != std::string::npos);
    for (int i = 0; i < kPartCount; ++i) {
        CAPTURE(i);
        CHECK(e.message.find(part_name(static_cast<Part>(i))) != std::string::npos);
        CHECK(e.hint.find(part_name(static_cast<Part>(i))) != std::string::npos);
    }

    e = one_error(with_keys("  { t = 0, v = [0, 0, 0], ease = \"bounce\" },\n"));
    for (const char* name : {"linear", "smooth", "ease_in", "ease_out", "hold"}) CHECK(e.message.find(name) != std::string::npos);
    e = one_error(in_table("[[hide]]", "from = 0\nto = 1\nparts = [\"legs\"]\n"));
    for (const char* name : {"head", "hair", "body", "shoulders", "weapons"}) CHECK(e.message.find(name) != std::string::npos);
    e = one_error(in_table("[[track]]", "part = \"head\"\nchannel = \"scale\"\nkeys = [{ t = 0, v = [0, 0, 0] }]\n"));
    CHECK(e.message.find("pos, rot") != std::string::npos);

    // The feet, shoulders and wings have no pose vector in the game: they are not parts.
    for (const char* name : {"foot_r", "foot_l", "shoulder_r", "shoulder_l", "wings"}) {
        CAPTURE(name);
        e = one_error(in_table("[[track]]", std::string("part = \"") + name + "\"\nchannel = \"rot\"\n" + kOneTrackKeys));
        CHECK(e.code == "unknown_part");
    }

    // A long name from the file is cut in the message.
    const std::string long_name(500, 'x');
    e = one_error(in_table("[[track]]", "part = \"" + long_name + "\"\nchannel = \"rot\"\nkeys = [{ t = 0, v = [0, 0, 0] }]\n"));
    CHECK(e.message.size() < 300);
}

TEST_CASE("anim load: every error of a clip is listed, in the order of the lines") {
    ClipLoad r = load("duration = \"forever\"\n"                 // 1
                      "end = \"stop\"\n"                         // 2
                      "[[track]]\n"                              // 3
                      "part = \"elbow\"\n"                       // 4
                      "channel = \"rot\"\n"                      // 5
                      "keys = [\n"                               // 6
                      "  { t = 1.4, v = [0, 0, 0] },\n"          // 7
                      "  { t = 0.2, v = [0, 900, 0] },\n"        // 8
                      "]\n"                                      // 9
                      "[[marker]]\n"                             // 10
                      "t = 2\n"                                  // 11
                      "name = \"hit\"\n");                       // 12
    CHECK_FALSE(r.ok);
    CHECK(r.clip.tracks.empty());
    CHECK(r.clip.markers.empty());
    CHECK(r.clip.duration_ms == 0);
    REQUIRE(r.errors.size() == 6);
    const int lines[] = {1, 2, 4, 7, 8, 11};
    const char* const codes[] = {"unknown_duration", "unknown_end", "unknown_part", "out_of_range", "out_of_range", "out_of_range"};
    const char* const paths[] = {"duration", "end", "track[0].part", "track[0].keys[0].t", "track[0].keys[1].v[1]", "marker[0].t"};
    for (int i = 0; i < 6; ++i) {
        CAPTURE(i);
        CHECK(r.errors[i].line == lines[i]);
        CHECK(r.errors[i].code == codes[i]);
        CHECK(r.errors[i].path == paths[i]);
    }

    // A clip with a hundred bad keys lists kMaxErrors of them.
    r = load(with_keys(repeat("  { t = 2, v = [0, 0, 0] },\n", 100)));
    CHECK_FALSE(r.ok);
    CHECK(static_cast<int>(r.errors.size()) == kMaxErrors);
}

TEST_CASE("anim load: the bounds and the caps are inclusive, and two keys may share a t") {
    CHECK(load(with_keys("  { t = 0, v = [720, -720, 0] },\n  { t = 1, v = [0, 0, 0] },\n")).ok);
    CHECK(load(with_keys("  { t = 0, v = [100, -100, 0] },\n", "pos")).ok);
    CHECK(load("duration = 600000\nblend_in = 60000\nblend_out = 0\n").ok);
    CHECK(load(in_table("[[hide]]", "from = 0\nto = 1\nparts = [\"head\", \"hair\", \"body\", \"shoulders\", \"weapons\"]\n")).ok);
    const std::string longest_name(kMaxMarkerName, 'b');
    CHECK(load("duration = 500\n[[marker]]\nt = 0\nname = \"a\"\n[[marker]]\nt = 1\nname = \"" + longest_name + "\"\n").ok);

    ClipLoad r = load(with_keys("  { t = 0.5, v = [0, 0, 0] },\n  { t = 0.5, v = [10, 0, 0] },\n"));
    REQUIRE(r.ok);
    CHECK(r.clip.tracks[0].keys.size() == 2);

    r = load(with_keys(repeat(kOneKey, kMaxKeysPerTrack)));
    REQUIRE(r.ok);
    CHECK(static_cast<int>(r.clip.tracks[0].keys.size()) == kMaxKeysPerTrack);
    r = load("duration = 500\n" + repeat(kOneTrack, kMaxTracks) + repeat(kOneHide, kMaxHides) + repeat(kOneMarker, kMaxMarkers));
    REQUIRE(r.ok);
    CHECK(static_cast<int>(r.clip.tracks.size()) == kMaxTracks);
    CHECK(static_cast<int>(r.clip.hides.size()) == kMaxHides);
    CHECK(static_cast<int>(r.clip.markers.size()) == kMaxMarkers);
    CHECK(r.clip.tracks[1].line == 6);   // each track is four lines, the first header is line 2
}

TEST_CASE("anim load: a borrowed look") {
    // A clip with a [borrow] table and no tracks is valid (phase 1). Spec "Retimed": 150 + 500 * 2.
    ClipLoad r = load("duration = 500\n[borrow]\nmode = 57\nspeed = 2\noffset_ms = 150\n");
    REQUIRE(r.ok);
    CHECK(r.clip.tracks.empty());
    CHECK(r.clip.borrow.present);
    CHECK(r.clip.borrow.mode == 57);
    CHECK(borrowed_mode_time(r.clip, 500) == 1150);

    r = load("duration = \"ability\"\n[borrow]\nmode = 110\n");
    REQUIRE(r.ok);
    CHECK(r.clip.borrow.mode == kMaxBorrowMode);
    CHECK(r.clip.borrow.speed == 1);
    CHECK(r.clip.borrow.offset_ms == 0);

    // A held frame, and a start before the mode's own: the evaluator keeps the time at 0 or above.
    r = load("duration = 500\n[borrow]\nmode = 0\nspeed = 0\noffset_ms = -200\n" + std::string(kOneHide));
    REQUIRE(r.ok);
    CHECK(r.clip.borrow.speed == 0);
    CHECK(r.clip.borrow.offset_ms == -200);
    CHECK(borrowed_mode_time(r.clip, 500) == 0);
    CHECK(r.clip.hides.size() == 1);
}

TEST_CASE("anim load: text that is not a clip is rejected, never thrown") {
    const std::vector<std::string> texts = {
        "",
        std::string("\0\xff\xfe\x01", 4),
        "[[track",
        "[track]]",
        "= = =",
        "duration = 500\nx = " + std::string(5000, '['),
        "duration = 500\ntrack = [[[[1]]]]\n",
        "[[track]]\n[[track.keys]]\n",
    };
    for (size_t i = 0; i < texts.size(); ++i) {
        CAPTURE(i);
        ClipLoad r = load(texts[i]);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.errors.empty());
        for (const ClipError& e : r.errors) CHECK_FALSE(format_error(e).empty());
        CHECK_FALSE(errors_to_json(r).empty());
    }
}

TEST_CASE("anim load: the log line and the JSON form") {
    ClipError e{"a.toml", 7, "track[0].part", "unknown_part", "the message", "the hint"};
    CHECK(format_error(e) == "a.toml:7: track[0].part: the message");
    e.line = 0;
    CHECK(format_error(e) == "a.toml: track[0].part: the message");
    e.line = 3;
    e.path.clear();
    CHECK(format_error(e) == "a.toml:3: the message");

    CHECK(errors_to_json(load(kDesignExample)) == "{\"ok\":true,\"errors\":[]}");

    // Spec "A fix hint": each error of the JSON form has one.
    ClipLoad r = load(with_keys(std::string(kOneKey) + "  { t = 1.4, v = [0, 0, 0], ease = \"bounce\" },\n"));
    REQUIRE(r.errors.size() == 2);
    std::string text = errors_to_json(r);
    CHECK(text.find('\n') == std::string::npos);
    CHECK(text.rfind("{\"ok\":false,\"errors\":[{\"source\":\"slam.anim.toml\",\"line\":7,\"path\":\"track[0].keys[1].t\","
                     "\"code\":\"out_of_range\",\"message\":",
                     0) == 0);
    auto j = nlohmann::json::parse(text, nullptr, false);
    REQUIRE(j.is_object());
    CHECK(j["ok"] == false);
    REQUIRE(j["errors"].size() == 2);
    for (const auto& je : j["errors"]) {
        CHECK(je.size() == 6);
        CHECK(je["source"] == kSource);
        CHECK(je["line"] == 7);
        CHECK(je["message"].is_string());
        REQUIRE(je["hint"].is_string());
        CHECK_FALSE(je["hint"].get<std::string>().empty());
    }
    CHECK(j["errors"][1]["code"] == "unknown_ease");
    CHECK(j["errors"][1]["path"] == "track[0].keys[1].ease");

    // A source name that is not UTF-8 still gives JSON.
    j = nlohmann::json::parse(errors_to_json(parse_clip("duration = 0\n", "bad\xff.toml")), nullptr, false);
    CHECK(j.is_object());
}
