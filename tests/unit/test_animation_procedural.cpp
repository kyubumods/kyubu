// Unit tests of the procedural tracks (openspec add-ability-animation tasks 8.1 and 8.2): `osc`, `spring` and `expr` in the evaluator,
// the validator and `kanim`. No game. The pure evaluator and the loader's own parser are what the game runs, so what is checked here
// is what the draw gets.
#include "doctest.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_expr.h"
#include "animation_load.h"
#include "kanim_core.h"

using namespace kyubu::game::anim;

namespace {

constexpr double kPi = 3.14159265358979323846;

Clip clip_of(const Track& tr, float duration_ms = 1000) {
    Clip c;
    c.duration_ms = duration_ms;
    c.tracks.push_back(tr);
    return c;
}

Track osc_track(float amp, float freq_hz, float decay = 0, float phase = 0, float centre = 0) {
    Track t;
    t.part = Part::Head;
    t.channel = Channel::Rot;
    t.source = Source::Osc;
    t.osc.amp[0] = amp;
    t.osc.centre[0] = centre;
    t.osc.freq_hz = freq_hz;
    t.osc.decay = decay;
    t.osc.phase_deg = phase;
    return t;
}

Track spring_track(float to, float at, float freq_hz, float damping, float from = 0) {
    Track t;
    t.part = Part::Body;
    t.channel = Channel::Pos;
    t.source = Source::Spring;
    t.spring.from[2] = from;
    t.spring.to[2] = to;
    t.spring.at = at;
    t.spring.freq_hz = freq_hz;
    t.spring.damping = damping;
    return t;
}

// The value of axis `axis` of a generator track `ms` into a clip of `duration_ms`.
float value_at(const Track& tr, float ms, int axis = 0, float duration_ms = 1000, const Signals& sg = Signals()) {
    float v[3];
    sample_track(tr, ms / duration_ms, duration_ms, sg, v);
    return v[axis];
}

bool compile(const std::string& text, ExprProgram& p, ExprError* e = nullptr) {
    ExprError local;
    ExprError& err = e ? *e : local;
    return compile_expr(text, p, err);
}

float run(const std::string& text, float u = 0, float t_ms = 0, const Signals& sg = Signals()) {
    ExprProgram p;
    ExprError err;
    REQUIRE_MESSAGE(compile_expr(text, p, err), text << ": " << err.message);
    return eval_expr(p, u, t_ms, sg);
}

ExprError fails(const std::string& text) {
    ExprProgram p;
    ExprError err;
    CHECK_MESSAGE(!compile_expr(text, p, err), "`" << text << "` compiled");
    CHECK(p.empty());
    return err;
}

std::string many_ones(int n) {
    std::string s = "1";
    for (int i = 1; i < n; ++i) s += "+1";
    return s;
}

const std::string kDesign = R"(duration = 1000
end = "hold"

[[track]]
part = "head"
channel = "rot"
osc = { amp = [5, 0, 0], freq = 3, decay = 4 }
)";

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// 8.1 osc

TEST_CASE("osc: amplitude, frequency, phase and centre") {
    Track t = osc_track(5, 2);   // 2 Hz: a quarter cycle is 125 ms
    CHECK(value_at(t, 0) == doctest::Approx(0).epsilon(1e-6));
    CHECK(value_at(t, 125) == doctest::Approx(5).epsilon(1e-4));
    CHECK(value_at(t, 250) == doctest::Approx(0).epsilon(1e-3));
    CHECK(value_at(t, 375) == doctest::Approx(-5).epsilon(1e-4));
    CHECK(value_at(t, 500) == doctest::Approx(0).epsilon(1e-3));
    // The other axes have no amplitude and stay at the centre of 0.
    CHECK(value_at(t, 125, 1) == 0);
    CHECK(value_at(t, 125, 2) == 0);

    // A phase of 90 degrees starts at the top; a centre moves the whole swing.
    Track p = osc_track(5, 2, 0, 90, 10);
    CHECK(value_at(p, 0) == doctest::Approx(15).epsilon(1e-5));
    CHECK(value_at(p, 250) == doctest::Approx(5).epsilon(1e-4));
}

TEST_CASE("osc: a decay settles the swing") {
    Track t = osc_track(5, 2, 4);   // e^(-4 s)
    CHECK(value_at(t, 125) == doctest::Approx(5 * std::exp(-0.5)).epsilon(1e-4));
    CHECK(value_at(t, 625) == doctest::Approx(5 * std::exp(-2.5)).epsilon(1e-4));
    // The peaks shrink, and in the last tenth of a second the swing is under 4 percent of the amplitude.
    float last = 5.0f;
    for (int cycle = 0; cycle < 2; ++cycle) {
        const float peak = value_at(t, 125.0f + 500.0f * cycle);
        CHECK(peak <= last);
        last = peak;
    }
    float tail = 0;
    for (int ms = 900; ms <= 1000; ms += 10) tail = std::fmax(tail, std::fabs(value_at(t, static_cast<float>(ms))));
    CHECK(tail < 0.2f);
    // No decay: the swing is as large at the end as at the start.
    Track steady = osc_track(5, 2, 0);
    CHECK(value_at(steady, 875) == doctest::Approx(-5).epsilon(1e-3));
}

