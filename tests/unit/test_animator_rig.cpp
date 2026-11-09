// Unit tests of the Animator's rig format and solver (openspec add-ability-animation task 6.2): each step, the order of steps, a
// position carried through a rotation, `add` and `set` over a base pose, a part the rig lacks, one bad rig per message, the
// stand-in rig, and the solver's part values against `kanim sample`. No game.
#include "doctest.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "kanim_core.h"
#include "rig.h"

using namespace kanimator;
namespace anim = kyubu::game::anim;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "cannot read " << path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

Rig good_rig(const std::string& text) {
    RigLoad r = parse_rig(text, "t.rig.toml");
    for (const ClipError& e : r.errors) MESSAGE(anim::format_error(e));
    REQUIRE(r.ok);
    return r.rig;
}

// A rig of one node `n` with `steps` (TOML array text) under a root with no steps.
Rig one_node(const std::string& steps) {
    return good_rig("name = \"t\"\n[[node]]\nname = \"root\"\n[[node]]\nname = \"n\"\nparent = \"root\"\nsteps = " + steps + "\n");
}

// Where the node's origin or another point of the node goes.
void where(const Rig& rig, const Pose& pose, const char* node, const float p[3], float out[3]) {
    Solved s = solve_pose(rig, pose);
    int i = rig.find(node);
    REQUIRE(i >= 0);
    s.world[i].apply(p, out);
}

const float kOrigin[3] = {0, 0, 0};

void expect(const float got[3], float x, float y, float z) {
    CHECK(got[0] == doctest::Approx(x).epsilon(1e-5).scale(1));
    CHECK(got[1] == doctest::Approx(y).epsilon(1e-5).scale(1));
    CHECK(got[2] == doctest::Approx(z).epsilon(1e-5).scale(1));
}

anim::Clip clip_of(const std::string& text) {
    anim::ClipLoad c = anim::parse_clip(text, "c.anim.toml");
    for (const ClipError& e : c.errors) MESSAGE(anim::format_error(e));
    REQUIRE(c.ok);
    return c.clip;
}

anim::Timing at_ms(float ms, float duration) {
    anim::Timing t;
    t.elapsed_ms = ms;
    t.duration_ms = duration;
    t.run_ms = duration;
    return t;
}

const std::string kStandIn = KYUBU_SOURCE_DIR "/tools/animator/rigs/standin.rig.toml";

}  // namespace

TEST_CASE("rig: each step moves a point as the frame of docs/animations.md says") {
    Pose zero;
    float out[3];
    const float up[3] = {0, 0, 1}, fwd[3] = {0, 1, 0}, right[3] = {1, 0, 0};

    where(one_node("[[\"translate\", 1, 2, 3]]"), zero, "n", kOrigin, out);
    expect(out, 1, 2, 3);
    // Positive x on rot tips the top of a part backward (toward -y).
    where(one_node("[[\"rot_x\", 90]]"), zero, "n", up, out);
    expect(out, 0, -1, 0);
    // Positive y rolls the top toward the player's right (+x).
    where(one_node("[[\"rot_y\", 90]]"), zero, "n", up, out);
    expect(out, 1, 0, 0);
    // Positive z turns counter-clockwise seen from above: right goes to forward.
    where(one_node("[[\"rot_z\", 90]]"), zero, "n", right, out);
    expect(out, 0, 1, 0);
    where(one_node("[[\"scale\", 2]]"), zero, "n", fwd, out);
    expect(out, 0, 2, 0);
    const float p[3] = {1, 1, 1};
    where(one_node("[[\"scale\", 1, 2, 3]]"), zero, "n", p, out);
    expect(out, 1, 2, 3);
}

