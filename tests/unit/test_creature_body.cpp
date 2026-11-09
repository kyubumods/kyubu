// Creature bodies, the pure part (openspec add-dragon-boss task 3.3): definition refusals, the Animator parity, cross-fade, markers, joint
// offsets, piece matrices and posed points.
#include "doctest.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

#include "creature_body_core.h"

using namespace kyubu::game;
namespace anim = kyubu::game::anim;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "cannot read " << path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

const std::string kDir = KYUBU_SOURCE_DIR "/tests/fixtures/rig/";

std::vector<body::PieceSpec> dragon_pieces(const std::string& rig_text) {
    kanimator::RigLoad rl = kanimator::parse_rig(rig_text, "dragon.rig.toml");
    REQUIRE(rl.ok);
    std::vector<body::PieceSpec> v;
    uint32_t slot = 3000;
    for (const kanimator::Node& n : rl.rig.nodes)
        if (n.model.kind != kanimator::Model::Kind::None) {
            body::PieceSpec p;
            p.node = n.name;
            p.slot = slot++;
            p.size[0] = 8; p.size[1] = 8; p.size[2] = 8;
            v.push_back(p);
        }
    return v;
}

// a clip that turns the head 40 degrees about x over 1000 ms; marker "hit" at 0.5
const char* kTurn =
    "duration = 1000\nend = \"hold\"\n\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nmode = \"add\"\n"
    "keys = [ { t = 0.0, v = [0, 0, 0] }, { t = 1.0, v = [40, 0, 0] } ]\n\n[[marker]]\nt = 0.5\nname = \"hit\"\n";
const char* kRest = "duration = 500\nend = \"loop\"\n\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nmode = \"add\"\nkeys = [ { t = 0.0, v = [0, 0, 0] }, { t = 1.0, v = [0, 0, 0] } ]\n\n[[marker]]\nt = 0.25\nname = \"tick\"\n";

struct Fx {
    std::string rig = slurp(kDir + "dragon.rig.toml");
    std::string bite = slurp(kDir + "bite.anim.toml");
    std::vector<body::PieceSpec> pieces = dragon_pieces(rig);
    body::DefineResult make() { return body::define("dragon", rig, {{"bite", bite}, {"turn", kTurn}, {"rest", kRest}}, pieces); }
};

}  // namespace

TEST_CASE("creature body: the dragon's body defines with 26 pieces and its clips") {
    Fx f;
    body::DefineResult r = f.make();
    INFO(r.error);
    REQUIRE(r.def);
    CHECK(r.def->pieces.size() == 26);
    CHECK(r.def->clips.size() == 3);
    CHECK(r.def->clip_index("bite") == 0);
    CHECK(r.def->clip_index("nope") == -1);
    CHECK(r.def->clips[0].duration_ms == 1700);
}

TEST_CASE("creature body: refusals name the reason") {
    Fx f;
    SUBCASE("a piece without a model") {
        std::string gone = f.pieces.back().node;
        f.pieces.pop_back();
        body::DefineResult r = f.make();
        CHECK_FALSE(r.def);
        CHECK(r.error.find(gone) != std::string::npos);
    }
    SUBCASE("a spec for a node that is not in the rig") {
        f.pieces.push_back({"no_such_node", 1, {1, 1, 1}});
        body::DefineResult r = f.make();
        CHECK_FALSE(r.def);
        CHECK(r.error.find("no_such_node") != std::string::npos);
    }
    SUBCASE("a rig that fails the Animator's check") {
        body::DefineResult r = body::define("bad", "name = \"x\"\n[[node]]\nname = \"a\"\nbogus = 1\n", {}, {});
        CHECK_FALSE(r.def);
        CHECK(r.error.find("rig") != std::string::npos);
    }
    SUBCASE("a clip that names a pose value the rig does not read") {
        const char* rig = "name = \"tiny\"\n[[node]]\nname = \"root\"\nsteps = [[\"translate\", 0, 0, 0]]\n"
                          "[[node]]\nname = \"p\"\nparent = \"root\"\nmodel = \"box 2 2 2\"\nsteps = [[\"rot_x\", \"head.rot.x\"]]\n";
        const char* clip = "duration = 100\n[[track]]\npart = \"tail\"\nchannel = \"rot\"\nmode = \"add\"\nkeys = [ { t = 0, v = [1, 0, 0] }, { t = 1, v = [2, 0, 0] } ]\n";
        body::DefineResult r = body::define("tiny", rig, {{"wag", clip}}, {{"p", 7, {2, 2, 2}}});
        CHECK_FALSE(r.def);
        CHECK(r.error.find("wag") != std::string::npos);
        CHECK(r.error.find("tail") != std::string::npos);
        body::DefineResult ok = body::define("tiny", rig, {{"ok", kTurn}}, {{"p", 7, {2, 2, 2}}});
        INFO(ok.error);
        CHECK(ok.def);
    }
    SUBCASE("a bad or repeated clip name") {
        CHECK_FALSE(body::define("d", f.rig, {{"Bad Name", kTurn}}, f.pieces).def);
        CHECK_FALSE(body::define("d", f.rig, {{"a", kTurn}, {"a", kTurn}}, f.pieces).def);
    }
    SUBCASE("a clip that fails the clip validator") {
        body::DefineResult r = body::define("d", f.rig, {{"broken", "duration = 100\n[[track]]\npart = \"nose\"\n"}}, f.pieces);
        CHECK_FALSE(r.def);
        CHECK(r.error.find("broken") != std::string::npos);
    }
}

