// Parity test of the shared rig library (openspec add-dragon-boss task 3.1): the dragon's `bite` clip at 0.5 s of its length (850 ms
// of 1700) on the dragon rig gives the node matrices below, to 1e-4. The Animator, kanim and (task 3.3) the loader link the same
// library (kyubu_rig), so these are the numbers the game must show. The fixtures are copies of the files in
// evidence/dragon-black-purple-2026-11-05; the matrices were taken from the Animator's solver on 2026-11-06.
#include "doctest.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

#include "animation_eval.h"
#include "animation_load.h"
#include "rig.h"

using namespace kanimator;
namespace anim = kyubu::game::anim;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "cannot read " << path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Golden {
    const char* node;
    float m[12];   // r row-major (9), then t (3)
};

const Golden kGolden[] = {
    {"neck2", {0.958777f, 0.002952f, -0.284143f, 0.283916f, 0.031303f, 0.958338f, 0.011724f, -0.999506f, 0.029174f, -6.230347f, 40.523224f, 25.920870f}},
    {"head", {0.861677f, -0.357879f, -0.359772f, 0.410499f, 0.908383f, 0.079567f, 0.298335f, -0.216247f, 0.929642f, -8.207536f, 47.356800f, 22.127066f}},
    {"jaw", {0.861677f, -0.357879f, -0.359772f, 0.410499f, 0.908383f, 0.079567f, 0.298335f, -0.216247f, 0.929642f, -10.535642f, 52.432476f, 21.867352f}},
    {"tail5", {0.771586f, -0.634365f, -0.047292f, 0.634914f, 0.763402f, 0.118751f, -0.039228f, -0.121653f, 0.991797f, 25.322876f, -108.660225f, 31.638073f}},
    {"wing_l_in", {0.062230f, -0.994571f, -0.083401f, 0.939718f, 0.086539f, -0.330820f, 0.336241f, -0.057786f, 0.940001f, -14.446623f, 9.190548f, 38.629780f}},
};

}  // namespace

TEST_CASE("rig library: the dragon's bite at 0.5 s gives the matrices of the Animator") {
    const std::string dir = KYUBU_SOURCE_DIR "/tests/fixtures/rig/";
    RigLoad rl = parse_rig(slurp(dir + "dragon.rig.toml"), "dragon.rig.toml");
    for (const anim::ClipError& e : rl.errors) MESSAGE(anim::format_error(e));
    REQUIRE(rl.ok);
    anim::ClipLoad cl = anim::parse_clip(slurp(dir + "bite.anim.toml"), "bite.anim.toml");
    for (const anim::ClipError& e : cl.errors) MESSAGE(anim::format_error(e));
    REQUIRE(cl.ok);

    anim::Timing at;
    at.elapsed_ms = 850;
    at.duration_ms = 1700;
    at.run_ms = 1700;
    Solved s = solve(rl.rig, Pose{}, cl.clip, at);
    REQUIRE(s.applied.ok);
    CHECK(s.missing_parts == 0);

    for (const Golden& g : kGolden) {
        int i = rl.rig.find(g.node);
        REQUIRE_MESSAGE(i >= 0, g.node);
        const Affine& a = s.world[i];
        const float got[12] = {a.r[0][0], a.r[0][1], a.r[0][2], a.r[1][0], a.r[1][1], a.r[1][2],
                               a.r[2][0], a.r[2][1], a.r[2][2], a.t[0], a.t[1], a.t[2]};
        for (int k = 0; k < 12; ++k) CHECK_MESSAGE(std::fabs(got[k] - g.m[k]) <= 1e-4f, g.node << " value " << k << ": " << got[k] << " vs " << g.m[k]);
    }

    // The same pose through solve_pose (the path the game takes each frame) gives the same matrices.
    Solved again = solve_pose(rl.rig, s.pose);
    REQUIRE(again.world.size() == s.world.size());
    for (size_t n = 0; n < s.world.size(); ++n)
        for (int r = 0; r < 3; ++r) {
            CHECK(std::fabs(again.world[n].t[r] - s.world[n].t[r]) <= 1e-4f);
            for (int c = 0; c < 3; ++c) CHECK(std::fabs(again.world[n].r[r][c] - s.world[n].r[r][c]) <= 1e-4f);
        }
}
