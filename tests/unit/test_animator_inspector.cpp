// Unit tests of the Animator's key inspector and curve view (openspec add-ability-animation task 6.6): Mirror L/R, the edits of a
// key's value and ease, Reset, and the samples of the curve. No window, no game.
#include "doctest.h"

#include <cmath>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "cliptext.h"
#include "keyinspector.h"
#include "timeline.h"

using namespace kanimator;

namespace {

anim::Clip clip_of(const std::string& text) {
    anim::ClipLoad c = anim::parse_clip(text, "test");
    for (const anim::ClipError& e : c.errors) MESSAGE(anim::format_error(e));
    REQUIRE(c.ok);
    return c.clip;
}

// The design's example, on the right hand, with a position track, plus a left hand that already has a turn of its own and a
// position that must stay. The comments are there to stay.
const char* kClip =
    "# a clip\n"
    "duration = 1000   # ms\n"
    "\n"
    "[[track]]\n"
    "part = \"hand_l\"\n"
    "channel = \"rot\"\n"
    "keys = [\n"
    "  { t = 0.0, v = [5, 5, 5] },\n"
    "  { t = 1.0, v = [9, 9, 9] },\n"
    "]\n"
    "\n"
    "[[track]]\n"
    "part = \"hand_r\"\n"
    "channel = \"rot\"\n"
    "mode = \"add\"\n"
    "keys = [\n"
    "  { t = 0.00, v = [0, 0, 0] },\n"
    "  { t = 0.35, v = [-110, 20, 10], ease = \"ease_out\" },   # wind-up\n"
    "  { t = 0.50, v = [30, -15, 0],   ease = \"ease_in\" },    # strike\n"
    "  { t = 1.00, v = [0, 0, 0],      ease = \"smooth\" },\n"
    "]\n"
    "\n"
    "# the right hand's reach\n"
    "[[track]]\n"
    "part = \"hand_r\"\n"
    "channel = \"pos\"\n"
    "mode = \"set\"\n"
    "keys = [\n"
    "  { t = 0.0, v = [1, 2, 3] },\n"
    "  { t = 0.5, v = [4, -2, 0] },\n"
    "]\n"
    "\n"
    "# the body\n"
    "[[track]]\n"
    "part = \"body\"\n"
    "channel = \"pos\"\n"
    "keys = [{ t = 0.0, v = [0, 0, 0] }, { t = 0.2, v = [0, 0, 2] }, { t = 1.0, v = [0, 0, 0] }]\n"
    "\n"
    "[[marker]]\n"
    "t = 0.50\n"
    "name = \"hit\"\n";

const anim::Track* find_track(const anim::Clip& c, anim::Part part, anim::Channel ch, int nth = 0) {
    for (const anim::Track& t : c.tracks)
        if (t.part == part && t.channel == ch && nth-- == 0) return &t;
    return nullptr;
}

std::string crlf(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

}  // namespace

TEST_CASE("mirror: the pairs and the axes") {
    anim::Part p;
    CHECK(mirror_partner(anim::Part::HandR, p));
    CHECK(p == anim::Part::HandL);
    CHECK(mirror_partner(anim::Part::HandL, p));
    CHECK(p == anim::Part::HandR);
    for (anim::Part single : {anim::Part::Root, anim::Part::Body, anim::Part::Head, anim::Part::Tail}) CHECK_FALSE(mirror_partner(single, p));

    // A position flips x. A rotation keeps x and flips y and z.
    CHECK(mirror_flips_axis(anim::Channel::Pos, 0));
    CHECK_FALSE(mirror_flips_axis(anim::Channel::Pos, 1));
    CHECK_FALSE(mirror_flips_axis(anim::Channel::Pos, 2));
    CHECK_FALSE(mirror_flips_axis(anim::Channel::Rot, 0));
    CHECK(mirror_flips_axis(anim::Channel::Rot, 1));
    CHECK(mirror_flips_axis(anim::Channel::Rot, 2));

    const float v[3] = {3, -4, 0};
    float m[3];
    mirror_vec(anim::Channel::Rot, v, m);
    CHECK(m[0] == 3);
    CHECK(m[1] == 4);
    CHECK(m[2] == 0);
    CHECK_FALSE(std::signbit(m[2]));   // a zero is not turned into -0
    mirror_vec(anim::Channel::Pos, v, m);
    CHECK(m[0] == -3);
    CHECK(m[1] == -4);
    CHECK(m[2] == 0);
    mirror_vec(anim::Channel::Pos, m, m);   // in place, and twice is the identity
    CHECK(m[0] == 3);
    CHECK(m[1] == -4);
}

TEST_CASE("mirror: hand_r onto hand_l, the left gets the right's keys with the sign of the mirrored axes flipped") {
    const std::string text = kClip;
    KeyEditResult r = mirror_part(text, anim::Part::HandR);
    REQUIRE(r.ok);
    const anim::Clip before = clip_of(text), after = r.clip;

    // The right hand and every other track are as they were, and so is the text above the removed table.
    for (anim::Part part : {anim::Part::HandR, anim::Part::Body})
        for (anim::Channel ch : {anim::Channel::Pos, anim::Channel::Rot}) {
            const anim::Track* a = find_track(before, part, ch);
            const anim::Track* b = find_track(after, part, ch);
            REQUIRE((a != nullptr) == (b != nullptr));
            if (!a) continue;
            REQUIRE(a->keys.size() == b->keys.size());
            for (size_t i = 0; i < a->keys.size(); ++i) {
                CHECK(a->keys[i].t == b->keys[i].t);
                for (int c = 0; c < 3; ++c) CHECK(a->keys[i].v[c] == b->keys[i].v[c]);
            }
        }
    CHECK(has(r.text, "# a clip\nduration = 1000   # ms\n"));
    CHECK(has(r.text, "# wind-up"));
    CHECK(has(r.text, "# the right hand's reach\n[[track]]"));   // a comment before a table stays with it
    CHECK(has(r.text, "# the body\n[[track]]"));
    CHECK(has(r.text, "name = \"hit\""));

    // The left hand's old rot track is gone; its keys are the right hand's, y and z flipped; the times and eases are kept.
    const anim::Track* lr = find_track(after, anim::Part::HandL, anim::Channel::Rot);
    const anim::Track* rr = find_track(after, anim::Part::HandR, anim::Channel::Rot);
    REQUIRE(lr);
    REQUIRE(rr);
    CHECK(find_track(after, anim::Part::HandL, anim::Channel::Rot, 1) == nullptr);
    CHECK(lr->mode == anim::TrackMode::Add);
    REQUIRE(lr->keys.size() == rr->keys.size());
    for (size_t i = 0; i < rr->keys.size(); ++i) {
        CHECK(lr->keys[i].t == rr->keys[i].t);
        CHECK(lr->keys[i].ease == rr->keys[i].ease);
        CHECK(lr->keys[i].v[0] == rr->keys[i].v[0]);
        CHECK(lr->keys[i].v[1] == -rr->keys[i].v[1]);
        CHECK(lr->keys[i].v[2] == -rr->keys[i].v[2]);
    }
    CHECK(lr->keys[1].v[1] == -20);
    CHECK(lr->keys[2].v[1] == 15);

    // The `set` track on pos: x flipped, the mode kept.
    const anim::Track* lp = find_track(after, anim::Part::HandL, anim::Channel::Pos);
    REQUIRE(lp);
    CHECK(lp->mode == anim::TrackMode::Set);
    REQUIRE(lp->keys.size() == 2);
    CHECK(lp->keys[0].v[0] == -1);
    CHECK(lp->keys[0].v[1] == 2);
    CHECK(lp->keys[1].v[0] == -4);
    CHECK(lp->keys[1].v[2] == 0);

    // The report: two tracks made, the two keys of the old left track replaced, every key of the new tracks selected.
    CHECK(r.made_tracks == 2);
    CHECK(r.replaced == 2);
    REQUIRE(r.selection.size() == 6);
    for (const KeyRef& k : r.selection) CHECK((find_track(after, anim::Part::HandL, anim::Channel::Rot) == &after.tracks[k.track] ||
                                               find_track(after, anim::Part::HandL, anim::Channel::Pos) == &after.tracks[k.track]));

    // The motion is the mirror image at every time: the evaluator gives the left hand the right hand's pose, mirrored.
    for (float u : {0.0f, 0.1f, 0.35f, 0.42f, 0.5f, 0.9f, 1.0f}) {
        float a[3], b[3], m[3];
        anim::sample_track(*rr, u, a);
        anim::sample_track(*lr, u, b);
        mirror_vec(anim::Channel::Rot, a, m);
        for (int c = 0; c < 3; ++c) CHECK(b[c] == doctest::Approx(m[c]).epsilon(1e-6));
    }
}

TEST_CASE("mirror: a second mirror gives the same text, and the other channels of the left hand stay") {
    // A left hand with a pos track of its own, and the right hand with a rot track only.
    const std::string text =
        "duration = 1000\n"
        "[[track]]\n"
        "part = \"hand_l\"\n"
        "channel = \"pos\"\n"
        "keys = [{ t = 0.0, v = [7, 7, 7] }]\n"
        "[[track]]\n"
        "part = \"hand_r\"\n"
        "channel = \"rot\"\n"
        "keys = [{ t = 0.0, v = [1, 2, 3] }, { t = 1.0, v = [4, 5, 6] }]\n";
    KeyEditResult a = mirror_part(text, anim::Part::HandR);
    REQUIRE(a.ok);
    const anim::Track* pos = find_track(a.clip, anim::Part::HandL, anim::Channel::Pos);
    REQUIRE(pos);
    CHECK(pos->keys.size() == 1);
    CHECK(pos->keys[0].v[0] == 7);   // the left hand's pos is the author's: the right has no pos track to give
    const anim::Track* rot = find_track(a.clip, anim::Part::HandL, anim::Channel::Rot);
    REQUIRE(rot);
    CHECK(rot->keys[1].v[1] == -5);
    CHECK(a.replaced == 0);

    KeyEditResult b = mirror_part(a.text, anim::Part::HandR);
    REQUIRE(b.ok);
    CHECK(b.text == a.text);
    CHECK(b.replaced == 2);
}

TEST_CASE("mirror: from the left hand, from a weapon name, and a CRLF file") {
    const std::string text = crlf(
        "duration = 500\n"
        "[[track]]\n"
        "part = \"weapon_l\"\n"
        "channel = \"rot\"\n"
        "keys = [{ t = 0.0, v = [10, 20, 30] }, { t = 1.0, v = [0, 0, 0] }]\n");
    KeyEditResult r = mirror_part(text, anim::Part::HandL);
    REQUIRE(r.ok);
    const anim::Track* t = find_track(r.clip, anim::Part::HandR, anim::Channel::Rot);
    REQUIRE(t);
    CHECK(t->keys[0].v[0] == 10);
    CHECK(t->keys[0].v[1] == -20);
    CHECK(t->keys[0].v[2] == -30);
    // Every line break of the new text is a CRLF.
    for (size_t i = 0; i < r.text.size(); ++i)
        if (r.text[i] == '\n') CHECK(i > 0);
    size_t bare = 0;
    for (size_t i = 0; i < r.text.size(); ++i)
        if (r.text[i] == '\n' && (i == 0 || r.text[i - 1] != '\r')) ++bare;
    CHECK(bare == 0);
}

TEST_CASE("mirror: refusals change nothing") {
    const std::string text = kClip;
    KeyEditResult r = mirror_part(text, anim::Part::Body);   // no pair
    CHECK_FALSE(r.ok);
    CHECK(r.text == text);
    CHECK_FALSE(r.error.empty());

    // The left hand of the sample has tracks; a clip with none for the part says so.
    const std::string only_body =
        "duration = 1000\n[[track]]\npart = \"body\"\nchannel = \"pos\"\nkeys = [{ t = 0.0, v = [0, 0, 1] }]\n";
    r = mirror_part(only_body, anim::Part::HandR);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("no track") != std::string::npos);