TEST_CASE("rig: a child follows its parent, and the first step is the outermost") {
    Pose zero;
    float out[3];
    // The parent moves its child's origin by its own steps; the child's matrix is the parent's times its own.
    Rig r = good_rig(
        "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"translate\", 10, 0, 0], [\"rot_z\", 90]]\n"
        "[[node]]\nname = \"b\"\nparent = \"a\"\nsteps = [[\"translate\", 5, 0, 0]]\n");
    where(r, zero, "b", kOrigin, out);
    expect(out, 10, 5, 0);   // 5 forward of a, which is turned 90 degrees at x = 10

    // translate then rotate is not rotate then translate.
    const float one_x[3] = {1, 0, 0};
    where(one_node("[[\"translate\", 10, 0, 0], [\"rot_z\", 90]]"), zero, "n", one_x, out);
    expect(out, 10, 1, 0);   // the point is turned first, then carried 10 along x
    where(one_node("[[\"rot_z\", 90], [\"translate\", 10, 0, 0]]"), zero, "n", one_x, out);
    expect(out, 0, 11, 0);   // carried first, then turned

    // Three angles: the last one acts on the point first. (1,0,0) turned 90 about z gives (0,1,0), then 90 about x gives (0,0,1).
    where(one_node("[[\"rot_x\", 90], [\"rot_z\", 90]]"), zero, "n", one_x, out);
    expect(out, 0, 0, 1);
    where(one_node("[[\"rot_z\", 90], [\"rot_x\", 90]]"), zero, "n", one_x, out);
    expect(out, 0, 1, 0);   // x does nothing to (1,0,0); then z takes it to (0,1,0)
}

TEST_CASE("rig: pose values are arguments, and a position goes through the rotation") {
    Pose pose;
    pose.parts[static_cast<int>(Part::Head)].pos[0] = 20;
    pose.parts[static_cast<int>(Part::Head)].rot[2] = 90;
    float out[3];
    // The stand-in order: angles outside, then the position. Measured in the game: pos 20 on x with rot 90 on z goes sideways.
    Rig r = one_node("[[\"rot_z\", \"head.rot.z\"], [\"translate\", \"head.pos\"]]");
    where(r, pose, "n", kOrigin, out);
    expect(out, 0, 20, 0);
    // The position by its three axes, and negated.
    Rig axes = one_node("[[\"translate\", \"head.pos.x\", 0, \"head.pos.x\"]]");
    where(axes, pose, "n", kOrigin, out);
    expect(out, 20, 0, 20);
    Rig neg = one_node("[[\"translate\", \"-head.pos\"], [\"rot_z\", \"-head.rot.z\"]]");
    const float one_x[3] = {1, 0, 0};
    where(neg, pose, "n", one_x, out);
    expect(out, -20, -1, 0);   // turned -90 (x goes to -y), carried -20 on x
    // A weapon name is the hand's.
    Rig w = one_node("[[\"rot_x\", \"weapon_r.rot.x\"]]");
    CHECK(w.parts_used() == (1u << static_cast<int>(Part::HandR)));
}

TEST_CASE("rig: add and set over a base pose, through the loader's evaluator") {
    Pose base;
    base.parts[static_cast<int>(Part::Head)].rot[0] = 10;
    base.parts[static_cast<int>(Part::Head)].pos[2] = 1;
    Rig r = good_rig("name = \"t\"\n[[node]]\nname = \"head\"\npart = \"head\"\nsteps = [[\"rot_z\", \"head.rot.z\"], "
                     "[\"rot_x\", \"head.rot.x\"], [\"translate\", \"head.pos\"]]\n");
    const char* head = "duration = 1000\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nmode = \"%s\"\n"
                       "keys = [{ t = 0, v = [0, 0, 30] }, { t = 1, v = [0, 0, 30] }]\n";
    char text[256];
    std::snprintf(text, sizeof text, head, "add");
    Solved add = solve(r, base, clip_of(text), at_ms(500, 1000));
    REQUIRE(add.applied.ok);
    CHECK(add.pose.parts[static_cast<int>(Part::Head)].rot[0] == doctest::Approx(10));
    CHECK(add.pose.parts[static_cast<int>(Part::Head)].rot[2] == doctest::Approx(30));
    CHECK(add.pose.parts[static_cast<int>(Part::Head)].pos[2] == doctest::Approx(1));   // the position is not a track's
    CHECK(base.parts[static_cast<int>(Part::Head)].rot[2] == 0);                          // the base pose is not changed

    std::snprintf(text, sizeof text, head, "set");
    Solved set = solve(r, base, clip_of(text), at_ms(500, 1000));
    CHECK(set.pose.parts[static_cast<int>(Part::Head)].rot[0] == doctest::Approx(0));    // the vector is replaced
    CHECK(set.pose.parts[static_cast<int>(Part::Head)].rot[2] == doctest::Approx(30));
    // The node's matrix is the one of the pose the clip left: z of 30 then x of 10 (add) turns the point (1,0,0).
    const float one_x[3] = {1, 0, 0};
    float a[3], b[3];
    add.world[0].apply(one_x, a);
    Pose want = add.pose;
    where(r, want, "head", one_x, b);
    expect(a, b[0], b[1], b[2]);
}

