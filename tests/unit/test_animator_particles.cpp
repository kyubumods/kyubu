// Unit tests of the Animator's particles (tools/animator/particles.h) and of the rig's `[particles]` and `[[emitter]]` tables: what is
// read, one bad emitter per message, the bursts' counts against the rate and the amount, the flight of a particle (speed, gravity,
// the ground), its life, size and colour, a loop's wrap, the same particles whatever the order of the times asked, the cubes, and a
// sheet with particles. No game.
#include "doctest.h"

#include <cmath>
#include <numeric>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "particles.h"
#include "rig.h"
#include "sheet.h"

using namespace kanimator;
namespace anim = kyubu::game::anim;

namespace {

const char* kNodes = "name = \"t\"\n[[node]]\nname = \"root\"\n[[node]]\nname = \"head\"\nparent = \"root\"\nmodel = \"box 4 4 4\"\n"
                     "steps = [[\"translate\", 0, 0, 10]]\n";

std::string rig_text(const std::string& emitter, bool world = true) {
    return std::string(kNodes) + (world ? "[particles]\nblock = 2\ngravity = 30\n" : "") + "[[emitter]]\n" + emitter;
}

Rig good_rig(const std::string& text) {
    RigLoad r = parse_rig(text, "t.rig.toml");
    for (const ClipError& e : r.errors) MESSAGE(anim::format_error(e));
    REQUIRE(r.ok);
    return r.rig;
}

// The first error's code, or "" for a good rig.
std::string first_code(const std::string& text) {
    RigLoad r = parse_rig(text, "t.rig.toml");
    return r.errors.empty() ? std::string() : r.errors.front().code;
}

// A clip of 1 s whose tail.pos.x is 0 up to `on_ms`, then 1 (hold keys).
anim::Clip switch_clip(float on_ms) {
    const std::string t = std::to_string(on_ms / 1000.0f);
    const std::string text = "duration = 1000\nend = \"hold\"\nblend_in = 0\nblend_out = 0\n[[track]]\npart = \"tail\"\nchannel = \"pos\"\n"
                             "mode = \"add\"\nkeys = [{ t = 0, v = [0, 0, 0] }, { t = " + t + ", v = [1, 0, 0], ease = \"hold\" }, "
                             "{ t = 1, v = [1, 0, 0] }]\n";
    anim::ClipLoad c = anim::parse_clip(text, "c.anim.toml");
    for (const ClipError& e : c.errors) MESSAGE(anim::format_error(e));
    REQUIRE(c.ok);
    return c.clip;
}

uint32_t total(const EmitterPlan& ep) { return std::accumulate(ep.count.begin(), ep.count.end(), 0u); }

}  // namespace

TEST_CASE("an emitter's fields are read") {
    const Rig rig = good_rig(rig_text("name = \"breath\"\nparent = \"head\"\nsteps = [[\"rot_x\", -18]]\namount = \"-tail.pos.x\"\n"
                                      "rate = 240\nstep_ms = 20\nspeed = [30, 40]\ncone = 8\nspreading = 1.5\nlife = [500, 700]\n"
                                      "size = 0.5\nsize_end = 0.1\ncolor = [1, 0.4, 0, 1]\ncolor_random = [1, 1, 0]\n"
                                      "color_end = [0.4, 0.1, 0.05]\ngravity = false\nground = \"stop\"\nspin = 50\nseed = 7\n"));
    REQUIRE(rig.emitters.size() == 1);
    const Emitter& e = rig.emitters[0];
    CHECK(rig.particles.present);
    CHECK(rig.particles.block == 2);
    CHECK(rig.particles.gravity == 30);
    CHECK(e.name == "breath");
    CHECK(e.parent == rig.find("head"));
    CHECK(e.steps.size() == 1);
    CHECK(e.amount.is_ref);
    CHECK(e.amount.negate);
    CHECK(e.amount.axis == 0);
    CHECK(e.rate == 240);
    CHECK(e.step_ms == 20);
    CHECK(e.speed.lo == 30);
    CHECK(e.speed.hi == 40);
    CHECK(e.cone == 8);
    CHECK(e.spreading == doctest::Approx(1.5));
    CHECK(e.life.lo == 500);
    CHECK(e.life.hi == 700);
    CHECK(e.size == doctest::Approx(0.5));
    CHECK(e.size_end == doctest::Approx(0.1));
    CHECK(e.color[1] == doctest::Approx(0.4));
    CHECK(e.has_color_random);
    CHECK(e.has_color_end);
    CHECK_FALSE(e.gravity);
    CHECK(e.ground_stop);
    CHECK(e.spin == 50);
    CHECK(e.seed == 7);
    CHECK((rig.parts_used() & (1u << static_cast<int>(anim::Part::Tail))) != 0);
}

