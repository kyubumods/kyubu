// Unit tests of the Animator's parity check (openspec add-ability-animation task 7.5): the capture reader, the rig's `[capture]` table
// and `slot` keys, the compare, the table, the edit that marks a rig file, and the `--parity` command. No game, no window: the captures
// are made up here from a made-up rig by the inverse of the compare's conversion, so what a game would write is never needed.
#include "doctest.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "json.hpp"
#include "parity.h"

using namespace kanimator;
namespace fs = std::filesystem;
namespace anim = kyubu::game::anim;

namespace {

// A rig with the `[capture]` table: the capture's unit is a tenth of a voxel, a frame's body_yaw turns the figure clockwise, and a
// model sits at its corner.
// A mirrored hand, as the game's has.
const char* kRig = R"(name = "test rig"
[capture]
unit   = 0.1
yaw    = "cw"
origin = "corner"

[[node]]
name  = "root"
part  = "root"
slot  = 0
steps = [["translate", 0, 0, 5], ["rot_z", "root.rot.z"]]

[[node]]
name   = "body"
parent = "root"
part   = "body"
model  = "box 10 6 12"
slot   = 2
steps  = [["translate", 0, 0, 3], ["translate", "body.pos"]]

[[node]]
name   = "head"
parent = "body"
part   = "head"
model  = "box 8 8 8"
slot   = 3
steps  = [["translate", 0, 1, 9], ["rot_x", "head.rot.x"], ["rot_y", "head.rot.y"], ["rot_z", "head.rot.z"], ["translate", "head.pos"]]

[[node]]
name   = "hand_r"
parent = "body"
part   = "hand_r"
model  = "box 4 4 4"
slot   = 10
steps  = [["translate", -6, 0, 0], ["scale", -1, 1, 1], ["rot_x", "hand_r.rot.x"], ["translate", "hand_r.pos"]]

[[node]]
name   = "label"
parent = "head"
model  = "box 1 1 1"
steps  = [["translate", 0, 0, 6]]
)";

Rig load_rig(const std::string& text = kRig) {
    RigLoad r = parse_rig(text, "test.rig.toml");
    for (const ClipError& e : r.errors) MESSAGE(anim::format_error(e));
    REQUIRE(r.ok);
    return r.rig;
}

std::vector<std::array<float, 3>> sizes_of(const Rig& rig) {
    std::vector<std::array<float, 3>> s;
    for (const Node& n : rig.nodes)
        s.push_back(n.model.kind == Model::Kind::Box ? std::array<float, 3>{n.model.box[0], n.model.box[1], n.model.box[2]}
                                                      : std::array<float, 3>{0, 0, 0});
    return s;
}

// The matrix a game would leave for `node` of `solved`: the compare's conversion run backwards. `ref` is the root's translation.
void game_matrix(const Rig& rig, const Solved& solved, size_t node, const std::array<float, 3>& size, const float ref[3], double yaw_deg,
                 float out[16]) {
    const CaptureMap& m = rig.capture;
    const Affine& w = solved.world[node];
    const Affine& root = solved.world[rig.root];
    double col[3][3];   // the images of the model axes, in the rig's frame
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) col[j][k] = w.r[k][j];
    // the game draws the figure turned about z by the frame's yaw, the way `yaw` says
    const double a = m.yaw_sign * yaw_deg * 3.14159265358979323846 / 180.0;
    auto turn = [&](const double v[3], double o[3]) {
        o[0] = std::cos(a) * v[0] - std::sin(a) * v[1];
        o[1] = std::sin(a) * v[0] + std::cos(a) * v[1];
        o[2] = v[2];
    };
    for (int i = 0; i < 3; ++i) {
        double t[3];
        turn(col[i], t);
        for (int j = 0; j < 3; ++j) out[4 * i + j] = static_cast<float>(m.unit * t[j]);
    }
    out[3] = out[7] = out[11] = 0;
    double c[3];
    for (int k = 0; k < 3; ++k) {
        c[k] = w.t[k] - root.t[k];
        if (m.corner)
            for (int i = 0; i < 3; ++i) c[k] -= 0.5 * size[i] * col[i][k];
    }
    double t[3];
    turn(c, t);
    for (int k = 0; k < 3; ++k) out[12 + k] = static_cast<float>(ref[k] + m.unit * t[k]);
    out[15] = 1;
}