TEST_CASE("creature body: bite at 0.5 s gives the Animator's matrices") {
    Fx f;
    body::DefineResult r = f.make();
    REQUIRE(r.def);
    body::Instance in;
    in.def = r.def;
    REQUIRE(in.play(r.def->clip_index("bite"), 0, false, 0));
    in.advance(850, nullptr);
    kanimator::Solved got = in.solve();
    // the Animator's own path, on the same rig and clip text
    kanimator::RigLoad rl = kanimator::parse_rig(f.rig, "dragon.rig.toml");
    anim::ClipLoad cl = anim::parse_clip(f.bite, "bite.anim.toml");
    REQUIRE((rl.ok && cl.ok));
    anim::Timing at;
    at.elapsed_ms = 850;
    at.duration_ms = 1700;
    at.run_ms = 1700;
    kanimator::Solved want = kanimator::solve(rl.rig, anim::Pose{}, cl.clip, at);
    REQUIRE(got.world.size() == want.world.size());
    for (size_t n = 0; n < want.world.size(); ++n) {
        for (int k = 0; k < 3; ++k) {
            CHECK(std::fabs(got.world[n].t[k] - want.world[n].t[k]) <= 1e-4f);
            for (int c = 0; c < 3; ++c) CHECK(std::fabs(got.world[n].r[k][c] - want.world[n].r[k][c]) <= 1e-4f);
        }
    }
    int head = rl.rig.find("head");
    REQUIRE(head >= 0);
    CHECK(std::fabs(got.world[head].t[0] - (-8.207536f)) <= 1e-3f);   // the golden of test_rig_library
}

TEST_CASE("creature body: cross-fade, hold and loop") {
    Fx f;
    body::DefineResult r = f.make();
    REQUIRE(r.def);
    const int turn = r.def->clip_index("turn"), rest = r.def->clip_index("rest");
    body::Instance in;
    in.def = r.def;
    const int head = static_cast<int>(anim::Part::Head);
    in.play(turn, 0, false, 0);
    in.advance(1000, nullptr);
    CHECK(in.pose().parts[head].rot[0] == doctest::Approx(40.0f).epsilon(1e-4));
    in.advance(5000, nullptr);   // held at the end
    CHECK(in.pose().parts[head].rot[0] == doctest::Approx(40.0f).epsilon(1e-4));
    CHECK(in.time_ms() == doctest::Approx(1000.0f));
    // fade to rest over 200 ms: at once still the turn's pose, then halfway, then the rest's
    in.play(rest, 0, true, 200);
    CHECK(in.pose().parts[head].rot[0] == doctest::Approx(40.0f).epsilon(1e-4));
    in.advance(100, nullptr);
    CHECK(in.pose().parts[head].rot[0] == doctest::Approx(20.0f).epsilon(1e-3));
    in.advance(100, nullptr);
    CHECK(in.pose().parts[head].rot[0] == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(in.fade_left_ms() == 0);
    // the loop wraps
    in.advance(400, nullptr);
    CHECK(in.time_ms() == doctest::Approx(100.0f));   // 200 + 400 = 600, one lap of 500 less
}

TEST_CASE("creature body: a marker is reported once, a loop's every lap") {
    Fx f;
    body::DefineResult r = f.make();
    REQUIRE(r.def);
    body::Instance in;
    in.def = r.def;
    std::vector<body::MarkerHit> hits;
    in.play(r.def->clip_index("turn"), 0, false, 0);
    in.advance(400, &hits);
    CHECK(hits.empty());
    in.advance(200, &hits);   // passes 500
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].name == "hit");
    in.advance(200, &hits);
    CHECK(hits.size() == 1);
    hits.clear();
    in.play(r.def->clip_index("rest"), 0, true, 0);   // tick at 125 of 500
    in.advance(1100, &hits);                          // laps: 125, 625, 1125 is past 1100
    CHECK(hits.size() == 2);
}