TEST_CASE("osc: it is a track like any other (add, set, blend, the end of the clip)") {
    Track t = osc_track(5, 2);
    Clip c = clip_of(t);
    Timing tm;
    tm.duration_ms = 1000;
    tm.elapsed_ms = 125;

    Pose base;
    base.parts[static_cast<int>(Part::Head)].rot[0] = 20;
    Pose add = base;
    Applied a = apply_clip(c, tm, add);
    CHECK(a.ok);
    CHECK((a.rot_parts & (1u << static_cast<int>(Part::Head))) != 0);
    CHECK(a.pos_parts == 0);
    CHECK(add.parts[static_cast<int>(Part::Head)].rot[0] == doctest::Approx(25).epsilon(1e-4));

    c.tracks[0].mode = TrackMode::Set;
    Pose set = base;
    apply_clip(c, tm, set);
    CHECK(set.parts[static_cast<int>(Part::Head)].rot[0] == doctest::Approx(5).epsilon(1e-4));

    // A hold clip past its end is at the pose of its last moment; a loop clip is at the pose of its lap.
    c.tracks[0].mode = TrackMode::Add;
    Pose past = Pose();
    tm.elapsed_ms = 5000;
    apply_clip(c, tm, past);
    CHECK(past.parts[static_cast<int>(Part::Head)].rot[0] == doctest::Approx(value_at(t, 1000)).epsilon(1e-4));
    c.end = End::Loop;
    Pose lap = Pose();
    tm.elapsed_ms = 1125;
    apply_clip(c, tm, lap);
    CHECK(lap.parts[static_cast<int>(Part::Head)].rot[0] == doctest::Approx(5).epsilon(1e-3));

    // blend_in scales it, like a key's value.
    c.end = End::Hold;
    c.blend_in_ms = 250;
    Pose blended = Pose();
    tm.elapsed_ms = 125;
    Applied b = apply_clip(c, tm, blended);
    CHECK(b.weight == doctest::Approx(0.5f));
    CHECK(blended.parts[static_cast<int>(Part::Head)].rot[0] == doctest::Approx(2.5f).epsilon(1e-3));
}

