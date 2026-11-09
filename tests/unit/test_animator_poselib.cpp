// Unit tests of the Animator's pose library (openspec add-ability-animation task 6.8): the baked pose file, the library folder, the
// base pose of a `[borrow]` clip at the retimed clock, Capture at t and Bake to keys. No game, no window: the baked poses are
// hand-made fixtures with made-up numbers (a real one is derived from the game and is never in the repository).
#include "doctest.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "poselib.h"

using namespace kanimator;
namespace fs = std::filesystem;

namespace {

const char* kParts[6] = {"root", "body", "head", "hand_r", "hand_l", "tail"};

// The vectors of part `i` at fixture time `t_ms`: every number differs by part, channel, axis and time, and is linear in the time, so
// a sample between two frames has a value that can be written down.
float fx_pos(int i, int axis, double t_ms) { return float(i * 10 + axis + t_ms * 0.01 * (axis + 1)); }
float fx_rot(int i, int axis, double t_ms) { return float(-100 + i * 7 + axis * 3 + t_ms * 0.1 * (axis + 1)); }

std::string vec_json(const float* v, int n) {
    std::string s = "[";
    for (int k = 0; k < n; ++k) {
        char b[48];
        std::snprintf(b, sizeof b, "%s%.4f", k ? ", " : "", v[k]);
        s += b;
    }
    return s + "]";
}

std::string frame_json(double t_ms, int pose_id = 57, int index = 0) {
    std::string s = "{\"index\": " + std::to_string(index) + ", \"t_ms\": " + std::to_string(t_ms) +
                    ", \"view\": \"live\", \"body_yaw\": 0, \"pose_id\": " + std::to_string(pose_id) +
                    ", \"mode_time_ms\": " + std::to_string(int(t_ms)) + ", \"vectors\": {";
    for (int i = 0; i < 6; ++i) {
        float pos[3] = {fx_pos(i, 0, t_ms), fx_pos(i, 1, t_ms), fx_pos(i, 2, t_ms)};
        float rot[3] = {fx_rot(i, 0, t_ms), fx_rot(i, 1, t_ms), fx_rot(i, 2, t_ms)};
        if (i) s += ", ";
        s += std::string("\"") + kParts[i] + "\": {";
        if (i != 5) s += "\"pos\": " + vec_json(pos, i == 0 ? 2 : 3) + ", ";   // the root has x and y, the tail none
        s += "\"rot\": " + vec_json(rot, 3) + "}";
    }
    return s + "}}";
}

std::string pose_file(const std::vector<double>& times, int pose_id = 57, const std::string& kind = "bake",
                      const std::string& format = "kyubu-capture/1") {
    std::string s = "{\n  \"format\": \"" + format + "\",\n  \"kind\": \"" + kind +
                    "\",\n  \"clip\": \"anim_capture:bake_pose" + std::to_string(pose_id) + "\",\n  \"build\": \"cube-0xFIXTURE\",\n  \"frames\": [";
    for (size_t k = 0; k < times.size(); ++k) s += (k ? ",\n    " : "\n    ") + frame_json(times[k], pose_id, int(k));
    return s + "\n  ]\n}\n";
}

BakedPose make_pose(const std::vector<double>& times, int pose_id = 57) {
    BakedLoad l = parse_baked_pose(pose_file(times, pose_id), "fixture");
    REQUIRE(l.ok);
    return l.pose;
}

std::vector<double> every(double step, double to) {
    std::vector<double> t;
    for (double x = 0; x <= to + 1e-9; x += step) t.push_back(x);
    return t;
}

anim::Clip parse_ok(const std::string& text) {
    anim::ClipLoad c = anim::parse_clip(text, "fixture");
    INFO(text);
    REQUIRE(c.ok);
    return c.clip;
}

// A track of the parsed clip by part, channel and mode, or null.
const anim::Track* track_of(const anim::Clip& c, anim::Part p, anim::Channel ch, anim::TrackMode m = anim::TrackMode::Set) {
    for (const anim::Track& t : c.tracks)
        if (t.part == p && t.channel == ch && t.mode == m) return &t;
    return nullptr;
}

const char* kEmptyClip = "duration = 1000\nend = \"hold\"\n";

// A scratch folder that is removed at the end of the test.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        path = fs::temp_directory_path() / ("kyubu-poselib-" + name);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void write(const std::string& rel, const std::string& text) const {
        fs::path p = path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }
};

}  // namespace

