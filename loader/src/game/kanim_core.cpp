#include "kanim_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace kyubu::kanim {

using namespace kyubu::game::anim;

namespace {

constexpr double kPopEps = 0.01;      // a value this far from 0 at an end of a clip is a pop (deg or voxels)
constexpr int kSegmentSteps = 200;    // points inside one key-to-key segment, for the fastest motion

// A number with at most two decimals and no trailing zeros: "-110", "0.35", "0".
std::string num(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.2f", v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

// A float as TOML text that reads back to the same float: the shortest of 6 to 9 digits that round-trips.
std::string exact(float v) {
    char b[64];
    for (int digits = 6; digits <= 9; ++digits) {
        std::snprintf(b, sizeof b, "%.*g", digits, static_cast<double>(v));
        if (std::strtof(b, nullptr) == v) break;
    }
    return b;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += static_cast<char>(ch); }
        else if (ch < 0x20 || ch == 0x7F) { char b[8]; std::snprintf(b, sizeof b, "\\u%04X", ch); out += b; }
        else out += static_cast<char>(ch);
    }
    return out + "\"";
}

const char* unit(Channel c) { return c == Channel::Pos ? "vox" : "deg"; }
const char* axis(int i) { return i == 0 ? "x" : i == 1 ? "y" : "z"; }

double vlen(const float a[3], const float b[3]) {
    double s = 0;
    for (int i = 0; i < 3; ++i) { double d = static_cast<double>(b[i]) - a[i]; s += d * d; }
    return std::sqrt(s);
}
double vmax_abs(const float a[3]) {
    double m = 0;
    for (int i = 0; i < 3; ++i) m = std::max(m, static_cast<double>(std::fabs(a[i])));
    return m;
}

std::string label(const Track& tr) {
    return std::string(part_name(tr.part)) + " " + channel_name(tr.channel) + " (" + (tr.mode == TrackMode::Add ? "add" : "set") + ")";
}

// The clip's duration in ms for the commands: its own, or the ability's. `assumed` is set when "ability" had no number.
float clip_duration(const Clip& c, const Options& o, bool& assumed) {
    assumed = false;
    if (!c.duration_from_ability) return c.duration_ms;
    if (o.ability_ms > 0) return o.ability_ms;
    assumed = true;
    return kAssumedAbilityMs;
}

std::string ms_of(double u, float dur) { return num(u * dur) + " ms"; }

bool load_or_fail(const std::string& text, const std::string& source, const Options& o, ClipLoad& r, Result& out) {
    r = parse_clip(text, source);
    if (r.ok) return true;
    out.code = 1;
    if (o.json) {
        out.text = errors_to_json(r) + "\n";
    } else {
        for (const ClipError& e : r.errors) out.text += format_error(e) + "\n";
    }
    return false;
}

struct Fastest {
    double speed = 0;     // unit per second
    double u = 0;         // where the fastest point is
    double size = 0;      // the change over the key-to-key segment that holds it
    float t0 = 0, t1 = 0; // that segment, as fractions
    bool found = false;
    bool sampled = false; // a generator: there is no key-to-key segment, the speed is read from samples
};
struct Jump {
    float t = 0;
    double size = 0;
};

// Continuous motion is measured inside each segment, so a jump (a `hold` ease, or two keys on one `t`) does not read as an
// enormous speed. Jumps are listed apart. Speed in a segment is size * slope / (span * duration).
void analyse_track(const Track& tr, float dur, Fastest& fast, std::vector<Jump>& jumps) {
    const auto& k = tr.keys;
    for (size_t i = 1; i < k.size(); ++i) {
        const Key& a = k[i - 1];
        const Key& b = k[i];
        double size = vlen(a.v, b.v);
        if (!(b.t > a.t) || b.ease == Ease::Hold) {
            if (size > kPopEps) jumps.push_back({b.t, size});
            continue;
        }
        double span = static_cast<double>(b.t) - a.t;
        for (int s = 0; s <= kSegmentSteps; ++s) {
            double f = static_cast<double>(s) / kSegmentSteps;
            double speed = size * ease_slope(b.ease, f) / (span * dur / 1000.0);
            if (speed > fast.speed) {
                fast.speed = speed;
                fast.u = a.t + f * span;
                fast.size = size;
                fast.t0 = a.t;
                fast.t1 = b.t;
                fast.found = true;
            }
        }
    }
}