TEST_CASE("osc: a result is held to the channel's bound, and a clip of no duration reads time 0") {
    Track t = osc_track(5000, 1, 0, 90);
    CHECK(value_at(t, 0) == kMaxRotDeg);
    t.channel = Channel::Pos;
    CHECK(value_at(t, 0) == kMaxPos);
    // The form with no duration gives a generator its value at time 0.
    float v[3];
    sample_track(osc_track(5, 2, 0, 90), 0.5f, v);
    CHECK(v[0] == doctest::Approx(5).epsilon(1e-5));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// 8.1 spring

TEST_CASE("spring: it waits, overshoots by the damping ratio, and settles") {
    const double zeta = 0.3, f = 3.0;
    Track t = spring_track(10, 0.2f, static_cast<float>(f), static_cast<float>(zeta));
    auto z = [&](float ms) { return value_at(t, ms, 2); };

    CHECK(z(0) == 0);
    CHECK(z(100) == 0);
    CHECK(z(200) == doctest::Approx(0).epsilon(1e-6));   // let go at 200 ms, at rest

    // The first peak: pi / wd seconds after release, 10 * (1 + exp(-zeta pi / sqrt(1 - zeta^2))).
    const double w0 = 2 * kPi * f, wd = w0 * std::sqrt(1 - zeta * zeta);
    const double peak_ms = 200 + 1000 * kPi / wd;
    const double overshoot = std::exp(-zeta * kPi / std::sqrt(1 - zeta * zeta));
    CHECK(z(static_cast<float>(peak_ms)) == doctest::Approx(10 * (1 + overshoot)).epsilon(1e-3));
    float top = 0;
    for (int ms = 200; ms <= 1000; ++ms) top = std::fmax(top, z(static_cast<float>(ms)));
    CHECK(top == doctest::Approx(10 * (1 + overshoot)).epsilon(2e-3));
    CHECK(top > 10);

    // It rings down: a later peak is lower, and well after the release the value sits on the target.
    CHECK(value_at(t, 2000, 2, 4000) == doctest::Approx(10).epsilon(2e-3));
    CHECK(value_at(t, 3999, 2, 4000) == doctest::Approx(10).epsilon(1e-4));
    // It writes only the axis the track names; the position channel has the vector.
    float v[3];
    sample_track(t, 0.7f, 1000, Signals(), v);
    CHECK(v[0] == 0);
    CHECK(v[1] == 0);
}

TEST_CASE("spring: critical and over-damped springs never overshoot") {
    for (float zeta : {1.0f, 1.00001f, 1.5f, 4.0f}) {
        Track t = spring_track(10, 0, 3, zeta);
        float prev = -1;
        for (int ms = 0; ms <= 3000; ms += 5) {
            const float v = value_at(t, static_cast<float>(ms), 2, 3000);
            CHECK(v <= 10.001f);
            CHECK(v >= prev - 1e-3f);   // never moves back
            prev = v;
        }
        CHECK_MESSAGE(prev > 9.0f, "zeta " << zeta << " reached " << prev);
    }
    // Over-damped is slower than critical to arrive.
    CHECK(value_at(spring_track(10, 0, 3, 4), 300, 2, 3000) < value_at(spring_track(10, 0, 3, 1), 300, 2, 3000));
}

TEST_CASE("spring: a `from` other than 0, and the same value with a seek in any order") {
    Track t = spring_track(-4, 0.5f, 2, 0.5f, 6);
    CHECK(value_at(t, 400, 2) == 6);
    // Closed form: no state, so asking out of order gives the same numbers.
    const float a = value_at(t, 900, 2), b = value_at(t, 700, 2);
    CHECK(value_at(t, 900, 2) == a);
    CHECK(value_at(t, 700, 2) == b);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// 8.2 expr: the parser

TEST_CASE("expr: numbers, operators, precedence and parentheses") {
    CHECK(run("2 + 3 * 4") == 14);
    CHECK(run("(2 + 3) * 4") == 20);
    CHECK(run("10 / 4") == 2.5f);
    CHECK(run("1 - 2 - 3") == -4);
    CHECK(run("2 * -3") == -6);
    CHECK(run("--3") == 3);
    CHECK(run("-2 * 3") == -6);
    CHECK(run("  7  ") == 7);
    CHECK(run("1.5e2") == 150);
    CHECK(run("2.5E-1") == 0.25f);
    CHECK(run(".5 + .25") == 0.75f);
    CHECK(run("5.") == 5);
}

TEST_CASE("expr: the names and the signals") {
    Signals sg;
    sg.charge = 0.5f;
    sg.speed = 8;
    sg.rank = 3;
    CHECK(run("u", 0.25f, 250, sg) == 0.25f);
    CHECK(run("t", 0.25f, 250, sg) == 250);
    CHECK(run("charge * 40", 0, 0, sg) == 20);
    CHECK(run("speed - 2", 0, 0, sg) == 6);
    CHECK(run("rank", 0, 0, sg) == 3);
    // A default signal set is 0 charge, 0 speed and rank 1.
    CHECK(run("charge + speed + rank") == 1);

    ExprProgram p;
    REQUIRE(compile("u * charge + rank", p));
    CHECK(p.uses == (kUsesU | kUsesCharge | kUsesRank));
    REQUIRE(compile("5", p));
    CHECK(p.uses == 0);
}

TEST_CASE("expr: the functions, angles in degrees") {
    CHECK(run("sin(90)") == doctest::Approx(1).epsilon(1e-6));
    CHECK(run("sin(30)") == doctest::Approx(0.5).epsilon(1e-6));
    CHECK(run("cos(0)") == 1);
    CHECK(run("cos(180)") == doctest::Approx(-1).epsilon(1e-6));
    CHECK(run("abs(-3.5)") == 3.5f);
    CHECK(run("min(2, 5)") == 2);
    CHECK(run("max(2, 5)") == 5);
    CHECK(run("clamp(7, 0, 5)") == 5);
    CHECK(run("clamp(-7, 0, 5)") == 0);
    CHECK(run("clamp(3, 0, 5)") == 3);
    CHECK(run("lerp(10, 20, 0.25)") == 12.5f);
    CHECK(run("smoothstep(0, 1, 0.5)") == 0.5f);
    CHECK(run("smoothstep(0, 1, -1)") == 0);
    CHECK(run("smoothstep(0, 1, 2)") == 1);
    CHECK(run("smoothstep(2, 2, 1)") == 0);   // an empty range is a step
    CHECK(run("smoothstep(2, 2, 3)") == 1);
    CHECK(run("pow(2, 10)") == 1024);
    CHECK(run("pow(9, 0.5)") == 3);
    // Calls nest, and a sum is an argument.
    CHECK(run("max(abs(-2), min(1 + 1, 5))") == 2);
    CHECK(run("sin(u * 360) * 4", 0.25f) == doctest::Approx(4).epsilon(1e-5));
}

TEST_CASE("expr: noise is smooth, bounded, and the same every time") {
    ExprProgram p;
    REQUIRE(compile("noise(t * 0.01)", p));
    Signals sg;
    float prev = eval_expr(p, 0, 0, sg);
    float lo = prev, hi = prev;
    for (int ms = 1; ms <= 5000; ++ms) {
        const float v = eval_expr(p, 0, static_cast<float>(ms), sg);
        CHECK(std::fabs(v - prev) < 0.05f);   // no jump between neighbouring milliseconds
        lo = std::fmin(lo, v);
        hi = std::fmax(hi, v);
        prev = v;
    }
    CHECK(lo >= -1.0f);
    CHECK(hi <= 1.0f);
    CHECK(hi - lo > 0.5f);   // it does move
    // Deterministic: the same inputs, the same number, from a second compile too.
    ExprProgram q;
    REQUIRE(compile("noise(t * 0.01)", q));
    for (float ms : {0.0f, 13.0f, 777.0f, 4999.0f}) CHECK(eval_expr(p, 0, ms, sg) == eval_expr(q, 0, ms, sg));
    // At a whole number the value is the lattice value, and noise of a huge number is 0 rather than a crash.
    CHECK(run("noise(1e9)") == 0);
}

TEST_CASE("expr: a result that is not a number is 0") {
    CHECK(run("1 / 0") == 0);
    CHECK(run("u / (u - u)") == 0);
    CHECK(run("pow(10, 1000)") == 0);
    CHECK(run("pow(-8, 0.5)") == 0);
    CHECK(run("1e9 * 1e9 * 1e9 * 1e9 * 1e9") == 0);   // beyond a float
}

TEST_CASE("expr: errors have a code and a column") {
    {
        ExprError e = fails("sin(u) + foo");
        CHECK(e.code == "unknown_name");
        CHECK(e.column == 10);
        CHECK(e.message.find("foo") != std::string::npos);
        CHECK(e.message.find("charge") != std::string::npos);   // the message lists the names
    }
    {
        ExprError e = fails("2 * bar(1)");
        CHECK(e.code == "unknown_function");
        CHECK(e.column == 5);
        CHECK(e.message.find("smoothstep") != std::string::npos);
    }
    {
        ExprError e = fails("min(1)");
        CHECK(e.code == "bad_arg_count");
        CHECK(e.column == 1);
        CHECK(e.message.find("2 arguments, not 1") != std::string::npos);
    }
    {
        ExprError e = fails("sin()");
        CHECK(e.code == "bad_arg_count");
    }
    {
        ExprError e = fails("1 +");
        CHECK(e.code == "bad_expression");
        CHECK(e.column == 4);
        CHECK(e.message.find("end of the text") != std::string::npos);
    }
    {
        ExprError e = fails("2 $ 3");
        CHECK(e.code == "bad_expression");
        CHECK(e.column == 3);
        CHECK(e.message.find("`$`") != std::string::npos);
    }
    {
        ExprError e = fails("(1 + 2");
        CHECK(e.code == "bad_expression");
        CHECK(e.column == 7);
    }
    {
        ExprError e = fails("1)");
        CHECK(e.code == "bad_expression");
        CHECK(e.column == 2);
    }
    {
        ExprError e = fails("sin");
        CHECK(e.code == "bad_expression");
        CHECK(e.message.find("function") != std::string::npos);
    }
    {
        ExprError e = fails("");
        CHECK(e.code == "bad_expression");
        CHECK(e.column == 1);
        ExprError s = fails("   ");
        CHECK(s.code == "bad_expression");
    }
    {
        ExprError e = fails("1.2.3");
        CHECK(e.column == 4);   // the second point
    }
    {
        ExprError e = fails("u u");
        CHECK(e.column == 3);
    }
    {
        ExprError e = fails("max(1, 2");
        CHECK(e.code == "bad_expression");
        CHECK(e.message.find("`,` or `)`") != std::string::npos);
    }
    {
        ExprError e = fails("1e999");
        CHECK(e.code == "bad_expression");
        CHECK(e.column == 1);
    }
}

TEST_CASE("expr: no loop, no assignment, no recursion; the text is bounded") {
    // The grammar has none of them, so each is an unknown word or an unexpected character.
    CHECK(fails("for(u)").code == "unknown_function");
    CHECK(fails("while").code == "unknown_name");
    CHECK(fails("if(u, 1, 2)").code == "unknown_function");
    CHECK(fails("x = 3").code == "unknown_name");
    CHECK(fails("u = 3").code == "bad_expression");
    CHECK(fails("u; u").code == "bad_expression");
    CHECK(fails("f(f(f(u)))").code == "unknown_function");
    CHECK(fails("u ^ 2").code == "bad_expression");
    CHECK(fails("u > 1").code == "bad_expression");
    CHECK(fails("[1, 2]").code == "bad_expression");
}

TEST_CASE("expr: the node cap is 64, the depth and the length are bounded") {
    ExprProgram p;
    // 32 numbers and 31 plus signs are 63 nodes; one more number is 65.
    CHECK(compile(many_ones(32), p));
    CHECK(p.code.size() == 63);
    CHECK(run(many_ones(32)) == 32);
    ExprError e;
    CHECK_FALSE(compile(many_ones(33), p, &e));
    CHECK(e.code == "expr_too_big");
    CHECK(e.message.find("64") != std::string::npos);
    CHECK(e.column > 1);

    // A call is a node of its own: 31 numbers and 30 plus signs with an abs is 62 nodes, two nested calls with 30 numbers 61.
    CHECK(compile("abs(" + many_ones(31) + ")", p));
    CHECK(p.code.size() == 62);
    CHECK(compile("abs(abs(" + many_ones(30) + "))", p));
    CHECK_FALSE(compile("abs(abs(abs(" + many_ones(32) + ")))", p, &e));

    // Nesting.
    std::string deep = "1";
    for (int i = 0; i < 12; ++i) deep = "(" + deep + ")";
    CHECK(compile(deep, p));
    for (int i = 0; i < 30; ++i) deep = "(" + deep + ")";
    CHECK_FALSE(compile(deep, p, &e));
    CHECK(e.code == "expr_too_big");
    std::string minus(200, '-');
    CHECK_FALSE(compile(minus + "1", p, &e));
    CHECK(e.code == "expr_too_big");

    // Length.
    CHECK_FALSE(compile(std::string(300, ' ') + "1", p, &e));
    CHECK(e.code == "expr_too_big");
    CHECK(e.message.find("256") != std::string::npos);
}

TEST_CASE("expr: a track evaluates each axis alone, and is held to the bound") {
    Track t;
    t.part = Part::HandR;
    t.channel = Channel::Rot;
    t.source = Source::Expr;
    ExprError err;
    REQUIRE(compile_expr("charge * 40", t.expr[0], err));
    REQUIRE(compile_expr("u * 90", t.expr[1], err));
    REQUIRE(compile_expr("9999", t.expr[2], err));
    Signals sg;
    sg.charge = 0.5f;
    float v[3];
    sample_track(t, 0.5f, 1000, sg, v);
    CHECK(v[0] == 20);
    CHECK(v[1] == 45);
    CHECK(v[2] == kMaxRotDeg);   // held to the bound

    // Through a clip: the signals ride in the Timing, so a held ability's charge moves the pose.
    Clip c = clip_of(t);
    Timing tm;
    tm.duration_ms = 1000;
    tm.elapsed_ms = 500;
    tm.signals.charge = 0;
    Pose none;
    apply_clip(c, tm, none);
    CHECK(none.parts[static_cast<int>(Part::HandR)].rot[0] == 0);
    tm.signals.charge = 1;
    Pose full;
    apply_clip(c, tm, full);
    CHECK(full.parts[static_cast<int>(Part::HandR)].rot[0] == 40);
    CHECK(clip_expr_uses(c) == (kUsesU | kUsesCharge));
    CHECK(clip_expr_uses(clip_of(osc_track(1, 1))) == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The validator and the loader

TEST_CASE("clip file: the three generators load, with their defaults") {
    const std::string text = R"(duration = 800

[[track]]
part = "head"
channel = "rot"
osc = { amp = [5, 0, 0], freq = 3, decay = 4 }

[[track]]
part = "body"
channel = "pos"
spring = { to = [0, 0, -2], at = 0.5, freq = 3, damping = 0.3 }

[[track]]
part = "weapon_r"
channel = "rot"
mode = "set"
expr = ["charge * 40", "sin(u * 360) * 4", "0"]
)";
    ClipLoad r = parse_clip(text, "gen.anim.toml");
    REQUIRE_MESSAGE(r.ok, (r.errors.empty() ? "" : format_error(r.errors[0])));
    REQUIRE(r.clip.tracks.size() == 3);
    const Track& o = r.clip.tracks[0];
    CHECK(o.source == Source::Osc);
    CHECK(o.keys.empty());
    CHECK(o.osc.amp[0] == 5);
    CHECK(o.osc.freq_hz == 3);
    CHECK(o.osc.decay == 4);
    CHECK(o.osc.phase_deg == 0);
    CHECK(o.osc.centre[1] == 0);
    const Track& s = r.clip.tracks[1];
    CHECK(s.source == Source::Spring);
    CHECK(s.spring.to[2] == -2);
    CHECK(s.spring.at == 0.5f);
    CHECK(s.spring.from[2] == 0);
    const Track& x = r.clip.tracks[2];
    CHECK(x.source == Source::Expr);
    CHECK(x.mode == TrackMode::Set);
    CHECK(x.part == Part::HandR);
    CHECK_FALSE(x.expr[0].empty());
    CHECK(x.expr[0].uses == kUsesCharge);
    CHECK(x.expr[1].uses == kUsesU);

    // The spring's default damping is the struct's.
    ClipLoad d = parse_clip(R"(duration = 100
[[track]]
part = "head"
channel = "rot"
spring = { to = [1, 0, 0], freq = 2 }
)", "d.anim.toml");
    REQUIRE(d.ok);
    CHECK(d.clip.tracks[0].spring.damping == doctest::Approx(0.3f));
    CHECK(d.clip.tracks[0].spring.at == 0);
}

TEST_CASE("clip file: errors in a generator name the path, the line and, for an expr, the column") {
    auto errors = [](const std::string& body) {
        return parse_clip("duration = 500\n[[track]]\npart = \"head\"\nchannel = \"rot\"\n" + body, "bad.anim.toml");
    };
    {
        ClipLoad r = errors("osc = { amp = [5, 0, 0], freq = 0 }\n");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "out_of_range");
        CHECK(r.errors[0].path == "track[0].osc.freq");
        CHECK(r.errors[0].line == 5);
    }
    {
        ClipLoad r = errors("osc = { freq = 3 }\n");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "missing_field");
        CHECK(r.errors[0].path == "track[0].osc.amp");
    }
    {
        ClipLoad r = errors("osc = { amp = [5, 0, 0], freq = 3, center = [0, 0, 0] }\n");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "unknown_key");
        CHECK(r.errors[0].message.find("centre") != std::string::npos);
    }
    {
        ClipLoad r = errors("osc = { amp = [5, 0], freq = 3 }\n");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "bad_length");
        CHECK(r.errors[0].path == "track[0].osc.amp");
    }
    {
        ClipLoad r = errors("osc = { amp = [5000, 0, 0], freq = 3 }\n");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "out_of_range");
        CHECK(r.errors[0].path == "track[0].osc.amp[0]");
    }
    {
        ClipLoad r = errors("osc = 3\n");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "bad_type");
    }
    {
        ClipLoad r = errors("spring = { to = [0, 0, 1], freq = 3, damping = 0 }\n");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].path == "track[0].spring.damping");
    }
    {
        ClipLoad r = errors("spring = { freq = 3 }\n");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "missing_field");
        CHECK(r.errors[0].path == "track[0].spring.to");
    }
    {
        ClipLoad r = errors("spring = { to = [0, 0, 1], freq = 3, at = 1.5 }\n");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].path == "track[0].spring.at");
    }
    {
        // An expression error carries its column; the third axis is checked too.
        ClipLoad r = errors(R"(expr = ["u * 5", "0", "sin(u) + wibble"]
)");
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].code == "unknown_name");
        CHECK(r.errors[0].path == "track[0].expr[2]");
        CHECK(r.errors[0].line == 5);
        CHECK(r.errors[0].column == 10);
        const std::string text = format_error(r.errors[0]);
        CHECK(text.find("track[0].expr[2]: column 10: unknown name `wibble`") != std::string::npos);
        const std::string js = errors_to_json(r);
        CHECK(js.find("\"column\":10") != std::string::npos);
        CHECK(js.find("\"code\":\"unknown_name\"") != std::string::npos);
    }
    {
        ClipLoad r = errors(R"(expr = ["1", "0"]
)");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "bad_length");
    }
    {
        ClipLoad r = errors(R"(expr = ["1", 0, "0"]
)");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "bad_type");
        CHECK(r.errors[0].path == "track[0].expr[1]");
    }
    {
        // 65 nodes: the clip is rejected (the spec's "unbounded expression").
        ClipLoad r = errors("expr = [\"" + many_ones(33) + "\", \"0\", \"0\"]\n");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "expr_too_big");
        CHECK(r.clip.tracks.empty());
    }
    {
        // Keys and a generator in one track, and two generators: an error at the second.
        ClipLoad r = errors("keys = [{ t = 0, v = [0, 0, 0] }]\nosc = { amp = [1, 0, 0], freq = 1 }\n");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "two_sources");
        CHECK(r.errors[0].path == "track[0].osc");
        CHECK(r.errors[0].line == 6);
        ClipLoad two = errors("osc = { amp = [1, 0, 0], freq = 1 }\nspring = { to = [1, 0, 0], freq = 1 }\n");
        REQUIRE_FALSE(two.ok);
        CHECK(two.errors[0].code == "two_sources");
        CHECK(two.errors[0].path == "track[0].spring");
    }
    {
        // No source at all keeps the old error.
        ClipLoad r = errors("");
        REQUIRE_FALSE(r.ok);
        CHECK(r.errors[0].code == "missing_field");
        CHECK(r.errors[0].path == "track[0].keys");
    }
}