TEST_CASE("poselib: a baked pose file is read") {
    BakedLoad l = parse_baked_pose(pose_file({0, 100, 250}), "pose.json");
    REQUIRE(l.ok);
    CHECK(l.pose.pose_id == 57);
    CHECK(l.pose.build == "cube-0xFIXTURE");
    CHECK(l.pose.source == "pose.json");
    REQUIRE(l.pose.frames.size() == 3);
    CHECK(l.pose.length_ms() == 250);
    const BakedFrame& f = l.pose.frames[1];
    CHECK(f.t_ms == 100);
    CHECK(f.pose.parts[int(anim::Part::Head)].rot[1] == doctest::Approx(fx_rot(2, 1, 100)).epsilon(1e-4));
    CHECK(f.pose.parts[int(anim::Part::HandL)].pos[2] == doctest::Approx(fx_pos(4, 2, 100)).epsilon(1e-4));
    // The root has x and y, and its z stays 0; the tail has no position.
    CHECK(f.pose.parts[int(anim::Part::Root)].pos[1] == doctest::Approx(fx_pos(0, 1, 100)).epsilon(1e-4));
    CHECK(f.pose.parts[int(anim::Part::Root)].pos[2] == 0);
    for (int k = 0; k < 3; ++k) CHECK(f.pose.parts[int(anim::Part::Tail)].pos[k] == 0);
    CHECK(f.pose.parts[int(anim::Part::Tail)].rot[0] == doctest::Approx(fx_rot(5, 0, 100)).epsilon(1e-4));
}

TEST_CASE("poselib: a null number reads as 0, a bad file is refused with its field") {
    std::string text = pose_file({0, 100});
    // The mod writes `null` for a number that is not finite.
    const size_t at = text.find("\"rot\": [");
    REQUIRE(at != std::string::npos);
    const size_t num = at + 8;
    text.replace(num, text.find(',', num) - num, "null");
    BakedLoad l = parse_baked_pose(text, "pose.json");
    REQUIRE(l.ok);
    CHECK(l.pose.frames[0].pose.parts[0].rot[0] == 0);

    struct Bad {
        std::string text;
        const char* names;   // what the message must name
    };
    std::string missing_part = pose_file({0, 100});
    missing_part.replace(missing_part.find("\"hand_l\""), 8, "\"hand_x\"");
    const std::vector<Bad> bad = {
        {"not json", "not a JSON object"},
        {pose_file({0}, 57, "bake", "kyubu-capture/9"), "format"},
        {pose_file({0, 50}, 57, "mystery"), "kind"},
        {"{\"format\": \"kyubu-capture/1\", \"kind\": \"bake\", \"build\": \"b\", \"frames\": []}", "frames"},
        {pose_file({100, 0}), "time order"},
        {pose_file({0}, 74), "pose_id"},
        {missing_part, "hand_l"},
    };
    for (const Bad& b : bad) {
        INFO(b.names);
        BakedLoad r = parse_baked_pose(b.text, "x.json");
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.other_kind);
        CHECK(r.error.find("x.json") == 0);
        CHECK(r.error.find(b.names) != std::string::npos);
    }
    // Frames of two poses are not one bake.
    std::string mixed = pose_file({0, 100});
    const size_t second = mixed.rfind("\"pose_id\": 57");
    mixed.replace(second, 13, "\"pose_id\": 58");
    BakedLoad m = parse_baked_pose(mixed, "x.json");
    CHECK_FALSE(m.ok);
    CHECK(m.error.find("one pose") != std::string::npos);
}

TEST_CASE("poselib: a rig or a capture file is another kind, not an error") {
    for (const char* kind : {"rig", "capture", "bake-replay"}) {
        BakedLoad r = parse_baked_pose(pose_file({0}, 0, kind), "x.json");
        CHECK_FALSE(r.ok);
        CHECK(r.other_kind);
    }
}