TEST_CASE("an emitter's defaults") {
    const Rig rig = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nrate = 10\nlife = 300\n"));
    const Emitter& e = rig.emitters[0];
    CHECK_FALSE(e.amount.is_ref);
    CHECK(e.amount.value == 1);
    CHECK(e.step_ms == 16);
    CHECK(e.life.lo == 300);
    CHECK(e.life.hi == 300);
    CHECK(e.size_end == e.size);
    CHECK(e.gravity);
    CHECK_FALSE(e.ground_stop);
    CHECK_FALSE(e.has_color_random);
}

TEST_CASE("a bad emitter is refused with its reason") {
    const std::string base = "name = \"e\"\nparent = \"head\"\nrate = 10\nlife = 300\n";
    CHECK(first_code(rig_text(base, false)) == "missing_field");                       // no [particles]
    CHECK(first_code(rig_text(base + "colour = [1, 0, 0]\n")) == "unknown_key");
    CHECK(first_code(rig_text("name = \"e\"\nparent = \"tail\"\nrate = 10\nlife = 300\n")) == "unknown_parent");
    CHECK(first_code(rig_text("name = \"e\"\nparent = \"head\"\nlife = 300\n")) == "missing_field");   // no rate
    CHECK(first_code(rig_text("name = \"e\"\nparent = \"head\"\nrate = 10\n")) == "missing_field");    // no life
    CHECK(first_code(rig_text(base + "amount = \"tail.pos\"\n")) == "bad_value");        // three numbers
    CHECK(first_code(rig_text(base + "amount = \"tail.size.x\"\n")) == "unknown_ref");
    CHECK(first_code(rig_text("name = \"e\"\nparent = \"head\"\nrate = 10\nlife = [700, 500]\n")) == "bad_value");
    CHECK(first_code(rig_text("name = \"e\"\nparent = \"head\"\nrate = 6000\nlife = 300\n")) == "out_of_range");
    CHECK(first_code(rig_text(base + "color = [1, 2, 0]\n")) == "out_of_range");
    CHECK(first_code(rig_text(base + "color = [1, 0]\n")) == "bad_value");
    CHECK(first_code(rig_text(base + "ground = \"bounce\"\n")) == "bad_value");
    CHECK(first_code(rig_text(base + "size = 0\n")) == "out_of_range");
    CHECK(first_code(rig_text(base + "gravity = 1\n")) == "bad_type");
    CHECK(first_code(rig_text(base + "[[emitter]]\n" + base)) == "duplicate_node");
    CHECK(first_code(std::string(kNodes) + "[particles]\ngravity = 30\n") == "missing_field");          // no block
}

TEST_CASE("the bursts throw `rate` particles a second while the amount is on") {
    const Rig rig = good_rig(rig_text("name = \"e\"\nparent = \"head\"\namount = \"tail.pos.x\"\nrate = 100\nstep_ms = 10\nlife = 300\n"));
    const anim::Clip clip = switch_clip(500);
    const ParticlePlan plan = plan_particles(rig, clip, 1000, {});
    REQUIRE(plan.emitters.size() == 1);
    const EmitterPlan& ep = plan.emitters[0];
    REQUIRE(ep.count.size() == 101);   // 0, 10, ..., 1000 ms
    for (size_t k = 0; k < 49; ++k) CHECK(ep.count[k] == 0);
    CHECK(total(ep) >= 50);
    CHECK(total(ep) <= 52);
    // the burst's place: the head, 10 up
    CHECK(ep.frame[0].t[2] == doctest::Approx(10));
    // the particles are numbered on from burst to burst
    for (size_t k = 1; k < ep.count.size(); ++k) CHECK(ep.first[k] == ep.first[k - 1] + ep.count[k - 1]);
}

TEST_CASE("a fraction of a particle a burst carries over") {
    const Rig rig = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nrate = 30\nstep_ms = 10\nlife = 300\n"));   // 0.3 a burst
    const ParticlePlan plan = plan_particles(rig, switch_clip(0), 1000, {});
    CHECK(total(plan.emitters[0]) == 30);   // 101 bursts of 0.3: 30.3
}

