#include "animation_eval.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kyubu::game::anim {

namespace {

const char* const kPartNames[kPartCount] = {"root", "body", "head", "hand_r", "hand_l", "tail"};
const char* const kEaseNames[] = {"linear", "smooth", "ease_in", "ease_out", "hold"};
const char* const kSourceNames[] = {"keys", "osc", "spring", "expr"};
struct HideName { uint32_t bit; const char* name; };
const HideName kHideNames[] = {{kHideHead, "head"}, {kHideHair, "hair"}, {kHideBody, "body"},
                               {kHideShoulders, "shoulders"}, {kHideWeapons, "weapons"}};

float clamp01(float f) {
    if (!(f > 0)) return 0;   // also catches NaN
    return f > 1 ? 1 : f;
}

// The clip's own fraction at a time: hold clamps at 1, loop wraps. The caller checked duration_ms > 0.
float norm_time(const Clip& c, const Timing& t) {
    float e = t.elapsed_ms;
    if (!(e > 0)) return 0;
    // The store through a volatile rounds the quotient to a float. The loader is built with x87 maths, where the value would
    // stay in extended precision (a cast does not round it), and 200 ms of 1,000 would compare below a `to` of 0.2f.
    volatile float u = c.end == End::Loop ? std::fmod(e, t.duration_ms) / t.duration_ms : e / t.duration_ms;
    if (c.end == End::Loop) {
        float w = u;
        return w < 0 ? 0 : w;
    }
    return clamp01(u);
}

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

// The unit step response of a damped spring that starts at rest, `tau` seconds after it is let go: 0 at tau 0, 1 when settled.
// `zeta` under 1 overshoots and rings, 1 is critical, above 1 creeps. Closed form, so it holds for any time with no state.
double spring_step(double tau, double freq_hz, double zeta) {
    if (!(tau > 0)) return 0;
    if (!(freq_hz > 0)) return 1;   // no frequency: a jump (a hand-built track; the validator rejects it)
    const double w0 = kTwoPi * freq_hz;
    if (!(zeta > 0)) return 1 - std::cos(w0 * tau);                        // undamped: rings for ever
    if (std::fabs(zeta - 1.0) < 1e-4) return 1 - std::exp(-w0 * tau) * (1 + w0 * tau);
    if (zeta < 1.0) {
        const double wd = w0 * std::sqrt(1 - zeta * zeta);
        const double a = zeta * w0;
        return 1 - std::exp(-a * tau) * (std::cos(wd * tau) + a / wd * std::sin(wd * tau));
    }
    const double r = std::sqrt(zeta * zeta - 1);
    const double r1 = -w0 * (zeta - r), r2 = -w0 * (zeta + r);
    return 1 - (r2 * std::exp(r1 * tau) - r1 * std::exp(r2 * tau)) / (r2 - r1);
}

float hold_to(double v, float bound) {
    if (!std::isfinite(v)) return 0;
    if (v > bound) return bound;
    if (v < -bound) return -bound;
    return static_cast<float>(v);
}

// A generator track at `u`: its time is u * duration (the lap's time for a loop, the clip's end held for a hold).
void sample_generator(const Track& tr, float u, float duration_ms, const Signals& sg, float out[3]) {
    const float bound = tr.channel == Channel::Pos ? kMaxPos : kMaxRotDeg;
    u = clamp01(u);
    const float t_ms = u * (duration_ms > 0 ? duration_ms : 0.0f);
    const double tau = static_cast<double>(t_ms) / 1000.0;
    switch (tr.source) {
        case Source::Osc: {
            const Osc& o = tr.osc;
            const double env = o.decay > 0 ? std::exp(-static_cast<double>(o.decay) * tau) : 1.0;
            const double w = std::sin(kTwoPi * o.freq_hz * tau + o.phase_deg * kDegToRad);
            for (int a = 0; a < 3; ++a) out[a] = hold_to(o.centre[a] + o.amp[a] * w * env, bound);
            break;
        }
        case Source::Spring: {
            const Spring& sp = tr.spring;
            const double x = spring_step(tau - static_cast<double>(sp.at) * duration_ms / 1000.0, sp.freq_hz, sp.damping);
            for (int a = 0; a < 3; ++a) out[a] = hold_to(sp.from[a] + (sp.to[a] - sp.from[a]) * x, bound);
            break;
        }
        case Source::Expr:
            for (int a = 0; a < 3; ++a) out[a] = hold_to(eval_expr(tr.expr[a], u, t_ms, sg), bound);
            break;
        case Source::Keys:
            out[0] = out[1] = out[2] = 0;
            break;
    }
}

}  // namespace

const char* source_name(Source s) {
    int i = static_cast<int>(s);
    return i >= 0 && i < 4 ? kSourceNames[i] : "?";
}

const char* part_name(Part p) {
    int i = static_cast<int>(p);
    return i >= 0 && i < kPartCount ? kPartNames[i] : "?";
}
bool part_from_name(const std::string& s, Part& out) {
    for (int i = 0; i < kPartCount; ++i)
        if (s == kPartNames[i]) { out = static_cast<Part>(i); return true; }
    // A weapon follows its hand, so its name is the hand's.
    if (s == "weapon_r") { out = Part::HandR; return true; }
    if (s == "weapon_l") { out = Part::HandL; return true; }
    return false;
}

const char* channel_name(Channel c) { return c == Channel::Pos ? "pos" : "rot"; }
bool channel_from_name(const std::string& s, Channel& out) {
    if (s == "pos") { out = Channel::Pos; return true; }
    if (s == "rot") { out = Channel::Rot; return true; }
    return false;
}