TEST_CASE("poselib: a sample is linear between draws and held outside them") {
    BakedPose p = make_pose({0, 100, 300});
    bool past = true;
    anim::Pose a = sample_baked(p, 0, &past);
    CHECK_FALSE(past);
    CHECK(a.parts[2].rot[0] == doctest::Approx(fx_rot(2, 0, 0)).epsilon(1e-4));
    // Half-way between 100 and 300 ms.
    anim::Pose mid = sample_baked(p, 200, &past);
    CHECK_FALSE(past);
    CHECK(mid.parts[2].rot[0] == doctest::Approx(fx_rot(2, 0, 200)).epsilon(1e-3));
    CHECK(mid.parts[1].pos[2] == doctest::Approx(fx_pos(1, 2, 200)).epsilon(1e-3));
    // Before the start and after the end the pose is held.
    anim::Pose before = sample_baked(p, -50, &past);
    CHECK(before.parts[2].rot[0] == doctest::Approx(a.parts[2].rot[0]));
    anim::Pose after = sample_baked(p, 900, &past);
    CHECK(past);
    CHECK(after.parts[2].rot[0] == doctest::Approx(fx_rot(2, 0, 300)).epsilon(1e-4));
    sample_baked(p, 300, &past);
    CHECK_FALSE(past);   // exactly the end is not past it
    // One frame: that frame at any time.
    BakedPose one = make_pose({0});
    CHECK(sample_baked(one, 5000).parts[0].rot[0] == doctest::Approx(fx_rot(0, 0, 0)).epsilon(1e-4));
}

TEST_CASE("poselib: the library reads a folder, the later stamp wins, other kinds pass") {
    TempDir d("scan");
    d.write("captures/bake_pose18-20261011-090000/pose.json", pose_file({0, 100, 200}, 18));
    d.write("captures/bake_pose18-20261011-100000/pose.json", pose_file({0, 100, 200, 300, 400}, 18));   // the later one: 5 frames
    d.write("captures/bake_pose3-20261011-090000/pose.json", pose_file({0, 40}, 3));
    d.write("captures/bake_pose3-20261011-090000/replay.json", "{ this file is not read");
    d.write("captures/rig-20261011-090000/pose.json", pose_file({0}, 0, "rig"));
    d.write("bake_pose61-20261011-090000/pose.json", pose_file({0, 10}, 61));               // straight under the folder
    d.write("captures/bake_pose9-20261011-090000/pose.json", pose_file({0, 10}, 9, "bake", "kyubu-capture/9"));   // a bad one
    d.write("captures/notes.txt", "x");

    PoseLibrary lib;
    PoseLibrary::Scan s = lib.scan(d.path.string());
    CHECK(s.loaded == 3);
    CHECK(s.replaced == 1);
    REQUIRE(s.problems.size() == 1);
    CHECK(s.problems[0].find("bake_pose9") != std::string::npos);
    CHECK(s.problems[0].find("format") != std::string::npos);
    CHECK(lib.ids() == std::vector<int>({3, 18, 61}));
    REQUIRE(lib.find(18) != nullptr);
    CHECK(lib.find(18)->frames.size() == 5);
    CHECK(lib.find(0) == nullptr);   // the rig is not a pose
    CHECK(lib.find(9) == nullptr);
    CHECK(lib.folder() == d.path.string());
}

TEST_CASE("poselib: a folder that is missing or not set gives an empty library and a problem") {
    PoseLibrary lib;
    PoseLibrary::Scan none = lib.scan("");
    CHECK(none.loaded == 0);
    REQUIRE(none.problems.size() == 1);
    CHECK(none.problems[0].find("no pose library folder") != std::string::npos);
    PoseLibrary::Scan gone = lib.scan((fs::temp_directory_path() / "kyubu-poselib-no-such-folder").string());
    CHECK(gone.loaded == 0);
    REQUIRE(gone.problems.size() == 1);
    CHECK(gone.problems[0].find("does not exist") != std::string::npos);
    // A scan replaces what the library held.
    TempDir d("replace");
    d.write("bake_pose5-1/pose.json", pose_file({0, 10}, 5));
    CHECK(lib.scan(d.path.string()).loaded == 1);
    CHECK(lib.scan("").loaded == 0);
    CHECK(lib.size() == 0);
}