// A frame for `pose`: every node with a slot gets its matrix.
CaptureFrame make_frame(const Rig& rig, const Pose& pose, int index, double yaw_deg, const float ref[3]) {
    CaptureFrame f;
    f.index = index;
    f.t_ms = index * 100.0;
    f.view = "front";
    f.body_yaw = yaw_deg;
    f.pose = pose;
    const Solved s = solve_pose(rig, pose);
    const auto sizes = sizes_of(rig);
    for (size_t i = 0; i < rig.nodes.size(); ++i) {
        const int slot = rig.nodes[i].slot;
        if (slot < 0) continue;
        if (static_cast<int>(i) == rig.root) {
            // the root's matrix: its origin (it has no model)
            game_matrix(rig, s, i, {0, 0, 0}, ref, yaw_deg, f.matrix[slot]);
        } else {
            game_matrix(rig, s, i, sizes[i], ref, yaw_deg, f.matrix[slot]);
        }
        f.has_matrix[slot] = true;
    }
    return f;
}

Pose pose_with(float head_x, float head_z, float hand_x, float root_z) {
    Pose p;
    p.parts[static_cast<int>(anim::Part::Head)].rot[0] = head_x;
    p.parts[static_cast<int>(anim::Part::Head)].rot[2] = head_z;
    p.parts[static_cast<int>(anim::Part::HandR)].rot[0] = hand_x;
    p.parts[static_cast<int>(anim::Part::Root)].rot[2] = root_z;
    return p;
}

Capture made_capture() {
    Capture c;
    c.source = "made-up";
    c.kind = "capture";
    const Rig rig = load_rig();
    const float ref[3] = {100, 200, 50};
    c.frames.push_back(make_frame(rig, pose_with(0, 0, 0, 0), 0, 180, ref));
    c.frames.push_back(make_frame(rig, pose_with(25, -40, 70, 0), 1, 90, ref));
    c.frames.push_back(make_frame(rig, pose_with(-10, 15, 120, 90), 2, -37.5, ref));
    return c;
}

const NodeParity& node_of(const Rig& rig, const ParityReport& rep, const char* name) {
    for (const NodeParity& n : rep.nodes)
        if (rig.nodes[n.node].name == name) return n;
    FAIL("no such node in the report: " << name);
    return rep.nodes[0];
}

std::string capture_json(const Capture& c) {
    nlohmann::json j;
    j["format"] = "kyubu-capture/1";
    j["kind"] = c.kind;
    j["clip"] = "made:up";
    j["build"] = "cube-test";
    nlohmann::json frames = nlohmann::json::array();
    for (const CaptureFrame& f : c.frames) {
        nlohmann::json jf;
        jf["index"] = f.index;
        jf["t_ms"] = f.t_ms;
        jf["view"] = f.view;
        jf["body_yaw"] = f.body_yaw;
        nlohmann::json vec;
        for (int p = 0; p < anim::kPartCount; ++p) {
            const anim::Part part = static_cast<anim::Part>(p);
            nlohmann::json pv;
            pv["rot"] = {f.pose.parts[p].rot[0], f.pose.parts[p].rot[1], f.pose.parts[p].rot[2]};
            if (part == anim::Part::Root) pv["pos"] = {f.pose.parts[p].pos[0], f.pose.parts[p].pos[1]};
            else if (part != anim::Part::Tail) pv["pos"] = {f.pose.parts[p].pos[0], f.pose.parts[p].pos[1], f.pose.parts[p].pos[2]};
            vec[anim::part_name(part)] = pv;
        }
        jf["vectors"] = vec;
        nlohmann::json mats = nlohmann::json::array();
        for (int s = 0; s < kCaptureSlots; ++s) {
            nlohmann::json m = nlohmann::json::array();
            for (int k = 0; k < 16; ++k) m.push_back(f.matrix[s][k]);
            mats.push_back(m);
        }
        jf["matrices"] = mats;
        frames.push_back(jf);
    }
    j["frames"] = frames;
    return j.dump();
}

}  // namespace

TEST_CASE("parity: a rig reads the [capture] table and the slots") {
    const Rig rig = load_rig();
    CHECK(rig.capture.present);
    CHECK(rig.capture.unit == doctest::Approx(0.1f));
    CHECK(rig.capture.yaw_sign == -1);
    CHECK(rig.capture.corner);
    CHECK(load_rig("name = \"x\"\n[capture]\nunit = 1\n[[node]]\nname = \"root\"\n").capture.yaw_sign == 1);   // `ccw` is the default
    CHECK(rig.nodes[rig.find("head")].slot == 3);
    CHECK(rig.nodes[rig.find("label")].slot == -1);
    // a rig with no table is still a rig
    const Rig plain = load_rig("name = \"x\"\n[[node]]\nname = \"root\"\n");
    CHECK_FALSE(plain.capture.present);
}