    // A clip that does not validate is not edited.
    r = mirror_part("duration = -4\n", anim::Part::HandR);
    CHECK_FALSE(r.ok);

    // Tracks written as an inline list cannot be removed or added as tables.
    const std::string list =
        "duration = 1000\n"
        "track = [\n"
        "  { part = \"hand_l\", channel = \"rot\", keys = [{ t = 0.0, v = [1, 1, 1] }] },\n"
        "  { part = \"hand_r\", channel = \"rot\", keys = [{ t = 0.0, v = [2, 2, 2] }] },\n"
        "]\n";
    r = mirror_part(list, anim::Part::HandR);
    CHECK_FALSE(r.ok);
    CHECK(r.text == list);
}

TEST_CASE("remove_track: one table goes, with its lines, and the comment of the next table stays") {
    const std::string text = kClip;
    std::string out, err;
    // Track 1 is hand_r.rot, followed by a blank line, a comment and the next table.
    REQUIRE(remove_track(text, 1, out, err));
    const anim::Clip c = clip_of(out);
    REQUIRE(c.tracks.size() == 3);
    CHECK(c.tracks[0].part == anim::Part::HandL);
    CHECK(c.tracks[1].channel == anim::Channel::Pos);
    CHECK(c.tracks[2].part == anim::Part::Body);
    CHECK_FALSE(has(out, "wind-up"));
    CHECK(has(out, "# the right hand's reach\n[[track]]"));
    CHECK(has(out, "[[marker]]"));

    // The last table before `[[marker]]`.
    REQUIRE(remove_track(text, 3, out, err));
    CHECK(clip_of(out).tracks.size() == 3);
    CHECK_FALSE(has(out, "# the body"));   // the comment right above a table goes with it
    CHECK(has(out, "# the right hand's reach\n[[track]]"));
    CHECK(has(out, "[[marker]]"));

    CHECK_FALSE(remove_track(text, 4, out, err));
    CHECK_FALSE(remove_track(text, -1, out, err));

    // A single track in a file with nothing after it, and no end-of-line at the end.
    const std::string one = "duration = 100\n[[track]]\npart = \"body\"\nchannel = \"pos\"\nkeys = [{ t = 0.0, v = [0, 0, 0] }]";
    REQUIRE(remove_track(one, 0, out, err));
    CHECK(out == "duration = 100\n");
}