TEST_CASE("poselib: a borrowed clip is drawn over its baked frames at the retimed clock") {
    PoseLibrary lib;
    lib.add(make_pose(every(50, 1000), 57));   // 0 .. 1000 ms in 50 ms draws

    // No borrow: the neutral pose, and nothing to say.
    anim::Clip plain = parse_ok(kEmptyClip);
    BaseResult n = base_pose_for(lib, plain, 300);
    CHECK(n.kind == BaseResult::Kind::Neutral);
    CHECK(base_label(n).empty());
    CHECK(n.pose.parts[2].rot[0] == 0);

    // Speed 2 and an offset of 100 ms: 100 + 150 * 2 = 400 ms into the pose.
    anim::Clip b = parse_ok("duration = 1000\n[borrow]\nmode = 57\nspeed = 2.0\noffset_ms = 100\n");
    BaseResult r = base_pose_for(lib, b, 150);
    REQUIRE(r.kind == BaseResult::Kind::Borrowed);
    CHECK(r.pose_id == 57);
    CHECK(r.pose_ms == doctest::Approx(400));
    CHECK_FALSE(r.past_end);
    CHECK(r.pose.parts[2].rot[1] == doctest::Approx(fx_rot(2, 1, 400)).epsilon(1e-3));
    CHECK(base_label(r).find("pose 57 at 400 ms") != std::string::npos);

    // 600 ms of the clip is 1300 ms in a pose that lasts 1000: held at its last draw and said so.
    BaseResult e = base_pose_for(lib, b, 600);
    CHECK(e.past_end);
    CHECK(e.pose.parts[2].rot[1] == doctest::Approx(fx_rot(2, 1, 1000)).epsilon(1e-4));
    CHECK(base_label(e).find("past its end") != std::string::npos);
}

TEST_CASE("poselib: a pose the library lacks is named, and the tracks go over the neutral pose") {
    PoseLibrary lib;
    lib.add(make_pose({0, 100}, 18));
    anim::Clip c = parse_ok(
        "duration = 1000\n[borrow]\nmode = 57\n\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nmode = \"add\"\n"
        "keys = [\n  { t = 0.0, v = [10, 0, 0] },\n  { t = 1.0, v = [10, 0, 0] },\n]\n");
    BaseResult r = base_pose_for(lib, c, 500);
    CHECK(r.kind == BaseResult::Kind::Missing);
    CHECK(r.pose_id == 57);
    const std::string label = base_label(r);
    CHECK(label.find("pose 57") != std::string::npos);
    CHECK(label.find("not in the pose library") != std::string::npos);
    CHECK(label.find("neutral pose") != std::string::npos);
    // The tracks over that base: the neutral pose plus the offset, as with no borrow at all.
    anim::Pose pose = r.pose;
    anim::Timing t;
    t.elapsed_ms = 500;
    t.duration_ms = 1000;
    t.run_ms = 1000;
    REQUIRE(anim::apply_clip(c, t, pose).ok);
    CHECK(pose.parts[int(anim::Part::Head)].rot[0] == doctest::Approx(10));
    // The empty library and the pose id out of the catalogue's range are the same case.
    CHECK(base_pose_for(PoseLibrary(), c, 0).kind == BaseResult::Kind::Missing);
}

TEST_CASE("poselib: an add track is an offset on the borrowed pose, a set track is the value") {
    PoseLibrary lib;
    lib.add(make_pose(every(100, 1000), 57));
    anim::Clip c = parse_ok(
        "duration = 1000\n[borrow]\nmode = 57\n\n"
        "[[track]]\npart = \"head\"\nchannel = \"rot\"\nmode = \"add\"\nkeys = [ { t = 0.0, v = [5, 0, 0] }, { t = 1.0, v = [5, 0, 0] } ]\n"
        "[[track]]\npart = \"hand_r\"\nchannel = \"rot\"\nmode = \"set\"\nkeys = [ { t = 0.0, v = [1, 2, 3] }, { t = 1.0, v = [1, 2, 3] } ]\n");
    BaseResult r = base_pose_for(lib, c, 400);
    REQUIRE(r.kind == BaseResult::Kind::Borrowed);
    anim::Pose pose = r.pose;
    anim::Timing t;
    t.elapsed_ms = 400;
    t.duration_ms = 1000;
    t.run_ms = 1000;
    REQUIRE(anim::apply_clip(c, t, pose).ok);
    CHECK(pose.parts[int(anim::Part::Head)].rot[0] == doctest::Approx(fx_rot(2, 0, 400) + 5).epsilon(1e-3));
    CHECK(pose.parts[int(anim::Part::Head)].rot[1] == doctest::Approx(fx_rot(2, 1, 400)).epsilon(1e-3));
    CHECK(pose.parts[int(anim::Part::HandR)].rot[2] == doctest::Approx(3));
    CHECK(pose.parts[int(anim::Part::HandR)].rot[0] == doctest::Approx(1));
}