TEST_CASE("parity: a bad [capture] table or slot is refused with its line") {
    auto first_error = [](const std::string& extra, const std::string& node_extra) {
        RigLoad r = parse_rig("name = \"x\"\n" + extra + "[[node]]\nname = \"root\"\n" + node_extra, "t.rig.toml");
        REQUIRE_FALSE(r.ok);
        return r.errors[0];
    };
    CHECK(first_error("[capture]\nunit = 0.1\nyaw = \"left\"\n", "").code == "bad_value");
    CHECK(first_error("[capture]\nyaw = \"cw\"\n", "").code == "missing_field");
    CHECK(first_error("[capture]\nunit = 0\n", "").code == "out_of_range");
    CHECK(first_error("[capture]\nunit = 1\norigin = \"middle\"\n", "").code == "bad_value");
    CHECK(first_error("[capture]\nunit = 1\nscale = 2\n", "").code == "unknown_key");
    CHECK(first_error("", "slot = 17\n").code == "out_of_range");
    CHECK(first_error("", "slot = \"3\"\n").code == "bad_type");
}

TEST_CASE("parity: a capture file is read, and a bad one names its field") {
    const Capture c = made_capture();
    CaptureLoad ok = parse_capture(capture_json(c), "made-up");
    REQUIRE(ok.ok);
    CHECK(ok.capture.frames.size() == 3);
    CHECK(ok.capture.kind == "capture");
    CHECK(ok.capture.frames[1].pose.parts[static_cast<int>(anim::Part::Head)].rot[2] == doctest::Approx(-40.0f));
    CHECK(ok.capture.frames[0].has_matrix[3]);
    CHECK_FALSE(ok.capture.frames[0].has_matrix[5]);   // the game left nothing there
    CHECK(ok.capture.frames[0].matrix[3][14] == doctest::Approx(c.frames[0].matrix[3][14]));

    CHECK_FALSE(parse_capture("[1]", "x").ok);
    CHECK(parse_capture("{\"format\": \"other\"}", "x").error.find("format") != std::string::npos);
    nlohmann::json j = nlohmann::json::parse(capture_json(c));
    j["frames"][1]["matrices"].erase(5);
    const CaptureLoad bad = parse_capture(j.dump(), "x");
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("frames[1].matrices") != std::string::npos);
    j = nlohmann::json::parse(capture_json(c));
    j["frames"][0]["matrices"][3][5] = nullptr;   // a number that was not finite: the part is hidden, the matrix is absent
    const CaptureLoad hidden = parse_capture(j.dump(), "x");
    REQUIRE(hidden.ok);
    CHECK_FALSE(hidden.capture.frames[0].has_matrix[3]);
    CHECK(hidden.capture.frames[0].has_matrix[2]);
}

TEST_CASE("parity: a capture made from the rig itself matches it exactly") {
    const Rig rig = load_rig();
    const ParityReport rep = run_parity(rig, sizes_of(rig), {made_capture()});
    REQUIRE(rep.ok);
    CHECK(rep.nodes.size() == 3);   // body, head and hand_r: the root is the reference and `label` has no slot
    CHECK(rep.frames == 3);
    Tolerance tol;
    for (const NodeParity& n : rep.nodes) {
        CAPTURE(rig.nodes[n.node].name);
        CHECK(n.frames == 3);
        CHECK(n.max_pos < 0.01);
        CHECK(n.max_angle < 0.05);
        CHECK(n.mirrored == 0);
        CHECK_FALSE(n.outside(tol));
    }
}

