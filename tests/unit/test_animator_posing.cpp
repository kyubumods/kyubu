// Unit tests of the Animator's posing by hand (openspec add-ability-animation task 6.4): picking a part with a ray, the handles in the
// frame of the rig, the maths of a drag, and the one function that writes a key into the clip's text. No window, no game.
#include "doctest.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "camera.h"
#include "model.h"
#include "posing.h"
#include "rig.h"

using namespace kanimator;

namespace {

const std::string kStandIn = KYUBU_SOURCE_DIR "/tools/animator/rigs/standin.rig.toml";

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Rig load_rig_text(const std::string& text) {
    RigLoad r = parse_rig(text, "test.rig.toml");
    for (const ClipError& e : r.errors) MESSAGE(anim::format_error(e));
    REQUIRE(r.ok);
    return r.rig;
}

Rig standin() { return load_rig_text(slurp(kStandIn)); }

anim::Clip parse_ok(const std::string& text) {
    anim::ClipLoad c = anim::parse_clip(text, "test");
    for (const ClipError& e : c.errors) MESSAGE(anim::format_error(e));
    REQUIRE(c.ok);
    return c.clip;
}

// A hand turned: keys at 0, 0.5 and 1, a comment, a trailing comma, and a comment after a key.
const char* kClip =
    "# a clip\n"
    "duration = 1000   # ms\n"
    "\n"
    "[[track]]\n"
    "part = \"hand_r\"\n"
    "channel = \"rot\"\n"
    "mode = \"add\"\n"
    "keys = [\n"
    "  { t = 0.00, v = [0, 0, 0] },\n"
    "  { t = 0.50, v = [-60, 0, 10], ease = \"ease_out\" },   # the wind-up\n"
    "  { t = 1.00, v = [0, 0, 0], ease = \"smooth\" },\n"
    "]\n";

PoseEdit turn(anim::Part part, int axis, float delta, float t_ms, anim::Channel ch = anim::Channel::Rot) {
    PoseEdit e;
    e.part = part;
    e.channel = ch;
    e.axis = axis;
    e.delta = delta;
    e.t_ms = t_ms;
    e.duration_ms = 1000;
    return e;
}

PoseEditResult ok_edit(const std::string& text, const PoseEdit& e) {
    PoseEditResult r = edit_pose_key(text, e);
    if (!r.ok) MESSAGE(r.error);
    REQUIRE(r.ok);
    return r;
}

std::string replace_once(std::string s, const std::string& from, const std::string& to) {
    size_t at = s.find(from);
    REQUIRE(at != std::string::npos);
    s.replace(at, from.size(), to);
    return s;
}

std::string crlf(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

void expect_key(const anim::Key& k, float t, float x, float y, float z) {
    CHECK(k.t == doctest::Approx(t).epsilon(1e-5));
    CHECK(k.v[0] == doctest::Approx(x).epsilon(1e-4).scale(1));
    CHECK(k.v[1] == doctest::Approx(y).epsilon(1e-4).scale(1));
    CHECK(k.v[2] == doctest::Approx(z).epsilon(1e-4).scale(1));
}

void expect_vec(Vec3 v, float x, float y, float z) {
    CHECK(v.x == doctest::Approx(x).epsilon(1e-4).scale(1));
    CHECK(v.y == doctest::Approx(y).epsilon(1e-4).scale(1));
    CHECK(v.z == doctest::Approx(z).epsilon(1e-4).scale(1));
}

constexpr Viewport kVp = {0, 0, 800, 600};

Camera look_at(Vec3 target) {
    Camera c;
    c.target = target;
    c.yaw_deg = 30;
    c.pitch_deg = 25;
    c.distance = 100;
    return c;
}

// The ray of the pointer when it is over the pixel of a world point.
Ray pointer_on(const Camera& c, Vec3 p) {
    float px = 0, py = 0;
    REQUIRE(project(c, kVp, p, px, py));
    return ray_through_pixel(c, kVp, px, py);
}

// p turned by `deg` about the axis through `origin` (right-handed), by Rodrigues' formula.
Vec3 turned(Vec3 p, Vec3 origin, Vec3 axis, float deg) {
    const float a = deg * 3.14159265358979f / 180.0f;
    Vec3 v = p - origin;
    Vec3 r = v * std::cos(a) + cross(axis, v) * std::sin(a) + axis * (dot(axis, v) * (1 - std::cos(a)));
    return origin + r;
}

Solved at_rest(const Rig& rig) { return solve_pose(rig, anim::Pose{}); }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// The edit

TEST_CASE("posing: a 20 degree drag on each axis changes that component of an add key, and only that text") {
    for (int axis = 0; axis < 3; ++axis) {
        PoseEditResult r = ok_edit(kClip, turn(anim::Part::HandR, axis, 20, 500));
        CHECK_FALSE(r.made_track);
        CHECK_FALSE(r.made_key);
        CHECK(r.track == 0);
        CHECK(r.key == 1);
        const float was[3] = {-60, 0, 10};
        CHECK(r.before == doctest::Approx(was[axis]));
        CHECK(r.after == doctest::Approx(was[axis] + 20));
        float now[3] = {-60, 0, 10};
        now[axis] += 20;
        const anim::Key& k = r.clip.tracks[0].keys[1];
        expect_key(k, 0.5f, now[0], now[1], now[2]);
        CHECK(k.ease == anim::Ease::EaseOut);
        CHECK(r.clip.tracks[0].keys.size() == 3);
        // The text differs from the original in one list and nowhere else: the comments, the layout and the other keys stay.
        std::string expect = replace_once(kClip, "v = [-60, 0, 10]",
                                          "v = [" + std::string(axis == 0 ? "-40" : "-60") + ", " + (axis == 1 ? "20" : "0") + ", " +
                                              (axis == 2 ? "30" : "10") + "]");
        CHECK(r.text == expect);
        CHECK(r.clip.tracks[0].mode == anim::TrackMode::Add);
    }
}

TEST_CASE("posing: a set track gets the value itself") {
    const std::string set_clip = replace_once(kClip, "mode = \"add\"", "mode = \"set\"");
    PoseEditResult r = ok_edit(set_clip, turn(anim::Part::HandR, 1, 20, 500));
    CHECK(r.clip.tracks[0].mode == anim::TrackMode::Set);
    expect_key(r.clip.tracks[0].keys[1], 0.5f, -60, 20, 10);
    // At the scrub time the part's value is the key's own, so the pose shows the number typed in the text.
    anim::Pose pose;
    anim::Timing at;
    at.elapsed_ms = 500;
    at.duration_ms = 1000;
    at.run_ms = 1000;
    anim::apply_clip(r.clip, at, pose);
    CHECK(pose.parts[static_cast<int>(anim::Part::HandR)].rot[1] == doctest::Approx(20));
}

TEST_CASE("posing: the spec's scenario, a part with no track gets a track and a key") {
    PoseEditResult r = ok_edit(kClip, turn(anim::Part::Head, 0, 20, 200));
    CHECK(r.made_track);
    CHECK(r.made_key);
    REQUIRE(r.clip.tracks.size() == 2);
    CHECK(r.track == 1);
    const anim::Track& tr = r.clip.tracks[1];
    CHECK(tr.part == anim::Part::Head);
    CHECK(tr.channel == anim::Channel::Rot);
    CHECK(tr.mode == anim::TrackMode::Add);
    // The posed key between a zero key at each end of the clip, so the pose does not hold for the whole clip.
    REQUIRE(tr.keys.size() == 3);
    CHECK(r.key == 1);
    expect_key(tr.keys[0], 0.0f, 0, 0, 0);
    expect_key(tr.keys[1], 0.2f, 20, 0, 0);
    expect_key(tr.keys[2], 1.0f, 0, 0, 0);
    CHECK(r.before == doctest::Approx(0));
    // The text before the edit is untouched: the new table is appended.
    CHECK(r.text.compare(0, std::string(kClip).size(), kClip) == 0);
    // A second drag on the same part and channel finds that track now.
    PoseEditResult r2 = ok_edit(r.text, turn(anim::Part::Head, 0, 5, 200));
    CHECK_FALSE(r2.made_track);
    CHECK_FALSE(r2.made_key);
    CHECK(r2.key == 1);
    REQUIRE(r2.clip.tracks[1].keys.size() == 3);
    expect_key(r2.clip.tracks[1].keys[1], 0.2f, 25, 0, 0);
}

TEST_CASE("posing: a position drag writes the pos channel, in a clip that has no track yet") {
    PoseEdit e = turn(anim::Part::Body, 2, -3.5f, 250, anim::Channel::Pos);
    e.duration_ms = 500;
    PoseEditResult r = ok_edit("duration = 500\n", e);
    REQUIRE(r.clip.tracks.size() == 1);
    CHECK(r.clip.tracks[0].channel == anim::Channel::Pos);
    CHECK(r.clip.tracks[0].part == anim::Part::Body);
    REQUIRE(r.clip.tracks[0].keys.size() == 3);
    expect_key(r.clip.tracks[0].keys[1], 0.5f, 0, 0, -3.5f);
}

TEST_CASE("posing: a new track's posed key at an end of the clip is that end's key") {
    PoseEditResult r = ok_edit(kClip, turn(anim::Part::Head, 1, 12, 0));
    const auto& a = r.clip.tracks[1].keys;
    REQUIRE(a.size() == 2);
    CHECK(r.key == 0);
    expect_key(a[0], 0.0f, 0, 12, 0);
    expect_key(a[1], 1.0f, 0, 0, 0);
    r = ok_edit(kClip, turn(anim::Part::Head, 1, 12, 1000));
    const auto& b = r.clip.tracks[1].keys;
    REQUIRE(b.size() == 2);
    CHECK(r.key == 1);
    expect_key(b[0], 0.0f, 0, 0, 0);
    expect_key(b[1], 1.0f, 0, 12, 0);
}

TEST_CASE("posing: the offset is divided by the blend weight, so the figure follows the drag") {
    // blend_in 100 ms of a 1000 ms clip: at 50 ms the weight is 0.5.
    const std::string blended = replace_once(kClip, "duration = 1000   # ms\n", "duration = 1000\nblend_in = 100\n");
    const anim::Clip clip = parse_ok(blended);
    anim::Timing at;
    at.elapsed_ms = 50;
    at.duration_ms = 1000;
    at.run_ms = 1000;
    const float w = anim::blend_weight(clip, at);
    REQUIRE(w == doctest::Approx(0.5f));
    anim::Pose before;
    anim::apply_clip(clip, at, before);
    PoseEdit e = turn(anim::Part::HandR, 0, 10, 50);
    e.weight = w;
    PoseEditResult r = ok_edit(blended, e);
    // The track moves by 10 / 0.5 = 20; the part on screen moved by the drag, 10.
    float track_was[3];
    anim::sample_track(clip.tracks[0], 0.05f, track_was);
    CHECK(r.after == doctest::Approx(track_was[0] + 20).epsilon(1e-3).scale(1));
    anim::Pose after;
    anim::apply_clip(r.clip, at, after);
    const int hand = static_cast<int>(anim::Part::HandR);
    CHECK(after.parts[hand].rot[0] - before.parts[hand].rot[0] == doctest::Approx(10).epsilon(1e-3).scale(1));
    // A set track is the same: base + w * (track - base) moves by delta when the track moves by delta / w.
    const std::string set_clip = replace_once(blended, "mode = \"add\"", "mode = \"set\"");
    const anim::Clip sc = parse_ok(set_clip);
    anim::Pose sb;
    anim::apply_clip(sc, at, sb);
    r = ok_edit(set_clip, e);
    anim::Pose sa;
    anim::apply_clip(r.clip, at, sa);
    CHECK(sa.parts[hand].rot[0] - sb.parts[hand].rot[0] == doctest::Approx(10).epsilon(1e-3).scale(1));
    // Where the weight is 1 nothing is divided; where it is nearly 0 the edit is refused and the text stays.
    e.weight = 1;
    float was[3];
    anim::sample_track(parse_ok(kClip).tracks[0], 0.05f, was);
    CHECK(ok_edit(kClip, e).after == doctest::Approx(was[0] + 10).epsilon(1e-3).scale(1));
    e.weight = 0.01f;
    PoseEditResult refused = edit_pose_key(kClip, e);
    CHECK_FALSE(refused.ok);
    CHECK(refused.text == kClip);
    CHECK(refused.error.find("blends") != std::string::npos);
    e.weight = 0;
    CHECK_FALSE(edit_pose_key(kClip, e).ok);
}

TEST_CASE("posing: a key between two keys takes the track's value there and the ease of the key after it") {
    PoseEditResult r = ok_edit(kClip, turn(anim::Part::HandR, 0, 20, 250));
    CHECK(r.made_key);
    CHECK_FALSE(r.made_track);
    CHECK(r.key == 1);
    const anim::Clip old = parse_ok(kClip);
    float was[3];
    anim::sample_track(old.tracks[0], 0.25f, was);
    CHECK(r.before == doctest::Approx(was[0]));
    const auto& keys = r.clip.tracks[0].keys;
    REQUIRE(keys.size() == 4);
    expect_key(keys[0], 0.0f, 0, 0, 0);
    expect_key(keys[1], 0.25f, was[0] + 20, was[1], was[2]);
    CHECK(keys[1].ease == anim::Ease::EaseOut);
    expect_key(keys[2], 0.5f, -60, 0, 10);
    expect_key(keys[3], 1.0f, 0, 0, 0);
    // The new line sits between its neighbours with their indentation; the comment stays on its key.
    CHECK(r.text.find("  { t = 0.25, v = [") != std::string::npos);
    CHECK(r.text.find("ease = \"ease_out\" },   # the wind-up") != std::string::npos);
    // The track at that time reads before + 20: what the drag showed.
    float now[3];
    anim::sample_track(r.clip.tracks[0], 0.25f, now);
    CHECK(now[0] == doctest::Approx(was[0] + 20).epsilon(1e-3));
}

TEST_CASE("posing: a key within half a millisecond of the scrub time is that key") {
    PoseEditResult r = ok_edit(kClip, turn(anim::Part::HandR, 0, 5, 500.4f));
    CHECK_FALSE(r.made_key);
    CHECK(r.clip.tracks[0].keys.size() == 3);
    expect_key(r.clip.tracks[0].keys[1], 0.5f, -55, 0, 10);
    r = ok_edit(kClip, turn(anim::Part::HandR, 0, 5, 501.0f));
    CHECK(r.made_key);
    CHECK(r.clip.tracks[0].keys.size() == 4);
}

TEST_CASE("posing: a key before the first and after the last, in every layout the files use") {
    const std::string late =
        "duration = 1000\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nkeys = [\n"
        "  { t = 0.20, v = [1, 0, 0] },\n"
        "  { t = 0.80, v = [2, 0, 0], ease = \"smooth\" }   # no comma after the last key\n"
        "]\n";
    {   // before the first key: the first key's ease is the segment that arrives at it, which the new key now ends
        PoseEditResult r = ok_edit(late, turn(anim::Part::Head, 0, 10, 100));
        REQUIRE(r.clip.tracks[0].keys.size() == 3);
        CHECK(r.key == 0);
        expect_key(r.clip.tracks[0].keys[0], 0.1f, 11, 0, 0);   // the first value holds before the first key: 1 + 10
        expect_key(r.clip.tracks[0].keys[1], 0.2f, 1, 0, 0);
    }
    {   // after the last key, which has no comma and a comment on its line
        PoseEditResult r = ok_edit(late, turn(anim::Part::Head, 1, 7, 900));
        REQUIRE(r.clip.tracks[0].keys.size() == 3);
        CHECK(r.key == 2);
        expect_key(r.clip.tracks[0].keys[2], 0.9f, 2, 7, 0);
        CHECK(r.text.find("# no comma after the last key\n  { t = 0.9, v = [2, 7, 0] },\n]") != std::string::npos);
    }
    const std::string one_line = "duration = 1000\n[[track]]\npart=\"head\"\nchannel=\"rot\"\nkeys=[{t=0,v=[0,0,0]},{t=0.4,v=[4,0,0]}]\n";
    {   // between, on a single line
        PoseEditResult r = ok_edit(one_line, turn(anim::Part::Head, 2, 3, 200));
        REQUIRE(r.clip.tracks[0].keys.size() == 3);
        expect_key(r.clip.tracks[0].keys[1], 0.2f, 2, 0, 3);
    }
    {   // after the last, on a single line
        PoseEditResult r = ok_edit(one_line, turn(anim::Part::Head, 0, 1, 700));
        REQUIRE(r.clip.tracks[0].keys.size() == 3);
        expect_key(r.clip.tracks[0].keys[2], 0.7f, 5, 0, 0);
    }
}

TEST_CASE("posing: CRLF text stays CRLF, and a comment with non-ASCII text on the key's line is no trouble") {
    std::string text = crlf(replace_once(kClip, "# the wind-up", "# l'\xC3\xA9lan"));
    PoseEditResult r = ok_edit(text, turn(anim::Part::HandR, 0, 20, 250));
    size_t lf = 0, crlfs = 0;
    for (size_t i = 0; i < r.text.size(); ++i) {
        if (r.text[i] == '\n') {
            ++lf;
            if (i > 0 && r.text[i - 1] == '\r') ++crlfs;
        }
    }
    CHECK(lf == crlfs);
    CHECK(r.text.find("# l'\xC3\xA9lan") != std::string::npos);
    r = ok_edit(text, turn(anim::Part::HandR, 0, 20, 500));
    expect_key(r.clip.tracks[0].keys[1], 0.5f, -40, 0, 10);
    // A new track on text with no newline at the end.
    std::string bare = "duration = 1000";
    r = ok_edit(bare, turn(anim::Part::Tail, 0, 4, 0));
    CHECK(r.clip.tracks.size() == 1);
    CHECK(r.text.compare(0, bare.size(), bare) == 0);
}

TEST_CASE("posing: the last track on the part and channel is the one written") {
    const std::string two = std::string(kClip) +
                            "\n[[track]]\npart = \"weapon_r\"\nchannel = \"rot\"\nmode = \"add\"\nkeys = [\n  { t = 0.0, v = [1, 2, 3] },\n]\n";
    PoseEditResult r = ok_edit(two, turn(anim::Part::HandR, 2, 1, 0));   // `weapon_r` is the right hand's other name
    CHECK(r.track == 1);
    expect_key(r.clip.tracks[1].keys[0], 0.0f, 1, 2, 4);
    expect_key(r.clip.tracks[0].keys[0], 0.0f, 0, 0, 0);
}

TEST_CASE("posing: values are held to the validator's range, and the time to the clip") {
    PoseEditResult r = ok_edit(kClip, turn(anim::Part::HandR, 0, 5000, 500));
    CHECK(r.after == doctest::Approx(anim::kMaxRotDeg));
    r = ok_edit(kClip, turn(anim::Part::HandR, 0, -5000, 500));
    CHECK(r.after == doctest::Approx(-anim::kMaxRotDeg));
    r = ok_edit(kClip, turn(anim::Part::Body, 0, 500, 0, anim::Channel::Pos));
    CHECK(r.after == doctest::Approx(anim::kMaxPos));
    r = ok_edit(kClip, turn(anim::Part::HandR, 0, 3, 99999));   // past the end: the last key
    CHECK_FALSE(r.made_key);
    expect_key(r.clip.tracks[0].keys[2], 1.0f, 3, 0, 0);
    r = ok_edit(kClip, turn(anim::Part::HandR, 0, 3, -50));     // before the start: the first key
    expect_key(r.clip.tracks[0].keys[0], 0.0f, 3, 0, 0);
}

TEST_CASE("posing: a bad clip, a bad drag and a layout the editor cannot extend change nothing") {
    PoseEditResult r = edit_pose_key("duration = [", turn(anim::Part::Head, 0, 1, 0));
    CHECK_FALSE(r.ok);
    CHECK(r.text == "duration = [");
    CHECK_FALSE(r.error.empty());
    PoseEdit e = turn(anim::Part::Head, 0, 1, 0);
    e.duration_ms = 0;
    CHECK_FALSE(edit_pose_key(kClip, e).ok);
    e = turn(anim::Part::Head, 3, 1, 0);
    CHECK_FALSE(edit_pose_key(kClip, e).ok);
    e = turn(anim::Part::Head, 0, std::nanf(""), 0);
    CHECK_FALSE(edit_pose_key(kClip, e).ok);

    // keys as [[track.keys]] tables: a value can be changed in place, a key cannot be added
    const std::string tables =
        "duration = 1000\n[[track]]\npart = \"head\"\nchannel = \"rot\"\n"
        "[[track.keys]]\nt = 0.0\nv = [0, 0, 0]\n[[track.keys]]\nt = 1.0\nv = [0, 0, 0]\n";
    r = ok_edit(tables, turn(anim::Part::Head, 0, 4, 1000));
    expect_key(r.clip.tracks[0].keys[1], 1.0f, 4, 0, 0);
    r = edit_pose_key(tables, turn(anim::Part::Head, 0, 4, 500));
    CHECK_FALSE(r.ok);
    CHECK(r.text == tables);

    // tracks as one `track = [ ... ]` list: a new `[[track]]` would clash with it
    const std::string listed =
        "duration = 1000\ntrack = [ { part = \"head\", channel = \"rot\", keys = [ { t = 0.0, v = [0, 0, 0] } ] } ]\n";
    r = ok_edit(listed, turn(anim::Part::Head, 0, 4, 0));
    expect_key(r.clip.tracks[0].keys[0], 0.0f, 4, 0, 0);
    r = edit_pose_key(listed, turn(anim::Part::Tail, 0, 4, 0));
    CHECK_FALSE(r.ok);
}

TEST_CASE("posing: one undo step per drag") {
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "kyubu_pose_test.anim.toml";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << kClip;
    }
    ClipDoc doc;
    std::string err;
    REQUIRE_MESSAGE(doc.open(file.string(), err), err);
    REQUIRE(doc.undo_depth() == 0);
    REQUIRE_FALSE(doc.dirty());
    // A drag is many pointer moves; the model is edited from the text at the press each time, and the document once, at the release.
    const std::string at_press = doc.text();
    PoseEditResult last;
    for (float d = 1; d <= 20; d += 1) last = ok_edit(at_press, turn(anim::Part::HandR, 0, d, 250));
    CHECK(doc.undo_depth() == 0);
    doc.set_text(last.text);
    CHECK(doc.undo_depth() == 1);
    CHECK(doc.dirty());
    CHECK(doc.text() == last.text);
    CHECK(doc.undo());
    CHECK(doc.text() == at_press);
    CHECK(doc.undo_depth() == 0);
    CHECK_FALSE(doc.dirty());
    // A drag that ended where it began leaves the text as it was: no step.
    PoseEditResult same = ok_edit(at_press, turn(anim::Part::HandR, 0, 0, 500));
    CHECK(same.text == at_press);
    doc.set_text(same.text);
    CHECK(doc.undo_depth() == 0);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Picking and handles

TEST_CASE("posing: a ray picks the nearest box, in each node's own frame") {
    const Rig rig = load_rig_text(
        "name = \"t\"\n"
        "[[node]]\nname = \"a\"\nmodel = \"box 2 2 2\"\nsteps = []\n"
        "[[node]]\nname = \"b\"\nparent = \"a\"\nmodel = \"box 2 10 2\"\nsteps = [[\"translate\", 0, 0, 0]]\n"
        "[[node]]\nname = \"c\"\nparent = \"a\"\nsteps = [[\"translate\", 20, 0, 0]]\n");
    std::vector<Affine> world(3);
    std::vector<NodeBox> boxes(3);
    boxes[0] = {true, {-1, -1, -1}, {1, 1, 1}};
    boxes[1] = {true, {-1, -5, -1}, {1, 5, 1}};
    // a is at (0, 0, 0); b is at (0, 20, 0), turned 90 degrees about z so its long side runs along x; c has no box.
    world[1].t[1] = 20;
    world[1].r[0][0] = 0;
    world[1].r[0][1] = -1;
    world[1].r[1][0] = 1;
    world[1].r[1][1] = 0;
    float dist = 0;
    // From -y looking along +y, the ray at x = 0 meets a (near) first.
    int hit = pick_node(rig, world, boxes, Ray{{0, -50, 0}, {0, 1, 0}}, &dist);
    CHECK(hit == 0);
    CHECK(dist == doctest::Approx(49));
    // At x = 4 only b is there: it is 10 long along x after the turn, so x of 4 is inside it and a is not.
    hit = pick_node(rig, world, boxes, Ray{{4, -50, 0}, {0, 1, 0}}, &dist);
    CHECK(hit == 1);
    CHECK(dist == doctest::Approx(50 + 19));
    // Beside both: no hit, and the node without a box is never picked.
    CHECK(pick_node(rig, world, boxes, Ray{{40, -50, 0}, {0, 1, 0}}) == -1);
    CHECK(pick_node(rig, world, boxes, Ray{{0, -50, 30}, {0, 1, 0}}) == -1);
}

TEST_CASE("posing: a click on a model node edits the part above it") {
    const Rig rig = standin();
    CHECK(part_node(rig, rig.find("head")) == rig.find("head"));
    // In the stand-in rig the weapon answers to `weapon_r`, which is the right hand's part: it is its own part node.
    CHECK(part_node(rig, rig.find("weapon_r")) == rig.find("weapon_r"));
    const Rig two = load_rig_text(
        "name = \"t\"\n"
        "[[node]]\nname = \"frame\"\nsteps = []\n"
        "[[node]]\nname = \"body\"\nparent = \"frame\"\npart = \"body\"\nsteps = [[\"rot_z\", \"body.rot.z\"]]\n"
        "[[node]]\nname = \"body_model\"\nparent = \"body\"\nmodel = \"body4.cub\"\nsteps = [[\"translate\", 0, 0, -5]]\n"
        "[[node]]\nname = \"prop\"\nparent = \"frame\"\nmodel = \"box 1 1 1\"\nsteps = []\n");
    CHECK(part_node(two, two.find("body_model")) == two.find("body"));
    CHECK(part_node(two, two.find("prop")) == -1);
    CHECK_FALSE(make_handles(two, at_rest(two), two.find("prop")).ok);
    CHECK_FALSE(make_handles(two, at_rest(two), -1).ok);
    Handles h = make_handles(two, at_rest(two), two.find("body_model"));
    CHECK(h.ok);
    CHECK(h.edit_node == two.find("body"));
    CHECK(h.part == anim::Part::Body);
}

TEST_CASE("posing: the handles come from the rig's steps") {
    const Rig rig = standin();
    // At rest the head's frame is the body's, which is the root's: the figure's own axes.
    Handles h = make_handles(rig, at_rest(rig), rig.find("head"));
    REQUIRE(h.ok);
    CHECK(h.part == anim::Part::Head);
    CHECK(h.pivot == rig.find("head"));
    expect_vec(h.origin, 0, 0, 18 + 11);
    for (int a = 0; a < 3; ++a) {
        REQUIRE(h.turn[a].present);
        REQUIRE(h.move[a].present);
        CHECK(h.turn[a].sign == doctest::Approx(1));
        CHECK(h.move[a].per_unit == doctest::Approx(1));
    }
    expect_vec(h.turn[0].axis, 1, 0, 0);
    expect_vec(h.turn[1].axis, 0, 1, 0);
    expect_vec(h.turn[2].axis, 0, 0, 1);
    expect_vec(h.move[1].axis, 0, 1, 0);

    // The weapon has no vector: its hand's handles, at the hand.
    Handles w = make_handles(rig, at_rest(rig), rig.find("weapon_r"));
    REQUIRE(w.ok);
    CHECK(w.part == anim::Part::HandR);
    CHECK(w.pivot == rig.find("hand_r"));
    CHECK(w.edit_node == rig.find("weapon_r"));

    // The root reads no z of its position: no arrow for it.
    Handles r = make_handles(rig, at_rest(rig), rig.find("root"));
    REQUIRE(r.ok);
    CHECK(r.move[0].present);
    CHECK(r.move[1].present);
    CHECK_FALSE(r.move[2].present);
    CHECK(r.turn[2].present);
}

TEST_CASE("posing: a ring turns in the frame its step runs in") {
    const Rig rig = standin();
    // The body's steps are z, y, x (outermost first). Its z ring is the parent's z whatever the others are; its x ring is turned by
    // the z and the y before it.
    anim::Pose p;
    p.parts[static_cast<int>(anim::Part::Body)].rot[2] = 90;
    Handles h = make_handles(rig, solve_pose(rig, p), rig.find("body"));
    REQUIRE(h.ok);
    expect_vec(h.turn[2].axis, 0, 0, 1);
    expect_vec(h.turn[1].axis, -1, 0, 0);   // Rz(90) turns +y to -x
    expect_vec(h.turn[0].axis, 0, 1, 0);    // and +x to +y
    // The head's frame is the body's, so a body turned 90 degrees about z turns the head's rings with it.
    Handles hd = make_handles(rig, solve_pose(rig, p), rig.find("head"));
    REQUIRE(hd.ok);
    expect_vec(hd.turn[0].axis, 0, 1, 0);
    expect_vec(hd.turn[2].axis, 0, 0, 1);
    // A position arrow is turned by the frame the translate runs in: the body's `pos` comes after its three turns.
    expect_vec(h.move[0].axis, 0, 1, 0);
    expect_vec(h.move[1].axis, -1, 0, 0);
    anim::Pose q;
    q.parts[static_cast<int>(anim::Part::Body)].rot[1] = 90;   // Ry(90) turns +x to -z
    Handles hy = make_handles(rig, solve_pose(rig, q), rig.find("body"));
    expect_vec(hy.turn[0].axis, 0, 0, -1);
    expect_vec(hy.turn[2].axis, 0, 0, 1);
}

TEST_CASE("posing: a negated argument, a scale and a mirror change the sign and the length of a handle") {
    const Rig rig = load_rig_text(
        "name = \"t\"\n"
        "[[node]]\nname = \"root\"\npart = \"root\"\nsteps = [[\"rot_z\", \"root.rot.z\"]]\n"
        "[[node]]\nname = \"n\"\nparent = \"root\"\npart = \"head\"\n"
        "steps = [[\"rot_x\", \"-head.rot.x\"], [\"scale\", 2], [\"rot_y\", \"head.rot.y\"], [\"translate\", \"head.pos\"]]\n"
        "[[node]]\nname = \"m\"\nparent = \"root\"\npart = \"tail\"\n"
        "steps = [[\"scale\", -1, 1, 1], [\"rot_x\", \"tail.rot.x\"], [\"translate\", \"tail.pos.x\", 0, 0]]\n");
    Handles h = make_handles(rig, at_rest(rig), rig.find("n"));
    REQUIRE(h.ok);
    CHECK(h.turn[0].sign == doctest::Approx(-1));
    CHECK(h.turn[1].sign == doctest::Approx(1));
    CHECK(h.move[0].per_unit == doctest::Approx(2));
    CHECK(h.move[2].per_unit == doctest::Approx(2));
    expect_vec(h.move[2].axis, 0, 0, 1);
    CHECK_FALSE(h.turn[2].present);   // no step reads head.rot.z

    Handles m = make_handles(rig, at_rest(rig), rig.find("m"));
    REQUIRE(m.ok);
    CHECK(m.turn[0].sign == doctest::Approx(-1));   // the mirror flips which way a right-handed turn about its x goes
    expect_vec(m.turn[0].axis, -1, 0, 0);
    CHECK(m.move[0].present);
    CHECK_FALSE(m.move[1].present);                 // the translate reads only tail.pos.x
    expect_vec(m.move[0].axis, -1, 0, 0);
}

TEST_CASE("posing: pick_handle finds a ring or an arrow within a few pixels") {
    const Rig rig = standin();
    Handles h = make_handles(rig, at_rest(rig), rig.find("head"));
    REQUIRE(h.ok);
    const Camera cam = look_at(h.origin);
    const float radius = handle_size(cam);
    for (int a = 0; a < 3; ++a) {
        const std::vector<Vec3> pts = ring_points(h, a, radius, 48);
        REQUIRE(pts.size() == 49);
        float px, py;
        REQUIRE(project(cam, kVp, pts[7], px, py));
        HandleHit hit = pick_handle(cam, kVp, h, px + 1, py - 1);
        REQUIRE(hit.hit);
        // The point may be on the far side of two rings that cross; the one asked for is among the nearest.
        CHECK(hit.distance_px < 3);
    }
    // An arrow's midpoint, away from the rings.
    float px, py;
    const Vec3 tip = h.origin + h.move[2].axis * (radius * 1.35f);
    REQUIRE(project(cam, kVp, h.origin * 0.5f + tip * 0.5f, px, py));
    HandleHit hit = pick_handle(cam, kVp, h, px, py);
    REQUIRE(hit.hit);
    CHECK(hit.kind == HandleKind::Move);
    CHECK(hit.axis == 2);
    // Far from everything.
    CHECK_FALSE(pick_handle(cam, kVp, h, 5, 5).hit);
    CHECK_FALSE(pick_handle(cam, kVp, Handles{}, px, py).hit);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The drag

TEST_CASE("posing: dragging a ring by 20 degrees changes the value by 20, on each axis, both ways") {
    const Rig rig = standin();
    Handles h = make_handles(rig, at_rest(rig), rig.find("head"));
    REQUIRE(h.ok);
    const Camera cam = look_at(h.origin);
    const float radius = handle_size(cam);
    for (int a = 0; a < 3; ++a) {
        const std::vector<Vec3> ring = ring_points(h, a, radius, 8);
        const Vec3 p0 = ring[1];
        for (float angle : {20.0f, -20.0f, 90.0f}) {
            HandleDrag d;
            REQUIRE(d.begin(h, HandleKind::Turn, a, pointer_on(cam, p0)));
            CHECK(d.delta() == doctest::Approx(0));
            REQUIRE(d.update(pointer_on(cam, turned(p0, h.origin, h.turn[a].axis, angle))));
            CHECK(d.delta() == doctest::Approx(angle * h.turn[a].sign).epsilon(1e-3).scale(1));
        }
    }
}

TEST_CASE("posing: a ring drag follows the pointer past a half turn, and a negated ring goes the other way") {
    const Rig rig = load_rig_text(
        "name = \"t\"\n[[node]]\nname = \"n\"\npart = \"head\"\nsteps = [[\"rot_z\", \"-head.rot.z\"]]\n");
    Handles h = make_handles(rig, at_rest(rig), 0);
    REQUIRE(h.ok);
    CHECK(h.turn[2].sign == doctest::Approx(-1));
    const Camera cam = look_at(h.origin);
    const Vec3 p0 = ring_points(h, 2, handle_size(cam), 8)[0];
    HandleDrag d;
    REQUIRE(d.begin(h, HandleKind::Turn, 2, pointer_on(cam, p0)));
    float turned_so_far = 0;
    for (int i = 0; i < 7; ++i) {   // seven steps of 60 degrees: 420, which a bare angle cannot say
        turned_so_far += 60;
        REQUIRE(d.update(pointer_on(cam, turned(p0, h.origin, h.turn[2].axis, turned_so_far))));
    }
    CHECK(d.delta() == doctest::Approx(-420).epsilon(1e-3).scale(1));
}

TEST_CASE("posing: dragging an arrow moves the value by the distance over the length of a unit") {
    const Rig rig = load_rig_text(
        "name = \"t\"\n[[node]]\nname = \"n\"\npart = \"head\"\nsteps = [[\"scale\", 4], [\"translate\", \"head.pos\"]]\n");
    Handles h = make_handles(rig, at_rest(rig), 0);
    REQUIRE(h.ok);
    CHECK(h.move[1].per_unit == doctest::Approx(4));
    const Camera cam = look_at(h.origin);
    const Vec3 a = h.origin + h.move[1].axis * 6.0f;
    HandleDrag d;
    REQUIRE(d.begin(h, HandleKind::Move, 1, pointer_on(cam, a)));
    REQUIRE(d.update(pointer_on(cam, h.origin + h.move[1].axis * 14.0f)));
    CHECK(d.delta() == doctest::Approx(8.0f / 4.0f).epsilon(1e-3).scale(1));
    REQUIRE(d.update(pointer_on(cam, h.origin + h.move[1].axis * 2.0f)));
    CHECK(d.delta() == doctest::Approx(-1).epsilon(1e-3).scale(1));
}

TEST_CASE("posing: a ray that cannot say leaves the last value, and a handle the rig lacks cannot be dragged") {
    const Rig rig = standin();
    Handles h = make_handles(rig, at_rest(rig), rig.find("root"));
    REQUIRE(h.ok);
    const Camera cam = look_at(h.origin);
    HandleDrag d;
    CHECK_FALSE(d.begin(h, HandleKind::Move, 2, pointer_on(cam, h.origin)));   // root has no z arrow
    CHECK_FALSE(d.active());
    const Vec3 p0 = ring_points(h, 2, handle_size(cam), 8)[0];
    REQUIRE(d.begin(h, HandleKind::Turn, 2, pointer_on(cam, p0)));
    REQUIRE(d.update(pointer_on(cam, turned(p0, h.origin, h.turn[2].axis, 30))));
    const float held = d.delta();
    // A ray parallel to the ring's plane (the ring is horizontal here): no value.
    Ray flat{{0, -100, h.origin.z}, {0, 1, 0}};
    CHECK_FALSE(d.update(flat));
    CHECK(d.delta() == held);
    d.end();
    CHECK_FALSE(d.update(pointer_on(cam, p0)));
}

TEST_CASE("posing: a drag in the window's terms, from the press to the key") {
    // The whole path the window runs, without the window: select a part, press on its x ring, move the pointer 20 degrees round, and
    // write the key. The clip's part value at the scrub time is 20 more than before.
    const Rig rig = standin();
    const std::string text = kClip;
    anim::Clip clip = parse_ok(text);
    anim::Pose base;
    anim::Timing at;
    at.elapsed_ms = 500;
    at.duration_ms = 1000;
    at.run_ms = 1000;
    Solved s = solve(rig, base, clip, at);
    const int hand = static_cast<int>(anim::Part::HandR);
    const float before = s.pose.parts[hand].rot[0];
    Handles h = make_handles(rig, s, rig.find("weapon_r"));   // the weapon is picked; the right hand's part is edited
    REQUIRE(h.ok);
    const Camera cam = look_at(h.origin);
    const Vec3 p0 = ring_points(h, 0, handle_size(cam), 16)[3];
    HandleDrag d;
    REQUIRE(d.begin(h, HandleKind::Turn, 0, pointer_on(cam, p0)));
    REQUIRE(d.update(pointer_on(cam, turned(p0, h.origin, h.turn[0].axis, 20))));
    PoseEdit e;
    e.part = h.part;
    e.channel = anim::Channel::Rot;
    e.axis = 0;
    e.delta = d.delta();
    e.t_ms = 500;
    e.duration_ms = 1000;
    PoseEditResult r = ok_edit(text, e);
    Solved after = solve(rig, base, r.clip, at);
    // The blend weight at 500 ms of 1000 is 1 (blend in 0, out 0), so the part reads the key's value.
    CHECK(after.pose.parts[hand].rot[0] == doctest::Approx(before + 20).epsilon(1e-3).scale(1));
}

TEST_CASE("posing: a part with only a generator track gets a keys track beside it (tracks stack)") {
    // covers: animation-editor/Procedural tracks (the editor reads and writes a clip that has generators and leaves them alone)
    const std::string text = R"(duration = 1000
[[track]]
part = "hand_r"
channel = "rot"
osc = { amp = [5, 0, 0], freq = 2 }
)";
    PoseEditResult r = ok_edit(text, turn(anim::Part::HandR, 0, 20, 500));
    REQUIRE(r.clip.tracks.size() == 2);
    CHECK(r.clip.tracks[0].source == anim::Source::Osc);   // the generator is as it was
    CHECK(r.clip.tracks[0].osc.amp[0] == 5);
    CHECK(r.clip.tracks[1].source == anim::Source::Keys);
    CHECK(r.made_track);
    CHECK(r.track == 1);
    CHECK(r.text.find("osc = { amp = [5, 0, 0], freq = 2 }") != std::string::npos);
}