TEST_CASE("poselib: Capture at t makes a key of each part's vectors at the pose time") {
    BakedPose p = make_pose(every(100, 1000));
    anim::Clip c = parse_ok(kEmptyClip);
    std::string out, err;
    CaptureResult r;
    REQUIRE(capture_at(kEmptyClip, c, p, 300, 0.5f, 1000, out, err, &r));
    CHECK(r.tracks_made == 11);   // pos of five parts (the tail has none) and rot of six
    CHECK(r.keys_added == 0);
    CHECK(r.keys_replaced == 0);
    anim::Clip got = parse_ok(out);
    REQUIRE(got.tracks.size() == 11);
    for (const anim::Track& t : got.tracks) {
        CHECK(t.mode == anim::TrackMode::Set);
        REQUIRE(t.keys.size() == 1);
        CHECK(t.keys[0].t == doctest::Approx(0.5f));
    }
    const anim::Track* head = track_of(got, anim::Part::Head, anim::Channel::Rot);
    REQUIRE(head);
    for (int a = 0; a < 3; ++a) CHECK(head->keys[0].v[a] == doctest::Approx(fx_rot(2, a, 300)).epsilon(2e-3));
    const anim::Track* root = track_of(got, anim::Part::Root, anim::Channel::Pos);
    REQUIRE(root);
    CHECK(root->keys[0].v[1] == doctest::Approx(fx_pos(0, 1, 300)).epsilon(2e-3));
    CHECK(root->keys[0].v[2] == 0);
    CHECK(track_of(got, anim::Part::Tail, anim::Channel::Pos) == nullptr);
    CHECK(track_of(got, anim::Part::Tail, anim::Channel::Rot) != nullptr);
}

TEST_CASE("poselib: a second capture at the same clip time replaces, at another time adds") {
    BakedPose p = make_pose(every(100, 1000));
    std::string t1, t2, t3, err;
    anim::Clip c0 = parse_ok(kEmptyClip);
    REQUIRE(capture_at(kEmptyClip, c0, p, 300, 0.5f, 1000, t1, err));
    anim::Clip c1 = parse_ok(t1);

    CaptureResult same;
    REQUIRE(capture_at(t1, c1, p, 700, 0.5f, 1000, t2, err, &same));   // the same clip time, another pose time
    CHECK(same.tracks_made == 0);
    CHECK(same.keys_replaced == 11);
    CHECK(same.keys_added == 0);
    anim::Clip c2 = parse_ok(t2);
    const anim::Track* head = track_of(c2, anim::Part::Head, anim::Channel::Rot);
    REQUIRE(head);
    REQUIRE(head->keys.size() == 1);
    CHECK(head->keys[0].v[0] == doctest::Approx(fx_rot(2, 0, 700)).epsilon(2e-3));

    CaptureResult other;
    REQUIRE(capture_at(t2, c2, p, 100, 0.2f, 1000, t3, err, &other));
    CHECK(other.keys_added == 11);
    anim::Clip c3 = parse_ok(t3);
    head = track_of(c3, anim::Part::Head, anim::Channel::Rot);
    REQUIRE(head);
    REQUIRE(head->keys.size() == 2);
    CHECK(head->keys[0].t == doctest::Approx(0.2f));   // in time order
    CHECK(head->keys[1].t == doctest::Approx(0.5f));
    // A key within half a millisecond is the same key.
    CaptureResult near;
    REQUIRE(capture_at(t3, c3, p, 100, 0.2f + 0.0003f, 1000, t1, err, &near));
    CHECK(near.keys_replaced == 11);
}

TEST_CASE("poselib: Capture at t leaves the author's text and add tracks alone") {
    const std::string text =
        "# a swing\nduration = 1000\nend = \"hold\"\n\n# the head turns\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nmode = \"add\"\n"
        "keys = [\n  { t = 0.0, v = [0, 0, 0] },\n  { t = 1.0, v = [20, 0, 0] },\n]\n";
    anim::Clip c = parse_ok(text);
    BakedPose p = make_pose(every(100, 1000));
    std::string out, err;
    CaptureResult r;
    REQUIRE(capture_at(text, c, p, 500, 1.0f, 1000, out, err, &r));
    CHECK(out.compare(0, text.size(), text) == 0);   // everything that was there is still there, unchanged
    CHECK(r.over_add_tracks == 1);                   // the head's rot has an add track: the new set track runs after it
    anim::Clip got = parse_ok(out);
    REQUIRE(got.tracks.size() == 12);
    const anim::Track* add = track_of(got, anim::Part::Head, anim::Channel::Rot, anim::TrackMode::Add);
    REQUIRE(add);
    CHECK(add->keys.size() == 2);
    // A key at t = 1 is a key at the end of the clip.
    const anim::Track* head = track_of(got, anim::Part::Head, anim::Channel::Rot);
    REQUIRE(head);
    CHECK(head->keys[0].t == 1.0f);
}