const char* ease_name(Ease e) {
    int i = static_cast<int>(e);
    return i >= 0 && i < 5 ? kEaseNames[i] : "?";
}
bool ease_from_name(const std::string& s, Ease& out) {
    for (int i = 0; i < 5; ++i)
        if (s == kEaseNames[i]) { out = static_cast<Ease>(i); return true; }
    return false;
}

float apply_ease(Ease e, float f) {
    f = clamp01(f);
    switch (e) {
        case Ease::Linear: return f;
        case Ease::Smooth: return f * f * (3 - 2 * f);
        case Ease::EaseIn: return f * f;
        case Ease::EaseOut: return 1 - (1 - f) * (1 - f);
        case Ease::Hold: return f >= 1 ? 1.0f : 0.0f;
    }
    return f;
}

const char* hide_group_name(uint32_t bit) {
    for (const auto& h : kHideNames)
        if (h.bit == bit) return h.name;
    return "?";
}
bool hide_group_from_name(const std::string& s, uint32_t& bit) {
    for (const auto& h : kHideNames)
        if (s == h.name) { bit = h.bit; return true; }
    return false;
}

float resolve_duration(const Clip& c, float ability_duration_ms) {
    float d = c.duration_from_ability ? ability_duration_ms : c.duration_ms;
    return d > 0 ? d : 0;
}

float blend_weight(const Clip& c, const Timing& t) {
    float w = 1;
    float e = t.elapsed_ms > 0 ? t.elapsed_ms : 0;
    if (c.blend_in_ms > 0) w *= clamp01(e / c.blend_in_ms);
    float run = t.run_ms > 0 ? t.run_ms : (c.end == End::Hold ? t.duration_ms : 0);
    if (c.blend_out_ms > 0 && run > 0) w *= clamp01((run - e) / c.blend_out_ms);
    return w;
}

void sample_track(const Track& tr, float u, float out[3]) { sample_track(tr, u, 0.0f, Signals(), out); }

void sample_track(const Track& tr, float u, float duration_ms, const Signals& signals, float out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (tr.source != Source::Keys) {
        sample_generator(tr, u, duration_ms, signals, out);
        return;
    }
    const auto& k = tr.keys;
    if (k.empty()) return;
    u = clamp01(u);
    size_t i = 0;   // the first key after u
    while (i < k.size() && k[i].t <= u) ++i;
    if (i == 0) { std::memcpy(out, k[0].v, sizeof(float) * 3); return; }
    if (i == k.size()) { std::memcpy(out, k[i - 1].v, sizeof(float) * 3); return; }
    const Key& a = k[i - 1];
    const Key& b = k[i];
    float span = b.t - a.t;   // > 0: a.t <= u < b.t
    float e = apply_ease(b.ease, span > 0 ? (u - a.t) / span : 1.0f);
    for (int n = 0; n < 3; ++n) out[n] = a.v[n] + (b.v[n] - a.v[n]) * e;
}

Applied apply_clip(const Clip& c, const Timing& t, Pose& pose) {
    Applied r;
    if (!(t.duration_ms > 0) || c.tracks.empty()) return r;
    r.ok = true;
    r.weight = blend_weight(c, t);
    float u = norm_time(c, t);
    for (const Track& tr : c.tracks) {
        if (tr.source == Source::Keys && tr.keys.empty()) continue;
        int pi = static_cast<int>(tr.part);
        if (pi < 0 || pi >= kPartCount) continue;
        float s[3];
        sample_track(tr, u, t.duration_ms, t.signals, s);
        float* vec = tr.channel == Channel::Pos ? pose.parts[pi].pos : pose.parts[pi].rot;
        for (int n = 0; n < 3; ++n) {
            float target = tr.mode == TrackMode::Add ? vec[n] + s[n] : s[n];
            vec[n] += (target - vec[n]) * r.weight;
        }
        (tr.channel == Channel::Pos ? r.pos_parts : r.rot_parts) |= 1u << pi;
    }
    return r;
}

uint32_t clip_expr_uses(const Clip& c) {
    uint32_t uses = 0;
    for (const Track& tr : c.tracks)
        if (tr.source == Source::Expr)
            for (const ExprProgram& p : tr.expr) uses |= p.uses;
    return uses;
}

uint32_t hide_mask(const Clip& c, const Timing& t) {
    if (!(t.duration_ms > 0)) return 0;
    float u = norm_time(c, t);
    uint32_t m = 0;
    for (const Hide& h : c.hides)
        if (u >= h.from && (u < h.to || h.to >= 1)) m |= h.groups;
    return m;
}

std::vector<int> markers_crossed(const Clip& c, float from_ms, float to_ms, float duration_ms) {
    std::vector<int> out;
    if (!(duration_ms > 0) || !(to_ms > from_ms) || c.markers.empty()) return out;
    std::vector<int> order(c.markers.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return c.markers[a].t < c.markers[b].t; });
    double dur = duration_ms;
    long long first = 0, last = 0;
    if (c.end == End::Loop) {
        first = static_cast<long long>(std::floor(std::max(0.0, double(from_ms)) / dur));
        last = static_cast<long long>(std::floor(double(to_ms) / dur));
        if (last - first > 64) last = first + 64;   // a stall of many laps reports the first 64
    }
    for (long long lap = first; lap <= last; ++lap)
        for (int i : order) {
            double at = (double(lap) + c.markers[i].t) * dur;
            if (at > from_ms && at <= to_ms) out.push_back(i);
        }
    return out;
}

float borrowed_mode_time(const Clip& c, float elapsed_ms) {
    if (!c.borrow.present) return 0;
    float e = elapsed_ms > 0 ? elapsed_ms : 0;
    float v = c.borrow.offset_ms + e * c.borrow.speed;
    return v > 0 ? v : 0;
}

}  // namespace kyubu::game::anim