TEST_CASE("creature body: a head offset layers on the clip and a group weight scales it") {
    Fx f;
    body::DefineResult r = f.make();
    REQUIRE(r.def);
    body::Instance in;
    in.def = r.def;
    const int head = static_cast<int>(anim::Part::Head);
    in.play(r.def->clip_index("rest"), 0, true, 0);
    const kanimator::Solved base = in.solve();
    const float off[3] = {0, 0, 30};
    in.set_offset(anim::Part::Head, anim::Channel::Rot, off, 2);
    CHECK(in.pose().parts[head].rot[2] == doctest::Approx(30.0f));
    in.set_group_weight(2, 0.5f);
    CHECK(in.pose().parts[head].rot[2] == doctest::Approx(15.0f));
    in.set_group_weight(2, 1.0f);
    kanimator::Solved turned = in.solve();
    const int hn = r.def->rig.find("head");
    REQUIRE(hn >= 0);
    bool moved = false;
    for (int k = 0; k < 3; ++k)
        for (int c = 0; c < 3; ++c) moved = moved || std::fabs(turned.world[hn].r[k][c] - base.world[hn].r[k][c]) > 1e-3f;
    CHECK(moved);
    // the ground node is as it was
    const int ground = r.def->rig.find("ground");
    REQUIRE(ground >= 0);
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(turned.world[ground].t[k] - base.world[ground].t[k]) <= 1e-4f);
    in.clear_offsets();
    CHECK(in.pose().parts[head].rot[2] == doctest::Approx(0.0f));
}

TEST_CASE("creature body: piece matrices and posed points") {
    // one piece 4x4x4 whose node sits 10 voxels up; scale 0.5 blocks a voxel; body at (100, 200, 5) turned 90 degrees
    const char* rig = "name = \"one\"\n[[node]]\nname = \"root\"\nsteps = [[\"translate\", 0, 0, 0]]\n"
                      "[[node]]\nname = \"p\"\nparent = \"root\"\nmodel = \"box 4 4 4\"\nsteps = [[\"translate\", 0, 10, 10], [\"rot_x\", \"head.rot.x\"]]\n";
    body::DefineResult r = body::define("one", rig, {{"rest", kRest}}, {{"p", 9, {4, 4, 4}}});
    INFO(r.error);
    REQUIRE(r.def);
    body::Instance in;
    in.def = r.def;
    in.scale = 0.5f;
    in.play(0, 0, true, 0);
    kanimator::Solved s = in.solve();
    const float pos[3] = {100, 200, 5};
    const float yaw = 1.5707963f;
    float m[16];
    body::piece_matrix(*r.def, s, 0, pos, yaw, in.scale, m);
    // the matrix is the node turned by yaw and scaled; its translation is the model's corner: node (0,10,10) minus (2,2,2) -> (-2,8,8)
    // turned 90 deg about z: (x,y) -> (-y, x) = (-8, -2); times 0.5 = (-4, -1); z 8 * 0.5 = 4
    CHECK(m[12] == doctest::Approx(96.0f).epsilon(1e-4));
    CHECK(m[13] == doctest::Approx(199.0f).epsilon(1e-4));
    CHECK(m[14] == doctest::Approx(9.0f).epsilon(1e-4));
    CHECK(m[0] == doctest::Approx(0.0f).epsilon(1e-3));   // x axis -> +y, times 0.5
    CHECK(m[1] == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(m[10] == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(m[15] == 1.0f);
    // the point at the model's centre is the node's place
    const float centre[3] = {2, 2, 2};
    float w[3];
    body::piece_point(*r.def, s, 0, centre, pos, yaw, in.scale, w);
    CHECK(w[0] == doctest::Approx(100.0f - 5.0f).epsilon(1e-4));   // node (0,10,10) -> (-10, 0) * 0.5 = (-5, 0)
    CHECK(w[1] == doctest::Approx(200.0f).epsilon(1e-4));
    CHECK(w[2] == doctest::Approx(10.0f).epsilon(1e-4));
}