TEST_CASE("a particle flies at its speed and falls") {
    // straight ahead (+y) from the head at 10 up, 10 blocks a second, 2 voxels a block, gravity 30 blocks a second squared
    const Rig rig = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nrate = 100\nstep_ms = 10\nspeed = 10\nlife = 1000\n"));
    const ParticlePlan plan = plan_particles(rig, switch_clip(0), 1000, {});
    const std::vector<LiveParticle> live = live_particles(rig, plan, 100, false);
    REQUIRE(live.size() == 11);   // the bursts at 0, 10, ..., 100 ms
    const LiveParticle& p = live[0];   // the oldest, 0.1 s old
    CHECK(p.pos[0] == doctest::Approx(0).epsilon(1e-4).scale(1));
    CHECK(p.pos[1] == doctest::Approx(10 * 2 * 0.1f));
    CHECK(p.pos[2] == doctest::Approx(10 - 0.5f * 30 * 2 * 0.01f));
    CHECK(p.size == doctest::Approx(0.25f * 2));
    const LiveParticle& q = live.back();   // just thrown
    CHECK(q.pos[1] == doctest::Approx(0).epsilon(1e-4).scale(1));
}

TEST_CASE("a particle without gravity flies straight, along its emitter's turned way") {
    const Rig rig = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nsteps = [[\"rot_z\", 90]]\nrate = 100\nstep_ms = 10\n"
                                      "speed = 5\nlife = 1000\ngravity = false\n"));
    const ParticlePlan plan = plan_particles(rig, switch_clip(0), 1000, {});
    const LiveParticle p = live_particles(rig, plan, 200, false)[0];
    CHECK(p.pos[0] == doctest::Approx(-5 * 2 * 0.2f));   // rot_z 90 turns +y to -x
    CHECK(p.pos[1] == doctest::Approx(0).epsilon(1e-4).scale(1));
    CHECK(p.pos[2] == doctest::Approx(10));
}

TEST_CASE("a particle that stops at the ground stays where it landed") {
    // straight down from 10 up at 2 blocks (4 voxels) a second: it lands after 2.5 s
    const Rig rig = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nsteps = [[\"rot_x\", -90]]\nrate = 100\nstep_ms = 10\n"
                                      "speed = 2\nlife = 5000\ngravity = false\nground = \"stop\"\n"));
    const anim::Clip clip = switch_clip(0);
    const ParticlePlan plan = plan_particles(rig, clip, 1000, {});
    const LiveParticle late = live_particles(rig, plan, 1000, false)[0];   // 1 s old: 4 down
    CHECK(late.pos[2] == doctest::Approx(6));
    // passing through: 20 voxels a second from 10 up is 10 under the ground after 1 s
    const Rig pass = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nsteps = [[\"rot_x\", -90]]\nrate = 100\nstep_ms = 10\n"
                                       "speed = 10\nlife = 5000\ngravity = false\n"));
    CHECK(live_particles(pass, plan_particles(pass, clip, 1000, {}), 1000, false)[0].pos[2] == doctest::Approx(-10));
    // falling (60 voxels a second squared) from 10 up: down in 0.58 s, and it stays there
    const Rig fall = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nrate = 100\nstep_ms = 10\nspeed = 1\nlife = 5000\n"
                                       "ground = \"stop\"\n"));
    const LiveParticle f = live_particles(fall, plan_particles(fall, clip, 1000, {}), 1000, false)[0];
    CHECK(f.pos[2] == doctest::Approx(0));
    CHECK(f.pos[1] == doctest::Approx(2 * std::sqrt(20.0f / 60)).epsilon(1e-3));   // 2 voxels a second until it landed
}

TEST_CASE("a particle lives its life, shrinks and turns colour") {
    const Rig rig = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nrate = 100\nstep_ms = 10\nlife = 200\nsize = 1\nsize_end = 0\n"
                                      "color = [1, 1, 0]\ncolor_end = [1, 0, 0]\ngravity = false\n"));
    const ParticlePlan plan = plan_particles(rig, switch_clip(0), 1000, {});
    const std::vector<LiveParticle> live = live_particles(rig, plan, 500, false);
    REQUIRE(!live.empty());
    CHECK(live.size() == 20);   // the bursts at 310 to 500 ms: 200 ms old or less is alive, 200 itself is not
    const LiveParticle& old = live[0];   // 190 ms old
    CHECK(old.size == doctest::Approx((1 - 190.0f / 200) * 2).epsilon(1e-3));
    CHECK(old.rgb[1] == doctest::Approx(1 - 190.0f / 200).epsilon(1e-3));
    CHECK(old.rgb[0] == doctest::Approx(1));
}