constexpr int kGenSteps = 400;          // samples over a generator track, for its range and fastest motion
constexpr double kBakeKeysPerS = 60.0;  // `bake` writes a key per 1/60 s of a generator, and always the two ends

// A generator has no keys, so its range, its fastest motion and its two ends are read from samples of the evaluator itself.
void analyse_generator(const Track& tr, float dur, const Signals& sg, float lo[3], float hi[3], float first[3], float last[3],
                       Fastest& fast) {
    float prev[3] = {0, 0, 0};
    for (int s = 0; s <= kGenSteps; ++s) {
        const float u = static_cast<float>(s) / kGenSteps;
        float v[3];
        sample_track(tr, u, dur, sg, v);
        for (int a = 0; a < 3; ++a) {
            if (s == 0) { lo[a] = hi[a] = v[a]; first[a] = v[a]; }
            lo[a] = std::min(lo[a], v[a]);
            hi[a] = std::max(hi[a], v[a]);
            last[a] = v[a];
        }
        if (s > 0) {
            const double speed = vlen(prev, v) / (dur / 1000.0 / kGenSteps);
            if (speed > fast.speed) {
                fast.speed = speed;
                fast.u = u;
                fast.found = true;
                fast.sampled = true;
            }
        }
        for (int a = 0; a < 3; ++a) prev[a] = v[a];
    }
}

// The generator in words: its numbers as the file gives them. An `expr` is listed by its names (the text is the file's).
std::string generator_words(const Track& tr) {
    auto v3 = [](const float v[3]) { return num(v[0]) + " " + num(v[1]) + " " + num(v[2]); };
    switch (tr.source) {
        case Source::Osc:
            return "osc, centre " + v3(tr.osc.centre) + ", amp " + v3(tr.osc.amp) + ", " + num(tr.osc.freq_hz) + " Hz, phase " +
                   num(tr.osc.phase_deg) + " deg, decay " + num(tr.osc.decay) + " /s";
        case Source::Spring:
            return "spring, from " + v3(tr.spring.from) + " to " + v3(tr.spring.to) + " at " + num(tr.spring.at) + ", " +
                   num(tr.spring.freq_hz) + " Hz, damping " + num(tr.spring.damping) +
                   (tr.spring.damping < 1 ? " (overshoots and rings)" : " (no overshoot)");
        case Source::Expr: {
            uint32_t uses = 0;
            for (const ExprProgram& p : tr.expr) uses |= p.uses;
            std::string s = "expr, reads";
            const char* names[5] = {"u", "t", "charge", "speed", "rank"};
            bool any = false;
            for (int i = 0; i < 5; ++i)
                if (uses & (1u << i)) { s += std::string(any ? ", " : " ") + names[i]; any = true; }
            return any ? s : "expr, reads no name (a constant)";
        }
        case Source::Keys: break;
    }
    return "keys";
}

// The signals a track reads from the game, as a sentence for `describe`; empty when it reads none.
std::string signal_words(const Track& tr) {
    uint32_t uses = 0;
    for (const ExprProgram& p : tr.expr) uses |= p.uses;
    std::string s;
    if (uses & kUsesCharge) s += (s.empty() ? "" : ", ") + std::string("charge");
    if (uses & kUsesSpeed) s += (s.empty() ? "" : ", ") + std::string("speed");
    if (uses & kUsesRank) s += (s.empty() ? "" : ", ") + std::string("rank");
    return s;
}

}  // namespace

// The slope of apply_ease at f (0..1), by the ease's own formula. The unit test compares it with differences of apply_ease, so a
// change to an easing that is not copied here fails there.
double ease_slope(Ease e, double f) {
    switch (e) {
        case Ease::Linear: return 1;
        case Ease::Smooth: return 6 * f * (1 - f);
        case Ease::EaseIn: return 2 * f;
        case Ease::EaseOut: return 2 * (1 - f);
        case Ease::Hold: return 0;
    }
    return 1;
}

bool parse_times(const std::string& s, std::vector<float>& out) {
    out.clear();
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (item.empty()) return false;
        char* end = nullptr;
        double v = std::strtod(item.c_str(), &end);
        if (end == item.c_str() || *end != '\0' || !(v >= 0 && v <= 1)) return false;
        out.push_back(static_cast<float>(v));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return !out.empty();
}