TEST_CASE("clip file: a generator's bound is the channel's") {
    ClipLoad r = parse_clip(R"(duration = 500
[[track]]
part = "head"
channel = "pos"
osc = { amp = [150, 0, 0], freq = 1 }
)", "p.anim.toml");
    REQUIRE_FALSE(r.ok);
    CHECK(r.errors[0].code == "out_of_range");
    CHECK(r.errors[0].path == "track[0].osc.amp[0]");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// kanim

TEST_CASE("kanim sample: a generator at the clip's fractions is the evaluator's number") {
    kyubu::kanim::Options o;
    o.times = {0.0f, 0.125f, 0.5f};
    kyubu::kanim::Result r = kyubu::kanim::sample(kDesign, "d.anim.toml", o);
    REQUIRE(r.code == 0);
    // 3 Hz at 125 ms of 1000: sin(135 degrees) * 5 * e^(-0.5)
    const double want = 5 * std::sin(2 * kPi * 3 * 0.125) * std::exp(-0.5);
    char buf[64];
    std::snprintf(buf, sizeof buf, "head: rot %.2f 0 0", want);
    CHECK_MESSAGE(r.text.find(buf) != std::string::npos, r.text);
    // The header gains the signals only for an expr clip.
    CHECK(r.text.find("charge") == std::string::npos);

    const std::string expr_clip = R"(duration = 1000
[[track]]
part = "hand_r"
channel = "rot"
expr = ["charge * 40 + rank", "0", "0"]
)";
    o.signals.charge = 0.5f;
    o.signals.rank = 2;
    o.times = {0.5f};
    kyubu::kanim::Result e = kyubu::kanim::sample(expr_clip, "e.anim.toml", o);
    REQUIRE(e.code == 0);
    CHECK(e.text.find("charge 0.5, speed 0, rank 2") != std::string::npos);
    CHECK(e.text.find("hand_r: rot 22 0 0") != std::string::npos);
}