TEST_CASE("inspector: a value is written to one key and nothing else changes") {
    const std::string text = kClip;
    const float v[3] = {-45.5f, 0, 12};
    KeyEditResult r = set_key_vec(text, {1, 1}, v);
    REQUIRE(r.ok);
    const anim::Track& t = r.clip.tracks[1];
    CHECK(t.keys[1].v[0] == doctest::Approx(-45.5f));
    CHECK(t.keys[1].v[1] == 0);
    CHECK(t.keys[1].v[2] == 12);
    CHECK(t.keys[1].t == doctest::Approx(0.35f));
    CHECK(t.keys[1].ease == anim::Ease::EaseOut);   // the time and the ease stay
    CHECK(has(r.text, "# wind-up"));
    CHECK(has(r.text, "# strike"));
    CHECK(r.selection.size() == 1);
    // The key before and the key after are the same numbers.
    CHECK(t.keys[0].v[0] == 0);
    CHECK(t.keys[2].v[0] == 30);

    // Past the loader's bound, or not a number: refused, text as it was.
    const float too_big[3] = {0, 721, 0};
    r = set_key_vec(text, {1, 1}, too_big);
    CHECK_FALSE(r.ok);
    CHECK(r.text == text);
    const float nan3[3] = {NAN, 0, 0};
    CHECK_FALSE(set_key_vec(text, {1, 1}, nan3).ok);
    CHECK_FALSE(set_key_vec(text, {9, 0}, v).ok);
    CHECK(value_limit(anim::Channel::Pos) == 100.0f);
    CHECK(value_limit(anim::Channel::Rot) == 720.0f);
}