Result check(const std::string& text, const std::string& source, const Options& o) {
    Result out;
    ClipLoad r;
    if (!load_or_fail(text, source, o, r, out)) return out;
    out.text = o.json ? errors_to_json(r) + "\n" : source + ": ok\n";
    return out;
}

Result describe(const std::string& text, const std::string& source, const Options& o) {
    Result out;
    ClipLoad r;
    if (!load_or_fail(text, source, o, r, out)) return out;
    const Clip& c = r.clip;
    bool assumed = false;
    float dur = clip_duration(c, o, assumed);
    std::string& s = out.text;

    s += "clip: " + source + "\n";
    if (c.duration_from_ability) {
        s += "duration: from the ability";
        s += assumed ? " (not given: assuming " + num(dur) + " ms; pass --ability-ms)\n" : ": " + num(dur) + " ms\n";
    } else {
        s += "duration: " + num(dur) + " ms";
        if (o.ability_ms > 0) {
            if (dur < o.ability_ms)
                s += " against the ability's " + num(o.ability_ms) + " ms: " +
                     (c.end == End::Hold ? "the last pose is held for " + num(o.ability_ms - dur) + " ms"
                                         : "the clip loops " + num(o.ability_ms / dur) + " times");
            else if (dur > o.ability_ms)
                s += " against the ability's " + num(o.ability_ms) + " ms: the last " + num(dur - o.ability_ms) +
                     " ms are never drawn";
            else
                s += " against the ability's " + num(o.ability_ms) + " ms: equal";
        }
        s += "\n";
    }
    s += std::string("end: ") + (c.end == End::Hold ? "hold" : "loop") + "\n";
    s += "blend: in " + num(c.blend_in_ms) + " ms, out " + num(c.blend_out_ms) + " ms\n";

    if (c.borrow.present)
        s += "borrow: mode " + std::to_string(c.borrow.mode) + ", speed " + num(c.borrow.speed) + ", offset " +
             num(c.borrow.offset_ms) + " ms\n";

    s += "tracks: " + std::to_string(c.tracks.size()) + "\n";
    std::vector<std::string> pops;
    for (size_t i = 0; i < c.tracks.size(); ++i) {
        const Track& tr = c.tracks[i];
        const std::string name = label(tr);
        // A keys track is read from its keys; a generator has none, so it is read from samples of the evaluator (the same numbers).
        const bool gen = tr.source != Source::Keys;
        float lo[3], hi[3], gfirst[3], glast[3];
        Fastest fast;
        std::vector<Jump> jumps;
        if (gen) {
            analyse_generator(tr, dur, o.signals, lo, hi, gfirst, glast, fast);
        } else {
            for (int a = 0; a < 3; ++a) {
                lo[a] = hi[a] = tr.keys[0].v[a];
                for (const Key& k : tr.keys) { lo[a] = std::min(lo[a], k.v[a]); hi[a] = std::max(hi[a], k.v[a]); }
            }
            analyse_track(tr, dur, fast, jumps);
        }
        s += "  " + name + ":";
        for (int a = 0; a < 3; ++a) s += std::string(a ? "," : "") + " " + axis(a) + " " + num(lo[a]) + ".." + num(hi[a]);
        s += std::string(" ") + unit(tr.channel) + "\n";
        if (gen) {
            s += "    generator: " + generator_words(tr) + "\n";
            const std::string sig = signal_words(tr);
            if (!sig.empty())
                s += "    signals: " + sig + " (read from the game; shown with charge " + num(o.signals.charge) + ", speed " +
                     num(o.signals.speed) + ", rank " + num(o.signals.rank) + "; pass --charge, --speed, --rank)\n";
        }
        if (fast.found && fast.sampled)
            s += "    fastest: " + num(fast.speed) + " " + unit(tr.channel) + "/s at " + num(fast.u) + " (" + ms_of(fast.u, dur) + ")\n";
        else if (fast.found)
            s += "    fastest: " + num(fast.speed) + " " + unit(tr.channel) + "/s at " + num(fast.u) + " (" + ms_of(fast.u, dur) +
                 "), moving " + num(fast.size) + " " + unit(tr.channel) + " from " + num(fast.t0) + " to " + num(fast.t1) + "\n";
        else
            s += "    fastest: none (no continuous motion)\n";
        for (const Jump& j : jumps)
            s += "    jump: " + num(j.size) + " " + unit(tr.channel) + " at " + num(j.t) + " (" + ms_of(j.t, dur) + ")\n";

        // A pop: the pose jumps when the clip starts or when the run ends, because the game then draws its own pose.
        const float* first = gen ? gfirst : tr.keys.front().v;
        const float* last = gen ? glast : tr.keys.back().v;
        if (c.blend_in_ms <= 0) {
            if (tr.mode == TrackMode::Add && vmax_abs(first) > kPopEps)
                pops.push_back("start: " + name + " begins " + num(vmax_abs(first)) + " " + unit(tr.channel) +
                               " off the base pose and blend_in is 0");
            else if (tr.mode == TrackMode::Set)
                pops.push_back("start: " + name + " replaces the vector at once: it pops unless the base pose equals the first key");
        }
        if (c.blend_out_ms <= 0) {
            if (c.end == End::Hold && tr.mode == TrackMode::Add && vmax_abs(last) > kPopEps)
                pops.push_back("end: " + name + " holds " + num(vmax_abs(last)) + " " + unit(tr.channel) +
                               " off the base pose when the run ends and blend_out is 0");
            else if (c.end == End::Hold && tr.mode == TrackMode::Set)
                pops.push_back("end: " + name + " replaces the vector until the run ends: it pops back to the game's pose");
            else if (c.end == End::Loop && (vmax_abs(first) > kPopEps || vmax_abs(last) > kPopEps || vlen(first, last) > kPopEps))
                pops.push_back("end: " + name + " loops and blend_out is 0: the pose where the run ends is wherever the loop is");
        }
        if (c.end == End::Loop && vlen(first, last) > kPopEps)
            pops.push_back("seam: " + name + " jumps " + num(vlen(first, last)) + " " + unit(tr.channel) + " at each lap");
    }
    if (pops.empty()) {
        s += "pops: none\n";
    } else {
        s += "pops:\n";
        for (const std::string& p : pops) s += "  " + p + "\n";
    }

    if (c.hides.empty()) {
        s += "hide: none\n";
    } else {
        s += "hide:\n";
        for (const Hide& h : c.hides) {
            std::string groups;
            for (int b = 0; b < 32; ++b)
                if (h.groups & (1u << b)) groups += std::string(groups.empty() ? "" : ", ") + hide_group_name(1u << b);
            s += "  " + groups + " from " + num(h.from) + " to " + num(h.to) + " (" + ms_of(h.from, dur) + " to " +
                 ms_of(h.to, dur) + ")\n";
        }
    }
    if (c.markers.empty()) {
        s += "markers: none\n";
    } else {
        s += "markers:\n";
        for (const Marker& m : c.markers) s += "  " + m.name + " at " + num(m.t) + " (" + ms_of(m.t, dur) + ")\n";
    }
    return out;
}