TEST_CASE("rig: a part the rig lacks is reported, and nothing else changes") {
    Rig r = good_rig("name = \"t\"\n[[node]]\nname = \"head\"\npart = \"head\"\nsteps = [[\"rot_x\", \"head.rot.x\"]]\n");
    anim::Clip c = clip_of("duration = 1000\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nkeys = [{ t = 0, v = [20, 0, 0] }]\n"
                           "[[track]]\npart = \"tail\"\nchannel = \"pos\"\nkeys = [{ t = 0, v = [0, 5, 0] }]\n"
                           "[[track]]\npart = \"hand_l\"\nchannel = \"rot\"\nkeys = [{ t = 0, v = [0, 0, 9] }]\n");
    Solved s = solve(r, Pose{}, c, at_ms(0, 1000));
    REQUIRE(s.applied.ok);
    CHECK(s.missing_parts == ((1u << static_cast<int>(Part::Tail)) | (1u << static_cast<int>(Part::HandL))));
    CHECK(s.pose.parts[static_cast<int>(Part::Tail)].pos[1] == doctest::Approx(5));   // the pose still carries the value
    float out[3];
    const float up[3] = {0, 0, 1};
    s.world[0].apply(up, out);
    expect(out, 0, -std::sin(20 * 3.14159265f / 180), std::cos(20 * 3.14159265f / 180));
}

TEST_CASE("rig: the stand-in rig loads, has the clip's parts, and stands at rest where it should") {
    RigLoad r = parse_rig(slurp(kStandIn), "standin.rig.toml");
    for (const ClipError& e : r.errors) MESSAGE(anim::format_error(e));
    REQUIRE(r.ok);
    CHECK(r.rig.name == "stand-in rig");
    CHECK(r.rig.nodes.size() == 7);
    CHECK(r.rig.nodes[r.rig.root].name == "root");
    CHECK(r.rig.order.size() == r.rig.nodes.size());
    // A parent comes before its children.
    std::vector<int> seen(r.rig.nodes.size(), 0);
    for (int i : r.rig.order) {
        if (r.rig.nodes[i].parent >= 0) CHECK(seen[r.rig.nodes[i].parent] == 1);
        seen[i] = 1;
    }
    CHECK(r.rig.parts_used() == (1u << anim::kPartCount) - 1);   // every part of a clip has a node
    float out[3];
    where(r.rig, Pose{}, "head", kOrigin, out);
    expect(out, 0, 0, 29);   // body at 18, head 11 above it
    // The weapon hangs from hand B: its node is the hand's with a 7 offset.
    const float seven[3] = {0, 0, 7};
    float hand[3], weapon[3];
    Pose pose;
    pose.parts[static_cast<int>(Part::HandR)].rot[1] = 40;
    where(r.rig, pose, "hand_r", seven, hand);
    where(r.rig, pose, "weapon_r", kOrigin, weapon);
    expect(weapon, hand[0], hand[1], hand[2]);
}