TEST_CASE("inspector: the ease of several keys, and a linear ease is not written") {
    const std::string text = kClip;
    KeyEditResult r = set_keys_ease(text, {{1, 1}, {1, 2}}, anim::Ease::Hold);
    REQUIRE(r.ok);
    CHECK(r.clip.tracks[1].keys[1].ease == anim::Ease::Hold);
    CHECK(r.clip.tracks[1].keys[2].ease == anim::Ease::Hold);
    CHECK(r.clip.tracks[1].keys[3].ease == anim::Ease::Smooth);   // not selected
    CHECK(r.clip.tracks[1].keys[1].v[0] == -110);                 // values kept

    r = set_keys_ease(text, {{1, 1}}, anim::Ease::Linear);
    REQUIRE(r.ok);
    CHECK(r.clip.tracks[1].keys[1].ease == anim::Ease::Linear);
    CHECK_FALSE(has(r.text, "ease_out"));
    CHECK(has(r.text, "ease_in"));

    CHECK_FALSE(set_keys_ease(text, {}, anim::Ease::Smooth).ok);
}

TEST_CASE("inspector: reset to neutral, zero for add and the base pose's value for set") {
    const std::string text = kClip;
    anim::Pose base;
    base.parts[static_cast<int>(anim::Part::HandR)].pos[0] = 8;
    base.parts[static_cast<int>(anim::Part::HandR)].pos[1] = -1;
    base.parts[static_cast<int>(anim::Part::HandR)].pos[2] = 0.5f;
    base.parts[static_cast<int>(anim::Part::HandR)].rot[0] = 99;   // an add track does not read it

    KeyEditResult r = reset_keys(text, {{1, 1}, {2, 0}}, base);
    REQUIRE(r.ok);
    const anim::Key& a = r.clip.tracks[1].keys[1];   // hand_r.rot, add
    CHECK(a.v[0] == 0);
    CHECK(a.v[1] == 0);
    CHECK(a.v[2] == 0);
    CHECK(a.t == doctest::Approx(0.35f));
    CHECK(a.ease == anim::Ease::EaseOut);
    const anim::Key& s = r.clip.tracks[2].keys[0];   // hand_r.pos, set
    CHECK(s.v[0] == 8);
    CHECK(s.v[1] == -1);
    CHECK(s.v[2] == 0.5f);
    CHECK(r.clip.tracks[2].keys[1].v[0] == 4);   // the other key of the set track stays
}