Result sample(const std::string& text, const std::string& source, const Options& o) {
    Result out;
    ClipLoad r;
    if (!load_or_fail(text, source, o, r, out)) return out;
    const Clip& c = r.clip;
    bool assumed = false;
    float dur = clip_duration(c, o, assumed);
    std::string& s = out.text;
    s += "clip: " + source + ", duration " + num(dur) + " ms";
    if (assumed) s += " (assumed; pass --ability-ms)";
    s += ", base pose all zeros";
    for (const Track& tr : c.tracks)
        if (tr.source == Source::Expr) {
            s += ", charge " + num(o.signals.charge) + ", speed " + num(o.signals.speed) + ", rank " + num(o.signals.rank);
            break;
        }
    s += "\n";
    std::vector<float> times = o.times.empty() ? std::vector<float>{0.0f, 0.5f, 1.0f} : o.times;
    for (float u : times) {
        Pose pose;
        Timing t;
        t.elapsed_ms = u * dur;
        t.duration_ms = dur;
        t.run_ms = dur;
        t.signals = o.signals;
        Applied ap = apply_clip(c, t, pose);
        s += "t " + num(u) + " (" + ms_of(u, dur) + "), weight " + num(ap.weight) + "\n";
        if (!ap.ok) { s += "  nothing to draw (no tracks)\n"; continue; }
        for (int p = 0; p < kPartCount; ++p) {
            const PartPose& pp = pose.parts[p];
            std::string line;
            if (ap.pos_parts & (1u << p)) line += " pos " + num(pp.pos[0]) + " " + num(pp.pos[1]) + " " + num(pp.pos[2]);
            if (ap.rot_parts & (1u << p)) line += " rot " + num(pp.rot[0]) + " " + num(pp.rot[1]) + " " + num(pp.rot[2]);
            if (!line.empty()) s += std::string("  ") + part_name(static_cast<Part>(p)) + ":" + line + "\n";
        }
    }
    return out;
}