TEST_CASE("rig: the solver's part values equal kanim sample on the design's example clip") {
    const std::string clip_path = KYUBU_SOURCE_DIR "/tests/fixtures/kanim/design_example.anim.toml";
    const std::string text = slurp(clip_path);
    anim::Clip clip = clip_of(text);
    RigLoad r = parse_rig(slurp(kStandIn), "standin.rig.toml");
    REQUIRE(r.ok);

    kyubu::kanim::Options o;
    o.times = {0.0f, 0.1f, 0.25f, 0.35f, 0.5f, 0.75f, 1.0f};
    kyubu::kanim::Result s = kyubu::kanim::sample(text, "design_example.anim.toml", o);
    REQUIRE(s.code == 0);
    // "t <u> (..), weight w" opens a block; "  hand_r: rot a b c" is a part's line. Parts are listed only when a track wrote them.
    std::map<std::string, std::vector<double>> rot_at;   // by the time's text
    std::string cur;
    size_t pos = 0;
    while (pos < s.text.size()) {
        size_t nl = s.text.find('\n', pos);
        std::string line = s.text.substr(pos, nl - pos);
        pos = nl == std::string::npos ? s.text.size() : nl + 1;
        if (line.rfind("t ", 0) == 0) {
            cur = line.substr(2, line.find(' ', 2) - 2);
        } else if (line.rfind("  hand_r: rot ", 0) == 0) {
            std::vector<double> v;
            const char* p = line.c_str() + 14;
            for (int i = 0; i < 3; ++i) {
                char* end;
                v.push_back(std::strtod(p, &end));
                p = end;
            }
            rot_at[cur] = v;
        }
    }
    REQUIRE(rot_at.size() == o.times.size());

    for (float u : o.times) {
        char key[32];
        std::snprintf(key, sizeof key, "%g", u);
        REQUIRE(rot_at.count(key) == 1);
        const std::vector<double>& want = rot_at[key];
        // kanim's duration: "ability" with none given is the assumed 1000 ms, and the run lasts the whole clip.
        anim::Timing t = at_ms(u * kyubu::kanim::kAssumedAbilityMs, kyubu::kanim::kAssumedAbilityMs);
        Solved got = solve(r.rig, Pose{}, clip, t);
        REQUIRE(got.applied.ok);
        const anim::PartPose& hand = got.pose.parts[static_cast<int>(Part::HandR)];
        // kanim prints three significant digits, so the text is within half a unit of its last digit (0.5% of the value).
        for (int i = 0; i < 3; ++i) CHECK_MESSAGE(hand.rot[i] == doctest::Approx(want[i]).epsilon(0.006).scale(0.1), "t " << key << " axis " << i);
        // And it is the evaluator's own number, bit for bit.
        Pose direct;
        anim::apply_clip(clip, t, direct);
        for (int p = 0; p < anim::kPartCount; ++p)
            for (int i = 0; i < 3; ++i) {
                CHECK(got.pose.parts[p].pos[i] == direct.parts[p].pos[i]);
                CHECK(got.pose.parts[p].rot[i] == direct.parts[p].rot[i]);
            }
        CHECK(got.missing_parts == 0);
    }
}

namespace {

struct BadRig {
    const char* what;
    const char* text;
    const char* code;
    const char* path;
    int line;   // 0: the error is about the whole file
};

}  // namespace