TEST_CASE("poselib: the edits refuse what they cannot do") {
    BakedPose p = make_pose(every(100, 1000));
    anim::Clip c = parse_ok(kEmptyClip);
    std::string out, err;
    CHECK_FALSE(capture_at(kEmptyClip, c, p, 100, 0.5f, 0, out, err));   // no length: no fraction to write
    CHECK(err.find("no length") != std::string::npos);
    BakedPose none;
    err.clear();
    CHECK_FALSE(capture_at(kEmptyClip, c, none, 100, 0.5f, 1000, out, err));
    CHECK(err.find("no frames") != std::string::npos);
    err.clear();
    CHECK_FALSE(bake_to_keys(kEmptyClip, c, p, 0, out, err));
    CHECK(err.find("no length") != std::string::npos);
    err.clear();
    CHECK_FALSE(bake_to_keys(kEmptyClip, c, none, 1000, out, err));

    // Tracks written as an inline list cannot take a new `[[track]]`: the error is the editor's.
    const std::string inline_tracks =
        "duration = 1000\ntrack = [ { part = \"head\", channel = \"rot\", keys = [ { t = 0.0, v = [0, 0, 0] } ] } ]\n";
    anim::ClipLoad il = anim::parse_clip(inline_tracks, "fixture");
    if (il.ok) {
        err.clear();
        CHECK_FALSE(capture_at(inline_tracks, il.clip, p, 100, 0.5f, 1000, out, err));
        CHECK_FALSE(err.empty());
    }
}

TEST_CASE("poselib: Bake to keys writes at most 30 keys a second, first and last kept") {
    // 1 s of draws at 60 a second: 61 draws, 16.67 ms apart.
    std::vector<double> t;
    for (int i = 0; i <= 60; ++i) t.push_back(i * 1000.0 / 60.0);
    BakedPose p = make_pose(t);
    anim::Clip c = parse_ok(kEmptyClip);
    std::string out, err;
    BakeResult r;
    REQUIRE(bake_to_keys(kEmptyClip, c, p, 1000, out, err, &r));
    CHECK(r.tracks_made == 11);
    CHECK(r.tracks_replaced == 0);
    CHECK(r.dropped == 0);
    CHECK(r.keys >= 30);
    CHECK(r.keys <= 32);   // every second draw, and the last
    anim::Clip got = parse_ok(out);
    REQUIRE(got.tracks.size() == 11);
    const anim::Track* head = track_of(got, anim::Part::Head, anim::Channel::Rot);
    REQUIRE(head);
    REQUIRE(int(head->keys.size()) == r.keys);
    CHECK(head->keys.front().t == 0.0f);
    CHECK(head->keys.back().t == doctest::Approx(1.0f));
    for (size_t k = 1; k < head->keys.size(); ++k) CHECK(head->keys[k].t > head->keys[k - 1].t);
    // A key is a draw: the value at its time is the fixture's.
    const anim::Key& mid = head->keys[head->keys.size() / 2];
    CHECK(mid.v[0] == doctest::Approx(fx_rot(2, 0, mid.t * 1000.0)).epsilon(5e-3));
    // And the clip plays the pose back: the evaluator at a key's time gives the baked vector.
    anim::Pose pose;
    anim::Timing tm;
    tm.elapsed_ms = float(mid.t * 1000.0);
    tm.duration_ms = 1000;
    tm.run_ms = 1000;
    REQUIRE(anim::apply_clip(got, tm, pose).ok);
    CHECK(pose.parts[int(anim::Part::Head)].rot[0] == doctest::Approx(fx_rot(2, 0, tm.elapsed_ms)).epsilon(5e-3));
}