TEST_CASE("a random colour mix and a random life stay in their ranges") {
    const Rig rig = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nrate = 1000\nstep_ms = 10\nlife = [100, 300]\n"
                                      "color = [1, 0, 0]\ncolor_random = [1, 1, 0]\nspeed = [1, 3]\ncone = 30\nspreading = 2\nspin = 90\n"));
    const ParticlePlan plan = plan_particles(rig, switch_clip(0), 1000, {});
    const std::vector<LiveParticle> live = live_particles(rig, plan, 600, false);
    REQUIRE(live.size() > 100);
    float lo = 1, hi = 0;
    for (const LiveParticle& p : live) {
        CHECK(p.rgb[0] == doctest::Approx(1));
        CHECK(p.rgb[2] == doctest::Approx(0));
        lo = std::min(lo, p.rgb[1]);
        hi = std::max(hi, p.rgb[1]);
    }
    CHECK(lo < 0.2f);
    CHECK(hi > 0.8f);
    CHECK(live.size() < 300);   // 10 a burst; bursts 300 ms old at most, and only some live past 100 ms
}

TEST_CASE("the particles at a time are the same whatever was asked before") {
    const Rig rig = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nrate = 500\nlife = [100, 400]\nspeed = [1, 3]\ncone = 40\n"
                                      "spreading = 1\nspin = 60\n"));
    const anim::Clip clip = switch_clip(0);
    const ParticlePlan plan = plan_particles(rig, clip, 1000, {});
    const std::vector<LiveParticle> a = live_particles(rig, plan, 300, false);
    (void)live_particles(rig, plan, 900, false);
    const std::vector<LiveParticle> b = live_particles(rig, plan_particles(rig, clip, 1000, {}), 300, false);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].pos[0] == b[i].pos[0]);
        CHECK(a[i].pos[1] == b[i].pos[1]);
        CHECK(a[i].rot[2] == b[i].rot[2]);
    }
}

TEST_CASE("a looping clip's start shows the bursts of the lap before") {
    const Rig rig = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nrate = 100\nstep_ms = 10\nlife = 300\n"));
    const ParticlePlan plan = plan_particles(rig, switch_clip(0), 1000, {});
    CHECK(live_particles(rig, plan, 50, false).size() == 6);           // 0 to 50 ms
    CHECK(live_particles(rig, plan, 50, true).size() == 6 + 24);       // and 760 to 990 ms of the lap before (1000 is this lap's 0)
}

TEST_CASE("a particle is a cube of its size, turned") {
    LiveParticle p;
    p.pos[0] = 5;
    p.size = 2;
    const Mesh m = particle_mesh({p});
    CHECK(m.triangle_count() == 12);
    Vec3 lo, hi;
    REQUIRE(mesh_bounds(m, lo, hi));
    CHECK(lo.x == doctest::Approx(4));
    CHECK(hi.x == doctest::Approx(6));
    p.rot[2] = 45;
    REQUIRE(mesh_bounds(particle_mesh({p}), lo, hi));
    CHECK(hi.x - lo.x == doctest::Approx(2 * std::sqrt(2.0f)));
    CHECK(particle_mesh({}).empty());
}

TEST_CASE("a sheet draws the particles") {
    const Rig rig = good_rig(rig_text("name = \"e\"\nparent = \"head\"\nrate = 400\nspeed = 3\nlife = 600\nsize = 1\ngravity = false\n"
                                      "color = [1, 0, 0]\n"));
    const Rig bare = good_rig(kNodes);
    const anim::Clip clip = switch_clip(0);
    std::vector<Mesh> meshes = {Mesh{}, mesh_box(4, 4, 4, 0x808080)};
    SheetRequest req;
    req.clip = &clip;
    req.meshes = &meshes;
    req.fractions = {0.5f};
    req.rig = &rig;
    const SheetResult with = make_mimic_sheet(req);
    req.rig = &bare;
    const SheetResult without = make_mimic_sheet(req);
    REQUIRE(with.ok);
    REQUIRE(without.ok);
    auto reds = [](const SheetResult& r) {
        size_t n = 0;
        for (size_t i = 0; i + 2 < r.image.rgb.size(); i += 3)
            if (r.image.rgb[i] > 120 && r.image.rgb[i + 1] < 60 && r.image.rgb[i + 2] < 60) ++n;
        return n;
    };
    CHECK(reds(with) > 50);
    CHECK(reds(without) == 0);
}
