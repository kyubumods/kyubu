// Unit tests of the Animator's effects (tools/animator/effects.h), the rig's `[[shockwave]]` and `[[pillars]]` tables, and the emitter
// keys that go with them (`at`, `for_ms`, `cone_min`, `offset`): what is read, one bad table per message, an emitter thrown only
// from a marker, ways between two cones, particles rising from a ring round the emitter, a ring started at a marker and aged as
// EMTweaks ages it, a loop's wrap, a pillar's rise, hold and sink, the wave, the cleared box, a pillar's stacked stones on its
// voxel grid and its rubble, the ring's mesh, and a sheet. No game.
#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "../../tools/animator/emtweaks/src/shockwave.hpp"
#include "animation_eval.h"
#include "animation_load.h"
#include "effects.h"
#include "particles.h"
#include "rig.h"
#include "sheet.h"

using namespace kanimator;
namespace anim = kyubu::game::anim;

namespace {

// A ground node and a head 10 up; 2 voxels a block.
const char* kNodes = "name = \"t\"\n[[node]]\nname = \"root\"\n[[node]]\nname = \"head\"\nparent = \"root\"\nmodel = \"box 4 4 4\"\n"
                     "steps = [[\"translate\", 0, 0, 10]]\n";
const char* kWorld = "[particles]\nblock = 2\ngravity = 30\n";

Rig good_rig(const std::string& text) {
    RigLoad r = parse_rig(text, "t.rig.toml");
    for (const ClipError& e : r.errors) MESSAGE(anim::format_error(e));
    REQUIRE(r.ok);
    return r.rig;
}

std::string first_code(const std::string& text) {
    RigLoad r = parse_rig(text, "t.rig.toml");
    return r.errors.empty() ? std::string() : r.errors.front().code;
}

// A clip of 1 s with no motion and markers [(t 0..1, name)].
anim::Clip marker_clip(const std::vector<std::pair<float, std::string>>& marks, bool loop = false) {
    std::string text = std::string("duration = 1000\nend = \"") + (loop ? "loop" : "hold") + "\"\nblend_in = 0\nblend_out = 0\n"
                       "[[track]]\npart = \"tail\"\nchannel = \"pos\"\nmode = \"add\"\nkeys = [{ t = 0, v = [0, 0, 0] }, { t = 1, v = [0, 0, 0] }]\n";
    for (const auto& m : marks) text += "[[marker]]\nt = " + std::to_string(m.first) + "\nname = \"" + m.second + "\"\n";
    anim::ClipLoad c = anim::parse_clip(text, "c.anim.toml");
    for (const ClipError& e : c.errors) MESSAGE(anim::format_error(e));
    REQUIRE(c.ok);
    return c.clip;
}

const char* kRing = "[[shockwave]]\nname = \"slam\"\nparent = \"root\"\nsteps = [[\"rot_x\", 90]]\nat = \"hit\"\ninner_radius = 1\n"
                    "thickness = 0.5\nexpansion = 0.25\nlife_ticks = 40\nfade_on_tick = 10\nfade_speed = 0.05\ncolor = [1, 0.5, 0, 0.8]\n";

std::string pillars(const std::string& more) {
    return std::string("[[pillars]]\nname = \"p\"\nparent = \"root\"\nsteps = [[\"rot_x\", 90]]\nat = \"hit\"\ncount = 12\nradius = [2, 6]\n") + more;
}

float dist2d(const float a[3]) { return std::sqrt(a[0] * a[0] + a[1] * a[1]); }

// A pillar of the first set standing on the figure's ground at its origin, all out: its up is +z, leaned `lean` degrees towards -y.
LivePillar upright(float width, float height, float lean = 0) {
    LivePillar q;
    q.width = width;
    q.height = height;
    q.lean = lean;
    q.out = 1;
    const float l = lean * 3.14159265f / 180;
    const float axes[3][3] = {{1, 0, 0}, {0, -std::sin(l), std::cos(l)}, {0, -std::cos(l), -std::sin(l)}};
    const float flat[3][3] = {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
    std::copy(&axes[0][0], &axes[0][0] + 9, &q.axes[0][0]);
    std::copy(&flat[0][0], &flat[0][0] + 9, &q.flat[0][0]);
    return q;
}

// The first `count` vertices of both meshes are the same.
bool same_mesh(const Mesh& a, const Mesh& b, size_t count) {
    if (a.tris.size() < count || b.tris.size() < count) return false;
    for (size_t i = 0; i < count; ++i)
        if (a.tris[i].x != b.tris[i].x || a.tris[i].y != b.tris[i].y || a.tris[i].z != b.tris[i].z || a.tris[i].color != b.tris[i].color)
            return false;
    return true;
}

}  // namespace

TEST_CASE("a shockwave's and a set of pillars' fields are read") {
    const Rig rig = good_rig(std::string(kNodes) + kWorld + kRing + "pixel_size = -1\n" +
                             pillars("clear = [-1, -2, 1, 0.5]\nwave = 20\ndelay = [0, 50]\nwidth = [0.5, 1]\nheight = [1, 3]\ntaper = 0.5\n"
                                     "cell = 0.5\nlean = 20\nturn = 45\nrise_ms = 100\nhold_ms = [500, 900]\nsink_ms = 300\n"
                                     "color = [0.5, 0.4, 0.3]\ncolor_random = [0.6, 0.5, 0.4]\ncolor_top = [0.3, 0.6, 0.2]\nseed = 9\n"
                                     "stones = [1, 3]\nrubble = [0, 2]\n"));
    REQUIRE(rig.shockwaves.size() == 1);
    const Shockwave& w = rig.shockwaves[0];
    CHECK(w.name == "slam");
    CHECK(w.parent == rig.find("root"));
    CHECK(w.at == "hit");
    CHECK(w.inner_radius == 1);
    CHECK(w.thickness == doctest::Approx(0.5));
    CHECK(w.expansion == doctest::Approx(0.25));
    CHECK(w.life_ticks == 40);
    CHECK(w.fade_on_tick == 10);
    CHECK(w.fade_speed == doctest::Approx(0.05));
    CHECK(w.color[3] == doctest::Approx(0.8));
    CHECK(w.pixel_size == -1);
    REQUIRE(rig.pillars.size() == 1);
    const Pillars& p = rig.pillars[0];
    CHECK(p.count == 12);
    CHECK(p.radius.lo == 2);
    CHECK(p.radius.hi == 6);
    CHECK(p.has_clear);
    CHECK(p.clear[3] == doctest::Approx(0.5));
    CHECK(p.wave == 20);
    CHECK(p.delay.hi == 50);
    CHECK(p.width.lo == doctest::Approx(0.5));
    CHECK(p.height.hi == 3);
    CHECK(p.taper == doctest::Approx(0.5));
    CHECK(p.cell == doctest::Approx(0.5));
    CHECK(p.stones.lo == 1);
    CHECK(p.stones.hi == 3);
    CHECK(p.rubble.hi == 2);
    CHECK(p.lean == 20);
    CHECK(p.turn == 45);
    CHECK(p.rise_ms == 100);
    CHECK(p.hold_ms.lo == 500);
    CHECK(p.sink_ms == 300);
    CHECK(p.has_color_random);
    CHECK(p.has_color_top);
    CHECK(p.color_top[1] == doctest::Approx(0.6));
    CHECK(p.seed == 9);
    CHECK(rig.has_effects());
}

TEST_CASE("the defaults of a shockwave and a set of pillars") {
    const Rig rig = good_rig(std::string(kNodes) + kWorld +
                             "[[shockwave]]\nname = \"s\"\nparent = \"root\"\nat = \"hit\"\n" + pillars(""));
    const Shockwave& w = rig.shockwaves[0];
    CHECK(w.inner_radius == 0);
    CHECK(w.life_ticks == 40);
    CHECK(w.pixel_size == 0);
    CHECK(w.color[3] == 1);
    const Pillars& p = rig.pillars[0];
    CHECK_FALSE(p.has_clear);
    CHECK(p.wave == 0);
    CHECK(p.taper == 1);
    CHECK(p.cell == doctest::Approx(0.25));
    CHECK(p.stones.lo == 2);
    CHECK(p.stones.hi == 4);
    CHECK(p.rubble.lo == 0);
    CHECK(p.rubble.hi == 3);
    CHECK_FALSE(p.has_color_top);
}

TEST_CASE("a bad shockwave or set of pillars is refused with its reason") {
    const std::string head = std::string(kNodes) + kWorld;
    const std::string ring = "[[shockwave]]\nname = \"s\"\nparent = \"root\"\nat = \"hit\"\n";
    CHECK(first_code(std::string(kNodes) + ring) == "missing_field");                                    // no [particles]
    CHECK(first_code(head + "[[shockwave]]\nname = \"s\"\nparent = \"root\"\n") == "missing_field");       // no at
    CHECK(first_code(head + ring + "colour = [1, 1, 1, 1]\n") == "unknown_key");
    CHECK(first_code(head + ring + "color = [1, 1, 1]\n") == "bad_value");                               // the alpha is needed
    CHECK(first_code(head + ring + "life_ticks = 2.5\n") == "bad_type");
    CHECK(first_code(head + ring + "life_ticks = 7000\n") == "out_of_range");
    CHECK(first_code(head + ring + "thickness = 0\n") == "out_of_range");
    CHECK(first_code(head + ring + ring) == "duplicate_node");
    CHECK(first_code(head + "[[shockwave]]\nname = \"s\"\nparent = \"tail\"\nat = \"hit\"\n") == "unknown_parent");
    CHECK(first_code(head + ring + "at = \"\"\n") != "");
    CHECK(first_code(head + "[[pillars]]\nname = \"p\"\nparent = \"root\"\nat = \"hit\"\nradius = 3\n") == "missing_field");   // count
    CHECK(first_code(head + "[[pillars]]\nname = \"p\"\nparent = \"root\"\nat = \"hit\"\ncount = 3\n") == "missing_field");    // radius
    CHECK(first_code(head + pillars("") + "count = 300\n") != "");
    CHECK(first_code(head + "[[pillars]]\nname = \"p\"\nparent = \"root\"\nat = \"hit\"\ncount = 300\nradius = 3\n") == "out_of_range");
    CHECK(first_code(head + pillars("width = 0\n")) == "out_of_range");
    CHECK(first_code(head + pillars("clear = [1, 0, 0, 1]\n")) == "bad_value");                         // most x below least x
    CHECK(first_code(head + pillars("clear = [1, 0, 2]\n")) == "bad_value");
    CHECK(first_code(head + pillars("lean = 70\n")) == "out_of_range");
    CHECK(first_code(head + pillars("taper = 2\n")) == "out_of_range");
    CHECK(first_code(head + pillars("stones = 0\n")) == "out_of_range");
    CHECK(first_code(head + pillars("rubble = [2, 9]\n")) == "out_of_range");
}

TEST_CASE("an emitter's marker keys, inner cone and offset are read and checked") {
    const std::string head = std::string(kNodes) + kWorld + "[[emitter]]\nname = \"e\"\nparent = \"head\"\nrate = 10\nlife = 300\n";
    const Rig rig = good_rig(head + "at = \"hit\"\nfor_ms = 80\ncone = 60\ncone_min = 20\noffset = [1, 2]\n");
    const Emitter& e = rig.emitters[0];
    CHECK(e.at == "hit");
    CHECK(e.for_ms == 80);
    CHECK(e.cone_min == 20);
    CHECK(e.offset.lo == 1);
    CHECK(e.offset.hi == 2);
    const Rig plain = good_rig(head);
    CHECK(plain.emitters[0].at.empty());
    CHECK(plain.emitters[0].for_ms == 100);
    CHECK(plain.emitters[0].offset.hi == 0);
    CHECK(first_code(head + "for_ms = 80\n") == "missing_field");          // for_ms needs at
    CHECK(first_code(head + "cone = 30\ncone_min = 40\n") == "bad_value"); // past the cone
    CHECK(first_code(head + "offset = [2, 1]\n") == "bad_value");
}

TEST_CASE("an emitter with `at` throws only from its markers, for `for_ms`") {
    const Rig rig = good_rig(std::string(kNodes) + kWorld +
                             "[[emitter]]\nname = \"e\"\nparent = \"head\"\nrate = 1000\nstep_ms = 10\nlife = 100\nat = \"hit\"\nfor_ms = 50\n");
    const ParticlePlan plan = plan_particles(rig, marker_clip({{0.3f, "hit"}, {0.8f, "hit"}, {0.5f, "other"}}), 1000, {});
    const EmitterPlan& ep = plan.emitters[0];
    uint32_t total = 0;
    for (size_t k = 0; k < ep.count.size(); ++k) {
        const float ms = static_cast<float>(k) * 10;
        const bool on = (ms >= 300 - 1e-3f && ms < 350 - 1e-3f) || (ms >= 800 - 1e-3f && ms < 850 - 1e-3f);
        if (!on) CHECK(ep.count[k] == 0);
        total += ep.count[k];
    }
    CHECK(total >= 99);    // 2 x 5 bursts of 10
    CHECK(total <= 101);
    // no such marker: nothing
    const ParticlePlan none = plan_particles(rig, marker_clip({{0.3f, "other"}}), 1000, {});
    for (uint32_t c : none.emitters[0].count) CHECK(c == 0);
    // a loop's window runs past its end into the lap's start
    const ParticlePlan wrap = plan_particles(rig, marker_clip({{0.98f, "hit"}}, true), 1000, {});
    CHECK(wrap.emitters[0].count[1] > 0);   // 10 ms is 30 ms after the marker
}

TEST_CASE("the ways lie between cone_min and cone, and offset starts them on a ring round the emitter") {
    const Rig rig = good_rig(std::string(kNodes) + kWorld +
                             "[[emitter]]\nname = \"e\"\nparent = \"head\"\nsteps = [[\"rot_x\", 90]]\nrate = 2000\nstep_ms = 10\n"
                             "life = 1000\nspeed = 5\ncone = 70\ncone_min = 40\noffset = [1, 3]\ngravity = false\n");
    const ParticlePlan plan = plan_particles(rig, marker_clip({}), 1000, {});
    // the first burst's particles, 0.5 s old: start on the ring of 2 to 6 voxels round (0, 0, 10), out by 5 blocks a second
    const std::vector<LiveParticle> live = live_particles(rig, plan, 500, false);
    REQUIRE(live.size() > 20);
    size_t checked = 0;
    for (size_t i = 0; i < 20; ++i) {
        const LiveParticle& p = live[i];
        // up (+y of the frame) is the figure's +z; the flight is 5 blocks a second x 2 voxels x 0.5 s = 5 voxels along the way
        const float rise = p.pos[2] - 10;
        const float angle = std::acos(std::clamp(rise / 5.0f, -1.0f, 1.0f)) * 180 / 3.14159265f;
        CHECK(angle >= 40 - 0.5f);
        CHECK(angle <= 70 + 0.5f);
        // the start's distance out: what is left after the way's own sideways part
        const float side = 5.0f * std::sin(angle * 3.14159265f / 180);
        const float start = dist2d(p.pos) - side;   // the offset is along the same side as the way leans
        CHECK(start >= 2 - 0.05f);
        CHECK(start <= 6 + 0.05f);
        ++checked;
    }
    CHECK(checked == 20);
}

TEST_CASE("a particle thrown up from the ground with ground = stop flies and lands") {
    // from z 0, straight up at 3 blocks a second (6 voxels), gravity 60 voxels a second squared: back down after 0.2 s
    const Rig rig = good_rig(std::string(kNodes) + kWorld +
                             "[[emitter]]\nname = \"e\"\nparent = \"root\"\nsteps = [[\"rot_x\", 90]]\nrate = 100\nstep_ms = 10\n"
                             "life = 1000\nspeed = 3\nground = \"stop\"\n");
    const ParticlePlan plan = plan_particles(rig, marker_clip({}), 1000, {});
    const LiveParticle mid = live_particles(rig, plan, 100, false)[0];
    CHECK(mid.pos[2] == doctest::Approx(6 * 0.1f - 0.5f * 60 * 0.01f));   // 0.3 voxels up at the top
    const LiveParticle late = live_particles(rig, plan, 500, false)[0];
    CHECK(late.pos[2] == doctest::Approx(0));
}

TEST_CASE("a shockwave starts at its marker and ages as EMTweaks ages it") {
    const Rig rig = good_rig(std::string(kNodes) + kWorld + kRing);
    const EffectPlan plan = plan_effects(rig, marker_clip({{0.25f, "hit"}}), 1000, {});
    REQUIRE(plan.rings.size() == 1);
    REQUIRE(plan.rings[0].size() == 1);
    CHECK(plan.rings[0][0].t_ms == doctest::Approx(250));
    CHECK(live_effects(rig, plan, 200, false).rings.empty());   // not yet
    const LiveEffects at = live_effects(rig, plan, 250 + 300, false);   // 15 ticks old
    REQUIRE(at.rings.size() == 1);
    const LiveRing& r = at.rings[0];
    CHECK(r.age_ticks == doctest::Approx(15));
    CHECK(r.inner == doctest::Approx(1 + 0.25f * 15));
    CHECK(r.outer == doctest::Approx(1.5f + 0.25f * 15));
    CHECK(r.alpha == doctest::Approx(0.8f * (1 - 0.05f * 5)));
    CHECK(r.facing[2] == doctest::Approx(1));   // rot_x 90: the frame's +y is up
    CHECK(live_effects(rig, plan, 250 + 40 * 20 + 1, false).rings.empty());   // past its life
    // a loop: a marker late in the lap is still alive early in the next one
    const EffectPlan loop = plan_effects(rig, marker_clip({{0.9f, "hit"}}, true), 1000, {});
    CHECK(live_effects(rig, loop, 100, true).rings.size() == 1);
    CHECK(live_effects(rig, loop, 100, false).rings.empty());
}

TEST_CASE("a ring's mesh is EMTweaks' cells, see-through, round its centre") {
    const Rig rig = good_rig(std::string(kNodes) + kWorld + kRing);
    const EffectPlan plan = plan_effects(rig, marker_clip({{0.0f, "hit"}}), 1000, {});
    const LiveEffects fx = live_effects(rig, plan, 200, false);   // 10 ticks: radii 3.5 to 4 blocks, alpha 0.8
    REQUIRE(fx.rings.size() == 1);
    const Mesh m = ring_mesh(rig, fx.rings);
    REQUIRE(!m.empty());
    // the same cells as EMTweaks builds for the ring, in voxels
    shockwave::Params p;
    p.inner_radius = 1;
    p.thickness = 0.5f;
    p.expansion = 0.25f;
    p.life_ticks = 40;
    p.fade_on_tick = 10;
    p.fade_speed = 0.05f;
    p.color[0] = 1;
    p.color[1] = 0.5f;
    p.color[2] = 0;
    p.color[3] = 0.8f;
    p.pixel_size = kDefaultRingPixel;
    std::vector<shockwave::Vertex> cells;
    REQUIRE(shockwave::build_pixel_ring(p, 10, cells));
    CHECK(m.tris.size() == cells.size());
    for (const MeshVertex& v : m.tris) {
        CHECK(v.z == doctest::Approx(0));
        CHECK(std::sqrt(v.x * v.x + v.y * v.y) <= (4 + kDefaultRingPixel * 1.5f) * 2);
        CHECK((v.color >> 24) == 204);   // 0.8 of 255
    }
}

TEST_CASE("a pillar rises fast, holds, and sinks") {
    CHECK(pillar_out(100, 500, 200, -1) == 0);
    CHECK(pillar_out(100, 500, 200, 0) == doctest::Approx(0));
    float peak = 0;
    for (float t = 0; t <= 100; t += 1) peak = std::max(peak, pillar_out(100, 500, 200, t));
    CHECK(peak > 1.03f);
    CHECK(peak < 1.07f);
    CHECK(pillar_out(100, 500, 200, 30) > 0.6f);   // most of the way out in its first third
    CHECK(pillar_out(100, 500, 200, 100) == doctest::Approx(1));
    CHECK(pillar_out(100, 500, 200, 400) == 1);
    CHECK(pillar_out(100, 500, 200, 700) == doctest::Approx(0.5));   // half way down, half way in
    CHECK(pillar_out(100, 500, 200, 801) == 0);
}

TEST_CASE("a set of pillars stands round its centre, rises with the wave, keeps out of the cleared box") {
    const Rig rig = good_rig(std::string(kNodes) + kWorld + pillars("wave = 10\nrise_ms = 50\nhold_ms = 2000\nclear = [-10, -10, 10, 0]\n"));
    const EffectPlan plan = plan_effects(rig, marker_clip({{0.0f, "hit"}}), 1000, {});
    // all out at 0.8 s: the farthest (6 blocks) started at 0.6 s
    const LiveEffects all = live_effects(rig, plan, 800, false);
    CHECK(all.pillars.size() == 12);
    for (const LivePillar& q : all.pillars) {
        const float r = dist2d(q.foot) / 2;   // blocks
        CHECK(r >= 2 - 1e-3f);
        CHECK(r <= 6 + 1e-3f);
        CHECK(q.foot[1] > 0);                 // y < 0 is cleared
        CHECK(q.foot[2] == doctest::Approx(0));
        CHECK(q.axes[1][2] == doctest::Approx(1));   // no lean: straight up
        CHECK(q.flat[1][2] == doctest::Approx(1));
    }
    // at 0.4 s only those within 4 blocks have started (the wave is 10 blocks a second)
    for (const LivePillar& q : live_effects(rig, plan, 400, false).pillars) CHECK(dist2d(q.foot) / 2 <= 4 + 1e-3f);
    CHECK(live_effects(rig, plan, 400, false).pillars.size() < 12);
    // the same pillars whatever was asked before
    const LiveEffects again = live_effects(rig, plan_effects(rig, marker_clip({{0.0f, "hit"}}), 1000, {}), 800, false);
    REQUIRE(again.pillars.size() == all.pillars.size());
    for (size_t i = 0; i < all.pillars.size(); ++i) CHECK(again.pillars[i].foot[0] == all.pillars[i].foot[0]);
}

TEST_CASE("a pillar's mesh: stacked stones on its voxel grid, as tall as its height, in shades of its colour") {
    const Rig rig = good_rig(std::string(kNodes) + kWorld +
                             pillars("cell = 0.25\nstones = [3, 3]\nrubble = [0, 0]\ntaper = 0.6\ncolor = [0.8, 0.4, 0.2]\n"));
    LivePillar q = upright(1.5f, 3);
    std::copy(rig.pillars[0].color, rig.pillars[0].color + 3, q.rgb);   // as live_effects gives it
    const Mesh m = pillar_mesh(rig, {q});
    REQUIRE_FALSE(m.empty());
    Vec3 lo, hi;
    REQUIRE(mesh_bounds(m, lo, hi));
    const float cell = 0.25f * 2;                // voxels: 2 a block
    CHECK(std::fabs(hi.z - 6) <= cell / 2 + 1e-3f);   // 3 blocks up, to the nearest voxel
    CHECK(lo.z == doctest::Approx(0));           // on the ground, nothing under it
    // its grid: 6 voxels across its foot and 4 of room on each side for the stones' shifts and leans, centred on its axis
    const float half = 7 * cell;
    CHECK(hi.x <= half + 1e-3f);
    CHECK(lo.x >= -half - 1e-3f);
    CHECK(hi.y <= half + 1e-3f);
    CHECK(lo.y >= -half - 1e-3f);
    std::set<uint32_t> colors;
    size_t off_grid = 0, not_red = 0;
    for (const MeshVertex& v : m.tris) {
        const float g[3] = {(v.x + half) / cell, (v.y + half) / cell, v.z / cell};
        for (float a : g) off_grid += std::fabs(a - std::round(a)) > 1e-3f;
        colors.insert(v.color);
        const uint32_t R = (v.color >> 16) & 0xFF, G = (v.color >> 8) & 0xFF, B = v.color & 0xFF;
        not_red += !(R >= G && G >= B);
    }
    CHECK(off_grid == 0);                        // every corner on the voxel grid
    CHECK(not_red == 0);                         // its stones, seams, rims and cracks are all shades of red-brown
    CHECK(colors.size() > 20);                   // and not one flat colour
    CHECK(m.triangle_count() > 200);             // nor one box: stones, facets, seams
    // the same pillar every time; another number is another pillar
    const Mesh again = pillar_mesh(rig, {q});
    CHECK(again.tris.size() == m.tris.size());
    CHECK(same_mesh(again, m, m.tris.size()));
    LivePillar other = q;
    other.number = 7;
    const Mesh m7 = pillar_mesh(rig, {other});
    CHECK_FALSE((m7.tris.size() == m.tris.size() && same_mesh(m7, m, m.tris.size())));
}

TEST_CASE("a pillar slides up through the ground, and its rubble lies flat round its foot") {
    const std::string keys = "cell = 0.25\nstones = [2, 2]\ncolor = [0.8, 0.4, 0.2]\n";
    const Rig bare = good_rig(std::string(kNodes) + kWorld + pillars(keys + "rubble = [0, 0]\n"));
    const Rig rubble = good_rig(std::string(kNodes) + kWorld + pillars(keys + "rubble = [4, 4]\n"));
    LivePillar q = upright(1.5f, 3);
    Vec3 lo, hi;
    // half out: its top half, cut at the ground
    q.out = 0.5f;
    REQUIRE(mesh_bounds(pillar_mesh(bare, {q}), lo, hi));
    CHECK(std::fabs(hi.z - 3) <= 0.25f + 1e-3f);
    CHECK(lo.z == doctest::Approx(0));
    // a little past its height just after it rises: its lowest row still reaches the ground
    q.out = 1.05f;
    REQUIRE(mesh_bounds(pillar_mesh(bare, {q}), lo, hi));
    CHECK(hi.z > 6);
    CHECK(lo.z == doctest::Approx(0));
    q.out = 0;
    CHECK(pillar_mesh(bare, {q}).empty());
    // leaned 30 degrees: the stones lean, the rubble (drawn after them, the same stones before it) lies level on the ground, at
    // most 4 voxels of 0.25 blocks high, on its own grid
    q = upright(1.5f, 3, 30);
    const Mesh stack = pillar_mesh(bare, {q}), all = pillar_mesh(rubble, {q});
    REQUIRE(all.tris.size() > stack.tris.size());
    CHECK(same_mesh(all, stack, stack.tris.size()));
    const float cell = 0.5f, side = (6 + 2 * (4 + 6)) * cell;
    size_t off = 0;
    for (size_t i = stack.tris.size(); i < all.tris.size(); ++i) {
        const MeshVertex& v = all.tris[i];
        off += v.z < -1e-3f || v.z > 4 * cell + 1e-3f;
        const float g[2] = {(v.x + side / 2) / cell, (v.y + side / 2) / cell};
        for (float a : g) off += std::fabs(a - std::round(a)) > 1e-3f;
    }
    CHECK(off == 0);
    // the stones leaned: some voxel corner well off the ground leans towards -y past the foot's grid
    REQUIRE(mesh_bounds(stack, lo, hi));
    CHECK(lo.y < -7 * cell);
}

TEST_CASE("a sheet draws the pillars and blends the rings") {
    const Rig rig = good_rig(std::string(kNodes) + kWorld +
                             "[[shockwave]]\nname = \"s\"\nparent = \"root\"\nat = \"hit\"\ninner_radius = 1\n"   // standing: faces the front view
                             "thickness = 3\ncolor = [0, 0, 1, 0.5]\n" +
                             pillars("color = [1, 0, 0]\nheight = 3\nwidth = 1.5\nhold_ms = 5000\n"));
    const anim::Clip clip = marker_clip({{0.1f, "hit"}});
    std::vector<Mesh> meshes = {Mesh{}, mesh_box(4, 4, 4, 0x808080)};
    SheetRequest req;
    req.rig = &rig;
    req.clip = &clip;
    req.meshes = &meshes;
    req.fractions = {0.05f, 0.5f};
    const SheetResult r = make_mimic_sheet(req);
    REQUIRE(r.ok);
    size_t reds = 0, blends = 0;
    for (size_t i = 0; i + 2 < r.image.rgb.size(); i += 3) {
        const uint8_t R = r.image.rgb[i], G = r.image.rgb[i + 1], B = r.image.rgb[i + 2];
        if (R > 120 && G < 60 && B < 60) ++reds;
        if (B > 120 && R < 60 && G < 80 && B < 200) ++blends;   // half blue over the dark background: not full blue
    }
    CHECK(reds > 50);
    CHECK(blends > 50);
}