TEST_CASE("poselib: Bake to keys keeps every draw that is slower than 30 a second") {
    BakedPose p = make_pose(every(200, 1000));   // 5 a second, as under Wine
    anim::Clip c = parse_ok(kEmptyClip);
    std::string out, err;
    BakeResult r;
    REQUIRE(bake_to_keys(kEmptyClip, c, p, 1000, out, err, &r));
    CHECK(r.keys == 6);
}

TEST_CASE("poselib: Bake to keys drops the draws past the clip and says how many") {
    BakedPose p = make_pose(every(100, 1000));   // 11 draws
    anim::Clip c = parse_ok("duration = 450\n");
    std::string out, err;
    BakeResult r;
    REQUIRE(bake_to_keys("duration = 450\n", c, p, 450, out, err, &r));
    CHECK(r.keys == 5);      // 0 .. 400
    CHECK(r.dropped == 6);   // 500 .. 1000
    anim::Clip got = parse_ok(out);
    const anim::Track* head = track_of(got, anim::Part::Head, anim::Channel::Rot);
    REQUIRE(head);
    CHECK(head->keys.back().t == doctest::Approx(400.0f / 450.0f));
    // A clip so short that one draw fits is refused.
    err.clear();
    CHECK_FALSE(bake_to_keys("duration = 50\n", parse_ok("duration = 50\n"), p, 50, out, err));
    CHECK(err.find("fewer than two") != std::string::npos);
}

TEST_CASE("poselib: Bake to keys replaces the set tracks of the pose and keeps add tracks") {
    const std::string text =
        "duration = 1000\n\n"
        "[[track]]\npart = \"head\"\nchannel = \"rot\"\nmode = \"set\"\nkeys = [ { t = 0.0, v = [90, 0, 0] }, { t = 1.0, v = [90, 0, 0] } ]\n\n"
        "[[track]]\npart = \"body\"\nchannel = \"rot\"\nmode = \"add\"\nkeys = [ { t = 0.0, v = [0, 0, 0] }, { t = 1.0, v = [0, 0, 15] } ]\n";
    anim::Clip c = parse_ok(text);
    BakedPose p = make_pose(every(100, 1000));
    std::string out, err;
    BakeResult r;
    REQUIRE(bake_to_keys(text, c, p, 1000, out, err, &r));
    CHECK(r.tracks_replaced == 1);
    CHECK(r.tracks_made == 11);
    CHECK(r.over_add_tracks == 1);   // the body's rot: the add track runs first, then the baked set track replaces it
    anim::Clip got = parse_ok(out);
    CHECK(got.tracks.size() == 12);   // the add track and eleven baked
    const anim::Track* add = track_of(got, anim::Part::Body, anim::Channel::Rot, anim::TrackMode::Add);
    REQUIRE(add);
    CHECK(add->keys.size() == 2);
    const anim::Track* head = track_of(got, anim::Part::Head, anim::Channel::Rot);
    REQUIRE(head);
    CHECK(head->keys.size() == 11);                                    // not the old two keys
    CHECK(head->keys[0].v[0] == doctest::Approx(fx_rot(2, 0, 0)).epsilon(2e-3));   // not 90
}

TEST_CASE("poselib: the library folder setting round-trips and a damaged file gives none") {
    Settings s;
    s.library_folder = "D:\\private folder\\pose-library \"x\"";
    Settings back = parse_settings(settings_text(s));
    CHECK(back.library_folder == s.library_folder);
    CHECK(parse_settings("").library_folder.empty());
    CHECK(parse_settings("{ not json").library_folder.empty());
    CHECK(parse_settings("[1, 2]").library_folder.empty());
    CHECK(parse_settings("{\"library_folder\": 7}").library_folder.empty());
}

TEST_CASE("poselib: the game folder setting (task 7.6) round-trips and does not disturb the library folder") {
    Settings s;
    s.library_folder = "D:/lib";
    s.game_folder = "D:/play \"game\"";
    const Settings back = parse_settings(settings_text(s));
    CHECK(back.game_folder == s.game_folder);
    CHECK(back.library_folder == s.library_folder);
    // a file the earlier Animator wrote has no game folder
    const Settings old = parse_settings("{\"library_folder\": \"D:/lib\"}");
    CHECK(old.library_folder == "D:/lib");
    CHECK(old.game_folder.empty());
    CHECK(parse_settings("{\"game_folder\": 3}").game_folder.empty());
}