Result bake(const std::string& text, const std::string& source, const Options& o) {
    Result out;
    ClipLoad r;
    if (!load_or_fail(text, source, o, r, out)) return out;
    const Clip& c = r.clip;
    std::string& s = out.text;
    s += "# baked by kanim from " + source + "\n";
    s += c.duration_from_ability ? "duration = \"ability\"\n" : "duration = " + exact(c.duration_ms) + "\n";
    s += std::string("end = ") + (c.end == End::Hold ? "\"hold\"" : "\"loop\"") + "\n";
    s += "blend_in = " + exact(c.blend_in_ms) + "\n";
    s += "blend_out = " + exact(c.blend_out_ms) + "\n";
    if (c.borrow.present) {
        s += "\n[borrow]\nmode = " + std::to_string(c.borrow.mode) + "\nspeed = " + exact(c.borrow.speed) +
             "\noffset_ms = " + exact(c.borrow.offset_ms) + "\n";
    }
    std::string baked_note;   // one line after the tracks: what the generators were baked with
    for (const Track& tr : c.tracks) {
        s += std::string("\n[[track]]\npart = \"") + part_name(tr.part) + "\"\nchannel = \"" + channel_name(tr.channel) +
             "\"\nmode = \"" + (tr.mode == TrackMode::Add ? "add" : "set") + "\"\nkeys = [\n";
        if (tr.source == Source::Keys) {
            for (const Key& k : tr.keys)
                s += "  { t = " + exact(k.t) + ", v = [" + exact(k.v[0]) + ", " + exact(k.v[1]) + ", " + exact(k.v[2]) +
                     "], ease = \"" + ease_name(k.ease) + "\" },\n";
        } else {
            // A generator becomes plain keys: one per 1/60 s of the clip (at most kMaxKeysPerTrack), the two ends included, joined
            // linearly. It is time-based, so the clip's duration matters; a signal is frozen at --charge, --speed and --rank.
            bool assumed = false;
            const float dur = clip_duration(c, o, assumed);
            int n = static_cast<int>(std::ceil(static_cast<double>(dur) / 1000.0 * kBakeKeysPerS));
            n = std::max(2, std::min(n, kMaxKeysPerTrack - 1));
            for (int i = 0; i <= n; ++i) {
                const float u = i == n ? 1.0f : static_cast<float>(i) / static_cast<float>(n);
                float v[3];
                sample_track(tr, u, dur, o.signals, v);
                s += "  { t = " + exact(u) + ", v = [" + exact(v[0]) + ", " + exact(v[1]) + ", " + exact(v[2]) + "] },\n";
            }
            baked_note = "# generators baked over " + num(dur) + " ms" + (assumed ? " (assumed; pass --ability-ms)" : "") +
                         " with charge " + num(o.signals.charge) + ", speed " + num(o.signals.speed) + ", rank " + num(o.signals.rank) + "\n";
        }
        s += "]\n";
    }
    s += baked_note;
    for (const Hide& h : c.hides) {
        s += "\n[[hide]]\nfrom = " + exact(h.from) + "\nto = " + exact(h.to) + "\nparts = [";
        bool firstg = true;
        for (int b = 0; b < 32; ++b)
            if (h.groups & (1u << b)) {
                s += std::string(firstg ? "" : ", ") + "\"" + hide_group_name(1u << b) + "\"";
                firstg = false;
            }
        s += "]\n";
    }
    for (const Marker& m : c.markers) s += "\n[[marker]]\nt = " + exact(m.t) + "\nname = " + quote(m.name) + "\n";
    return out;
}

}  // namespace kyubu::kanim