TEST_CASE("curve: the samples are the evaluator's, and the range holds the keys") {
    const anim::Clip c = clip_of(kClip);
    const anim::Track& tr = c.tracks[1];   // hand_r.rot, four keys with three eases
    std::vector<float> xs;
    curve_samples(tr, 0, 100, xs);
    REQUIRE(xs.size() == 101);
    for (int i = 0; i <= 100; ++i) {
        float v[3];
        anim::sample_track(tr, i / 100.0f, v);
        CHECK(xs[i] == v[0]);
    }
    CHECK(xs[0] == 0);
    CHECK(xs[35] == doctest::Approx(-110.0f));   // the key at 0.35
    CHECK(xs[100] == 0);

    // The same number as the game's own application of the clip: a `set` track writes the sample into the pose.
    anim::ClipLoad cl = anim::parse_clip(
        "duration = 1000\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nmode = \"set\"\n"
        "keys = [{ t = 0.0, v = [0, 0, 0] }, { t = 0.6, v = [40, -20, 8], ease = \"smooth\" }, { t = 1.0, v = [-10, 5, 0] }]\n",
        "t");
    REQUIRE(cl.ok);
    std::vector<float> ys;
    curve_samples(cl.clip.tracks[0], 1, 50, ys);
    for (int i = 0; i <= 50; i += 5) {
        anim::Pose pose;
        anim::Timing timing;
        timing.elapsed_ms = 1000.0f * static_cast<float>(i) / 50.0f;
        timing.duration_ms = 1000.0f;
        const anim::Applied ap = anim::apply_clip(cl.clip, timing, pose);
        REQUIRE(ap.ok);
        CHECK(pose.parts[static_cast<int>(anim::Part::Head)].rot[1] == doctest::Approx(ys[i]).epsilon(1e-5));
    }

    // Degenerate asks give nothing.
    curve_samples(tr, 3, 10, xs);
    CHECK(xs.empty());
    curve_samples(tr, 0, 0, xs);
    CHECK(xs.empty());

    const CurveRange range = curve_range(tr);
    CHECK(range.lo < -110.0f);
    CHECK(range.hi > 30.0f);
    for (int a = 0; a < 3; ++a) {
        curve_samples(tr, a, 200, xs);
        for (float v : xs) {
            CHECK(v >= range.lo);
            CHECK(v <= range.hi);
        }
    }
    // A flat track still has a span.
    anim::Track flat;
    anim::Key k;
    flat.keys.push_back(k);
    const CurveRange fr = curve_range(flat);
    CHECK(fr.hi - fr.lo >= 1.0f);
    CHECK(fr.lo < 0);
    CHECK(fr.hi > 0);
}