TEST_CASE("parity: a part that does not match is named, with its error and its frame") {
    const Rig rig = load_rig();
    Capture c = made_capture();
    // the right hand 0.6 voxel off in the capture's x (the rig's -x): the matrix translation moves by 0.06 of the capture's unit
    c.frames[1].matrix[10][12] += 0.06f;
    // the head turned 3 degrees about the capture's z more than the rig says: rotate its two first rows
    {
        float* m = c.frames[2].matrix[3];
        const double a = 3.0 * 3.14159265358979323846 / 180.0;
        float r0[3] = {m[0], m[1], m[2]}, r1[3] = {m[4], m[5], m[6]};
        for (int k = 0; k < 3; ++k) {
            m[k] = static_cast<float>(std::cos(a) * r0[k] + std::sin(a) * r1[k]);
            m[4 + k] = static_cast<float>(-std::sin(a) * r0[k] + std::cos(a) * r1[k]);
        }
    }
    const ParityReport rep = run_parity(rig, sizes_of(rig), {c});
    REQUIRE(rep.ok);
    Tolerance tol;
    const NodeParity& hand = node_of(rig, rep, "hand_r");
    CHECK(hand.max_pos == doctest::Approx(0.6).epsilon(0.02));
    CHECK(hand.pos_at == "made-up frame 1");
    CHECK(hand.outside(tol));
    const NodeParity& head = node_of(rig, rep, "head");
    CHECK(head.max_angle == doctest::Approx(3.0).epsilon(0.02));
    CHECK(head.angle_at == "made-up frame 2");
    CHECK(head.outside(tol));
    CHECK_FALSE(node_of(rig, rep, "body").outside(tol));
    // a looser tolerance accepts the head's 3 degrees (and the 0.3 voxel its corner's turn moves the centre) but not the hand's 0.6 voxel
    Tolerance loose;
    loose.pos = 0.5;
    loose.angle = 4.0;
    CHECK_FALSE(head.outside(loose));
    CHECK(hand.outside(loose));

    const std::string table = parity_table(rig, rep, tol);
    CHECK(table.find("hand_r") != std::string::npos);
    CHECK(table.find("APPROXIMATE (position)") != std::string::npos);
    // the head's matrix was turned about its own corner, so the centre it stands for moved too
    CHECK(table.find("APPROXIMATE (position, angle)") != std::string::npos);
    CHECK(table.find("made-up frame 1") != std::string::npos);
    CHECK(table.find("2 node(s) outside the tolerance") != std::string::npos);
}

TEST_CASE("parity: a mirrored model in one and not the other is its own verdict") {
    const Rig rig = load_rig();
    Capture c = made_capture();
    for (int k = 8; k < 11; ++k) c.frames[0].matrix[10][k] = -c.frames[0].matrix[10][k];   // the hand's third axis the other way
    const ParityReport rep = run_parity(rig, sizes_of(rig), {c});
    REQUIRE(rep.ok);
    const NodeParity& hand = node_of(rig, rep, "hand_r");
    CHECK(hand.mirrored == 1);
    CHECK(hand.outside(Tolerance{}));
    CHECK(parity_table(rig, rep, Tolerance{}).find("mirror") != std::string::npos);
}

TEST_CASE("parity: a slot the game left empty is skipped, and a frame with no root matrix is not compared") {
    const Rig rig = load_rig();
    Capture c = made_capture();
    c.frames[1].has_matrix[10] = false;
    c.frames[2].has_matrix[0] = false;
    const ParityReport rep = run_parity(rig, sizes_of(rig), {c});
    REQUIRE(rep.ok);
    CHECK(rep.skipped == 1);
    CHECK(node_of(rig, rep, "hand_r").frames == 1);   // frame 1 has no hand, frame 2 has no root
    CHECK(node_of(rig, rep, "head").frames == 2);
    CHECK_FALSE(rep.notes.empty());
}

TEST_CASE("parity: a rig that cannot be checked is refused with the reason") {
    const Rig plain = load_rig("name = \"x\"\n[[node]]\nname = \"root\"\n");
    ParityReport r = run_parity(plain, {{0, 0, 0}}, {});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("[capture]") != std::string::npos);
    const Rig no_root_slot = load_rig("name = \"x\"\n[capture]\nunit = 1\n[[node]]\nname = \"root\"\n");
    r = run_parity(no_root_slot, {{0, 0, 0}}, {});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("root") != std::string::npos);
}