TEST_CASE("rig: one bad rig for each message carries its line and field path") {
    const BadRig cases[] = {
        {"syntax", "name = \n", "toml_syntax", "", 1},
        {"no name", "[[node]]\nname = \"a\"\n", "missing_field", "name", 0},
        {"name type", "name = 4\n[[node]]\nname = \"a\"\n", "bad_type", "name", 1},
        {"top key", "name = \"t\"\nnode_count = 2\n[[node]]\nname = \"a\"\n", "unknown_key", "node_count", 2},
        {"no node", "name = \"t\"\n", "missing_field", "node", 0},
        {"node key", "name = \"t\"\n[[node]]\nname = \"a\"\nparnet = \"b\"\n", "unknown_key", "node[0].parnet", 4},
        {"node name missing", "name = \"t\"\n[[node]]\nmodel = \"box 1 1 1\"\n", "missing_field", "node[0].name", 2},
        {"bad node name", "name = \"t\"\n[[node]]\nname = \"Head\"\n", "bad_name", "node[0].name", 3},
        {"duplicate", "name = \"t\"\n[[node]]\nname = \"a\"\n[[node]]\nname = \"a\"\nparent = \"a\"\n", "duplicate_node", "node[1].name", 5},
        {"unknown parent", "name = \"t\"\n[[node]]\nname = \"a\"\n[[node]]\nname = \"b\"\nparent = \"nobody\"\n", "unknown_parent",
         "node[1].parent", 6},
        {"two roots", "name = \"t\"\n[[node]]\nname = \"a\"\n[[node]]\nname = \"b\"\n", "two_roots", "node[1]", 4},
        {"no root", "name = \"t\"\n[[node]]\nname = \"a\"\nparent = \"b\"\n[[node]]\nname = \"b\"\nparent = \"a\"\n", "no_root", "node", 0},
        {"cycle", "name = \"t\"\n[[node]]\nname = \"r\"\n[[node]]\nname = \"a\"\nparent = \"b\"\n[[node]]\nname = \"b\"\nparent = \"a\"\n",
         "parent_cycle", "node[1].parent", 4},
        {"unknown part", "name = \"t\"\n[[node]]\nname = \"a\"\npart = \"foot_r\"\n", "unknown_part", "node[0].part", 4},
        {"box sizes", "name = \"t\"\n[[node]]\nname = \"a\"\nmodel = \"box 0 4 4\"\n", "bad_model", "node[0].model", 4},
        {"model name", "name = \"t\"\n[[node]]\nname = \"a\"\nmodel = \"My Model\"\n", "bad_model", "node[0].model", 4},
        {"steps type", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = 3\n", "bad_type", "node[0].steps", 4},
        {"step type", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [\"rot_x\"]\n", "bad_type", "node[0].steps[0]", 4},
        {"step kind", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"spin\", 3]]\n", "unknown_step", "node[0].steps[0][0]", 4},
        {"translate count", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"translate\", 1, 2]]\n", "bad_arg_count", "node[0].steps[0]", 4},
        {"rot count", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"rot_x\", \"head.pos\"]]\n", "bad_arg_count", "node[0].steps[0]", 4},
        {"scale count", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"scale\", 1, 2]]\n", "bad_arg_count", "node[0].steps[0]", 4},
        {"ref part", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"rot_x\", \"nose.rot.x\"]]\n", "unknown_ref", "node[0].steps[0][1]", 4},
        {"ref channel", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"rot_x\", \"head.turn.x\"]]\n", "unknown_ref", "node[0].steps[0][1]", 4},
        {"ref axis", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"rot_x\", \"head.rot.w\"]]\n", "unknown_ref", "node[0].steps[0][1]", 4},
        {"ref dot", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"rot_x\", \"head\"]]\n", "unknown_ref", "node[0].steps[0][1]", 4},
        {"arg type", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"rot_x\", true]]\n", "bad_type", "node[0].steps[0][1]", 4},
        {"arg range", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"translate\", 0, 0, 5000]]\n", "out_of_range", "node[0].steps[0][3]", 4},
        {"arg nan", "name = \"t\"\n[[node]]\nname = \"a\"\nsteps = [[\"rot_x\", nan]]\n", "not_finite", "node[0].steps[0][1]", 4},
    };
    for (const BadRig& c : cases) {
        INFO("case: " << c.what);
        RigLoad r = parse_rig(c.text, "bad.rig.toml");
        REQUIRE_FALSE(r.ok);
        REQUIRE_FALSE(r.errors.empty());
        const ClipError& e = r.errors[0];
        CHECK(e.code == c.code);
        CHECK(e.path == c.path);
        CHECK(e.line == c.line);
        CHECK(e.source == "bad.rig.toml");
        CHECK_FALSE(e.message.empty());
        CHECK_FALSE(e.hint.empty());
        CHECK(r.rig.nodes.empty());   // a refused rig is empty
    }
    // The text of the spec's scenario: a parent that does not exist.
    RigLoad r = parse_rig("name = \"t\"\n[[node]]\nname = \"a\"\n[[node]]\nname = \"b\"\nparent = \"nobody\"\n", "x.rig.toml");
    REQUIRE(r.errors.size() == 1);
    CHECK(anim::format_error(r.errors[0]) ==
          "x.rig.toml:6: node[1].parent: the parent \"nobody\" is not the name of any node");
}

TEST_CASE("rig: every error of a file is listed, in the order of the lines") {
    RigLoad r = parse_rig("name = \"t\"\n[[node]]\nname = \"a\"\nmodel = \"box 0 1 1\"\n[[node]]\nname = \"b\"\nparent = \"a\"\n"
                          "part = \"nose\"\nsteps = [[\"rot_x\", true]]\n", "m.rig.toml");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.errors.size() == 3);
    CHECK(r.errors[0].path == "node[0].model");
    CHECK(r.errors[1].path == "node[1].part");
    CHECK(r.errors[2].path == "node[1].steps[0][1]");
}