TEST_CASE("curve: a generator track is sampled with the clip's duration, and its range is read from the samples") {
    // covers: animation-editor/Procedural tracks: An oscillation
    anim::ClipLoad cl = anim::parse_clip(R"(duration = 1000
[[track]]
part = "hand_r"
channel = "rot"
osc = { amp = [5, 0, 0], freq = 2 }
)", "gen");
    REQUIRE(cl.ok);
    const anim::Track& tr = cl.clip.tracks[0];
    std::vector<float> xs;
    curve_samples(tr, 0, 8, xs, 1000);   // 2 Hz over one second: two full swings, 4 samples of 8 per swing
    REQUIRE(xs.size() == 9);
    CHECK(xs[0] == doctest::Approx(0).epsilon(1e-5).scale(1));
    CHECK(xs[1] == doctest::Approx(5).epsilon(1e-4).scale(1));    // 125 ms: the top
    CHECK(xs[3] == doctest::Approx(-5).epsilon(1e-4).scale(1));   // 375 ms: the bottom
    CurveRange r = curve_range(tr, 1000);
    CHECK(r.hi > 5);   // the range holds the swing, padded
    CHECK(r.lo < -5);
    // With no duration the generator is read at time 0 and the curve is flat: a caller that has the clip passes its length.
    curve_samples(tr, 0, 4, xs);
    for (float v : xs) CHECK(v == 0);
}

TEST_CASE("mirror: a generator track on either hand is refused, and nothing changes") {
    const std::string text =
        "duration = 1000\n[[track]]\npart = \"hand_r\"\nchannel = \"rot\"\nkeys = [{ t = 0.0, v = [1, 2, 3] }]\n"
        "[[track]]\npart = \"hand_l\"\nchannel = \"rot\"\nosc = { amp = [5, 0, 0], freq = 2 }\n";
    KeyEditResult r = mirror_part(text, anim::Part::HandR);
    CHECK_FALSE(r.ok);
    CHECK(r.text == text);
    CHECK(r.error.find("generator") != std::string::npos);
    r = mirror_part(text, anim::Part::HandL);
    CHECK_FALSE(r.ok);
}