TEST_CASE("parity: marking a rig file changes the approximate lines and nothing else") {
    const std::string text =
        "name = \"x\"\r\n# a comment\r\n[[node]]\r\nname = \"root\"\r\n[[node]]\r\nparent = \"root\"\r\nname   = \"head\"\r\n"
        "steps  = [[\"translate\", 0, 0, 1],\r\n          [\"rot_x\", 5]]\r\n"
        "[[node]]\r\nname = \"hand\"\r\nparent = \"root\"\r\napproximate = false   # measured\r\n";
    std::string out, err;
    REQUIRE(mark_approximate(text, {"head", "hand"}, out, err));
    CHECK(out ==
          "name = \"x\"\r\n# a comment\r\n[[node]]\r\nname = \"root\"\r\n[[node]]\r\nparent = \"root\"\r\nname   = \"head\"\r\n"
          "approximate = true\r\n"
          "steps  = [[\"translate\", 0, 0, 1],\r\n          [\"rot_x\", 5]]\r\n"
          "[[node]]\r\nname = \"hand\"\r\nparent = \"root\"\r\napproximate = true  # measured\r\n");
    // and it still parses, with the marks
    const Rig back = load_rig(out);
    CHECK(back.nodes[back.find("head")].approximate);
    // marking again changes nothing
    std::string again;
    REQUIRE(mark_approximate(out, {"head", "hand"}, again, err));
    CHECK(again == out);
    // an unknown node is an error and writes nothing
    CHECK_FALSE(mark_approximate(text, {"tail"}, out, err));
    CHECK(err.find("tail") != std::string::npos);
    // a table's `[capture]` header and an LF file
    const std::string lf = "name = \"x\"\n[capture]\nunit = 1\n[[node]]\nname = \"a\"\n";
    REQUIRE(mark_approximate(lf, {"a"}, out, err));
    CHECK(out == "name = \"x\"\n[capture]\nunit = 1\n[[node]]\nname = \"a\"\napproximate = true\n");
}

TEST_CASE("parity: the command reads a capture folder, prints the table, exits 3 and marks the rig") {
    const fs::path dir = fs::temp_directory_path() / "kyubu_parity_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "caps" / "cap_a");
    fs::create_directories(dir / "caps" / "cap_b");
    const Rig rig = load_rig();
    Capture a = made_capture();
    Capture b = made_capture();
    b.frames[1].matrix[10][12] += 0.06f;   // cap_b's hand is off
    {
        std::ofstream(dir / "caps" / "cap_a" / "pose.json") << capture_json(a);
        std::ofstream(dir / "caps" / "cap_b" / "pose.json") << capture_json(b);
        std::ofstream(dir / "t.rig.toml", std::ios::binary) << kRig;
    }
    ParityCommand cmd;
    cmd.capture_paths = {(dir / "caps").string()};
    cmd.rig_path = (dir / "t.rig.toml").string();
    cmd.mark = true;
    std::FILE* fo = std::tmpfile();
    std::FILE* fe = std::tmpfile();
    REQUIRE(fo);
    REQUIRE(fe);
    const int rc = run_parity_command(cmd, fo, fe);
    auto drain = [](std::FILE* f) {
        std::rewind(f);
        std::string s;
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        std::fclose(f);
        return s;
    };
    const std::string out = drain(fo), err = drain(fe);
    CAPTURE(out);
    CAPTURE(err);
    CHECK(rc == 3);
    CHECK(out.find("2 capture(s), 6 frame(s)") != std::string::npos);
    CHECK(out.find("cap_b frame 1") != std::string::npos);
    CHECK(out.find("rig file updated") != std::string::npos);
    std::ifstream rf(dir / "t.rig.toml", std::ios::binary);
    const std::string marked((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    const Rig back = load_rig(marked);
    CHECK(back.nodes[back.find("hand_r")].approximate);
    CHECK_FALSE(back.nodes[back.find("head")].approximate);
    CHECK_FALSE(back.nodes[back.find("label")].approximate);   // it has no slot: never measured, never marked

    // a run over the good capture alone is within the tolerance (exit 0); it leaves the mark that is there
    cmd.capture_paths = {(dir / "caps" / "cap_a").string()};
    std::FILE* fo2 = std::tmpfile();
    std::FILE* fe2 = std::tmpfile();
    CHECK(run_parity_command(cmd, fo2, fe2) == 0);
    CHECK(drain(fo2).find("rig file unchanged") != std::string::npos);
    drain(fe2);
    std::ifstream rf2(dir / "t.rig.toml", std::ios::binary);
    const std::string kept((std::istreambuf_iterator<char>(rf2)), std::istreambuf_iterator<char>());
    CHECK(kept == marked);

    // a missing capture and a missing rig are errors
    cmd.capture_paths = {(dir / "nowhere").string()};
    std::FILE* fo3 = std::tmpfile();
    std::FILE* fe3 = std::tmpfile();
    CHECK(run_parity_command(cmd, fo3, fe3) == 1);
    CHECK(drain(fe3).find("no pose.json") != std::string::npos);
    drain(fo3);
    fs::remove_all(dir, ec);
}