TEST_CASE("kanim describe: a generator is read from samples, with its numbers in words") {
    kyubu::kanim::Options o;
    kyubu::kanim::Result r = kyubu::kanim::describe(kDesign, "d.anim.toml", o);
    REQUIRE(r.code == 0);
    CHECK(r.text.find("head rot (add):") != std::string::npos);
    CHECK(r.text.find("generator: osc, centre 0 0 0, amp 5 0 0, 3 Hz, phase 0 deg, decay 4 /s") != std::string::npos);
    CHECK(r.text.find("fastest:") != std::string::npos);
    // It starts at the centre, which is 0 on an add track: no pop at the start. It has not settled to 0 by the end of a short
    // clip with a slow decay, so the end is reported.
    CHECK(r.text.find("start:") == std::string::npos);

    const std::string spring = R"(duration = 1000
[[track]]
part = "body"
channel = "pos"
spring = { to = [0, 0, -2], at = 0.2, freq = 3, damping = 0.3 }
)";
    kyubu::kanim::Result s = kyubu::kanim::describe(spring, "s.anim.toml", o);
    REQUIRE(s.code == 0);
    CHECK(s.text.find("generator: spring, from 0 0 0 to 0 0 -2 at 0.2, 3 Hz, damping 0.3 (overshoots and rings)") != std::string::npos);
    // The overshoot is in the range: past -2 (to -2.74).
    CHECK(s.text.find("z -2.7") != std::string::npos);
    // Holds -2 at the end of a run with blend_out 0: the pop is named.
    CHECK(s.text.find("end: body pos (add) holds 2 vox off the base pose") != std::string::npos);

    const std::string expr_clip = R"(duration = 1000
[[track]]
part = "hand_r"
channel = "rot"
expr = ["charge * 40", "speed", "0"]
)";
    kyubu::kanim::Result e = kyubu::kanim::describe(expr_clip, "e.anim.toml", o);
    REQUIRE(e.code == 0);
    CHECK(e.text.find("generator: expr, reads charge, speed") != std::string::npos);
    CHECK(e.text.find("signals: charge, speed (read from the game; shown with charge 0, speed 0, rank 1;") != std::string::npos);
    o.signals.charge = 1;
    kyubu::kanim::Result f = kyubu::kanim::describe(expr_clip, "e.anim.toml", o);
    CHECK(f.text.find("x 40..40") != std::string::npos);
}

TEST_CASE("kanim bake: a generator becomes plain keys that evaluate to the same pose") {
    kyubu::kanim::Options o;
    kyubu::kanim::Result r = kyubu::kanim::bake(kDesign, "d.anim.toml", o);
    REQUIRE(r.code == 0);
    CHECK(r.text.find("osc") == std::string::npos);
    CHECK(r.text.find("# generators baked over 1000 ms with charge 0, speed 0, rank 1") != std::string::npos);

    ClipLoad orig = parse_clip(kDesign, "d.anim.toml");
    ClipLoad baked = parse_clip(r.text, "baked.anim.toml");
    REQUIRE(orig.ok);
    REQUIRE_MESSAGE(baked.ok, (baked.errors.empty() ? "" : format_error(baked.errors[0])));
    REQUIRE(baked.clip.tracks.size() == 1);
    CHECK(baked.clip.tracks[0].source == Source::Keys);
    // One key per 1/60 s of 1000 ms, the two ends in.
    CHECK(baked.clip.tracks[0].keys.size() == 61);
    CHECK(baked.clip.tracks[0].keys.front().t == 0);
    CHECK(baked.clip.tracks[0].keys.back().t == 1);
    // At a key the baked clip is the generator's value; between keys it is within the linear interpolation's error.
    for (float u : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        float a[3], b[3];
        sample_track(orig.clip.tracks[0], u, 1000, Signals(), a);
        sample_track(baked.clip.tracks[0], u, 1000, Signals(), b);
        CHECK_MESSAGE(std::fabs(a[0] - b[0]) < 0.25f, "u " << u << ": " << a[0] << " vs " << b[0]);
    }

    // A signal is frozen at the value given; the long clip is capped at kMaxKeysPerTrack keys.
    const std::string expr_clip = R"(duration = 10000
[[track]]
part = "hand_r"
channel = "rot"
expr = ["charge * 40", "0", "0"]
)";
    o.signals.charge = 0.5f;
    kyubu::kanim::Result e = kyubu::kanim::bake(expr_clip, "e.anim.toml", o);
    REQUIRE(e.code == 0);
    ClipLoad eb = parse_clip(e.text, "eb.anim.toml");
    REQUIRE(eb.ok);
    CHECK(eb.clip.tracks[0].keys.size() == static_cast<size_t>(kMaxKeysPerTrack));
    CHECK(eb.clip.tracks[0].keys[7].v[0] == 20);
    CHECK(e.text.find("with charge 0.5, speed 0, rank 1") != std::string::npos);
}

TEST_CASE("kanim: a generator clip with an error prints the column") {
    kyubu::kanim::Options o;
    const std::string bad = R"(duration = 1000
[[track]]
part = "hand_r"
channel = "rot"
expr = ["foo", "0", "0"]
)";
    kyubu::kanim::Result r = kyubu::kanim::check(bad, "b.anim.toml", o);
    CHECK(r.code == 1);
    CHECK(r.text.find("b.anim.toml:5: track[0].expr[0]: column 1: unknown name `foo`") != std::string::npos);
    o.json = true;
    kyubu::kanim::Result j = kyubu::kanim::check(bad, "b.anim.toml", o);
    CHECK(j.text.find("\"column\":1") != std::string::npos);
}

TEST_CASE("kanim: the documented procedural clip checks, describes and samples as the golden files say") {
    // The clip is the sample block of docs/animations.md ("Procedural tracks"): check_docs.py keeps the two the same.
    const std::string dir = KYUBU_SOURCE_DIR "/tests/fixtures/kanim/";
    auto slurp = [](const std::string& path) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        REQUIRE_MESSAGE(f != nullptr, "cannot read " << path);
        std::string s;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        std::fclose(f);
        return s;
    };
    const std::string clip = slurp(dir + "procedural.anim.toml");
    kyubu::kanim::Options o;
    kyubu::kanim::Result c = kyubu::kanim::check(clip, "procedural.anim.toml", o);
    CHECK(c.code == 0);
    CHECK(c.text == "procedural.anim.toml: ok\n");
    CHECK(kyubu::kanim::describe(clip, "procedural.anim.toml", o).text == slurp(dir + "golden/describe_procedural.txt"));
    o.times = {0.0f, 0.25f, 0.5f, 1.0f};
    o.signals.charge = 0.5f;
    CHECK(kyubu::kanim::sample(clip, "procedural.anim.toml", o).text == slurp(dir + "golden/sample_procedural.txt"));

    // All three kinds of source are in it, and it has no keys at all.
    ClipLoad r = parse_clip(clip, "procedural.anim.toml");
    REQUIRE(r.ok);
    REQUIRE(r.clip.tracks.size() == 3);
    CHECK(r.clip.tracks[0].source == Source::Spring);
    CHECK(r.clip.tracks[1].source == Source::Osc);
    CHECK(r.clip.tracks[2].source == Source::Expr);
    for (const Track& t : r.clip.tracks) CHECK(t.keys.empty());
}
