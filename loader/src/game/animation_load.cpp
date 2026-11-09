// The TOML loader and the validator of ability clips: see animation_load.h. No game.
#include "animation_load.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string_view>

#include "json.hpp"
#include "toml_config.h"

namespace kyubu::game::anim {
namespace {

constexpr size_t kMaxQuoted = 40;   // a name from the file is cut to this many bytes in a message or a path

struct Ctx {
    const std::string& source;
    std::vector<ClipError> errors;

    void add(int line, std::string path, const char* code, std::string message, std::string hint, int column = 0) {
        if (static_cast<int>(errors.size()) >= kMaxErrors) return;
        errors.push_back({source, line, std::move(path), code, std::move(message), std::move(hint), column});
    }
};

int line_of(const toml::node& n) { return static_cast<int>(n.source().begin.line); }

const char* kind(const toml::node& n) {
    switch (n.type()) {
        case toml::node_type::boolean: return "a boolean";
        case toml::node_type::integer: return "an integer";
        case toml::node_type::floating_point: return "a float";
        case toml::node_type::string: return "a string";
        case toml::node_type::array: return "an array";
        case toml::node_type::table: return "a table";
        default: return "a date or a time";
    }
}

std::string num(double d) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", d);
    return b;
}

// Text from the file, cut at a UTF-8 boundary when it is long.
std::string cut(std::string_view s) {
    if (s.size() <= kMaxQuoted) return std::string(s);
    size_t n = kMaxQuoted;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return std::string(s.substr(0, n)) + "...";
}
std::string in_quotes(std::string_view s) { return "\"" + cut(s) + "\""; }

std::string join(const std::string& base, std::string_view key) {
    return base.empty() ? std::string(key) : base + "." + std::string(key);
}
std::string at(const std::string& base, size_t i) { return base + "[" + std::to_string(i) + "]"; }

// The last step of a path, in backticks: the subject of a message. "track[0].keys[1].t" gives "`t`".
std::string field_name(const std::string& path) {
    size_t dot = path.rfind('.');
    return "`" + (dot == std::string::npos ? path : path.substr(dot + 1)) + "`";
}

// The names a field takes, for a message. They come from the evaluator's own tables, so the two cannot drift.
std::string part_list() {
    std::string s;
    for (int i = 0; i < kPartCount; ++i) s += (i ? ", " : "") + std::string(part_name(static_cast<Part>(i)));
    return s;
}
std::string ease_list() {
    std::string s;
    for (int i = 0; i <= static_cast<int>(Ease::Hold); ++i) s += (i ? ", " : "") + std::string(ease_name(static_cast<Ease>(i)));
    return s;
}
std::string group_list() {
    std::string s;
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        std::string_view name = hide_group_name(bit);
        if (name != "?") s += (s.empty() ? "" : ", ") + std::string(name);
    }
    return s;
}
std::string channel_list() { return std::string(channel_name(Channel::Pos)) + ", " + channel_name(Channel::Rot); }

// Every key of a table must be one the format names.
void check_keys(Ctx& c, const toml::table& t, const std::string& base, std::initializer_list<std::string_view> known) {
    for (auto&& kv : t) {
        std::string_view key = kv.first.str();
        if (std::find(known.begin(), known.end(), key) != known.end()) continue;
        std::string list;
        for (std::string_view k : known) list += (list.empty() ? "" : ", ") + std::string(k);
        c.add(static_cast<int>(kv.first.source().begin.line), join(base, cut(key)), "unknown_key",
              "unknown key " + in_quotes(key) + "; the keys here are " + list, "Remove the key, or correct its spelling.");
    }
}

// The node of `key`. A missing one is an error when `example` is given (the field is required); `line` is the table's.
const toml::node* field(Ctx& c, const toml::table& t, int line, const std::string& base, const char* key,
                        const char* example = nullptr) {
    const toml::node* n = t.get(key);
    if (!n && example)
        c.add(line, join(base, key), "missing_field", "`" + std::string(key) + "` is missing",
              "Add `" + std::string(key) + " = " + example + "`.");
    return n;
}

// A finite number from `lo` to `hi` (above `lo` when `lo_open`), integer or float. `out` is set only when it is one.
bool number(Ctx& c, const toml::node& n, const std::string& path, double lo, double hi, bool lo_open, const std::string& hint,
            float& out) {
    if (!n.is_number()) {
        c.add(line_of(n), path, "bad_type", field_name(path) + " must be a number, not " + kind(n), hint);
        return false;
    }
    double d = n.is_integer() ? static_cast<double>(n.as_integer()->get()) : n.as_floating_point()->get();
    if (!std::isfinite(d)) {
        c.add(line_of(n), path, "not_finite", field_name(path) + " must be a finite number, not nan or inf", hint);
        return false;
    }
    if (d < lo || d > hi || (lo_open && d <= lo)) {
        c.add(line_of(n), path, "out_of_range",
              field_name(path) + " is " + num(d) + "; it must be " + (lo_open ? "above " : "from ") + num(lo) +
                  (lo_open ? " and at most " : " to ") + num(hi),
              hint);
        return false;
    }
    out = static_cast<float>(d);
    return true;
}

// A string that must be one of a set of names. `find` maps the name; `list` is the set, for the message.
template <class Find>
bool named(Ctx& c, const toml::node& n, const std::string& path, const char* code, const char* what, const std::string& list,
           Find find) {
    std::string hint = "Change " + field_name(path) + " to one of: " + list + ".";
    if (!n.is_string()) {
        c.add(line_of(n), path, "bad_type", field_name(path) + " must be a string, not " + kind(n), hint);
        return false;
    }
    const std::string& s = n.as_string()->get();
    if (find(s)) return true;
    c.add(line_of(n), path, code, "unknown " + std::string(what) + " " + in_quotes(s) + "; the " + what + "s are " + list, hint);
    return false;
}

// The tables of `[[name]]`, at most `cap` of them. Null, with an error, for anything else.
const toml::array* rows(Ctx& c, const toml::node& n, const char* name, int cap, const char* too_many) {
    const toml::array* a = n.as_array();
    if (!a || !(a->empty() || a->is_array_of_tables())) {
        c.add(line_of(n), name, "bad_type",
              "`" + std::string(name) + "` must be an array of tables, not " + (a ? "an array of other values" : kind(n)),
              "Write each one under its own `[[" + std::string(name) + "]]` header.");
        return nullptr;
    }
    if (a->size() > static_cast<size_t>(cap)) {
        c.add(line_of(n), name, too_many,
              "there are " + std::to_string(a->size()) + " `[[" + name + "]]` tables; the most is " + std::to_string(cap),
              "Remove `[[" + std::string(name) + "]]` tables until " + std::to_string(cap) + " or fewer are left.");
        return nullptr;
    }
    return a;
}

// One key of a track. True when its `t` is valid, so the caller can check the order.
bool read_key(Ctx& c, const toml::table& t, const std::string& base, double bound, const std::string& v_hint, Key& k) {
    k.line = line_of(t);
    check_keys(c, t, base, {"t", "v", "ease"});
    bool t_ok = false;
    if (const toml::node* n = field(c, t, k.line, base, "t", "0.5"))
        t_ok = number(c, *n, join(base, "t"), 0, 1, false,
                      "Give `t` as a fraction of the clip's duration: 0 is the start, 1 is the end.", k.t);
    if (const toml::node* n = field(c, t, k.line, base, "v", "[0, 0, 0]")) {
        std::string path = join(base, "v");
        const toml::array* a = n->as_array();
        if (!a)
            c.add(line_of(*n), path, "bad_type", std::string("`v` must be an array of three numbers, not ") + kind(*n),
                  "Write `v = [x, y, z]`.");
        else if (a->size() != 3)
            c.add(line_of(*n), path, "bad_length",
                  "`v` has " + std::to_string(a->size()) + " values; it must have three (x, y, z)", "Write `v = [x, y, z]`.");
        else
            for (size_t i = 0; i < 3; ++i) number(c, (*a)[i], at(path, i), -bound, bound, false, v_hint, k.v[i]);
    }
    if (const toml::node* n = field(c, t, k.line, base, "ease"))
        named(c, *n, join(base, "ease"), "unknown_ease", "ease", ease_list(),
              [&](const std::string& s) { return ease_from_name(s, k.ease); });
    return t_ok;
}

// Three numbers `[x, y, z]`, each from -bound to bound. False when the node is not that (the errors are listed). `out` is set whole or
// not at all.
bool read_vec3(Ctx& c, const toml::node& n, const std::string& path, double bound, const std::string& hint, float out[3]) {
    const toml::array* a = n.as_array();
    if (!a) {
        c.add(line_of(n), path, "bad_type", field_name(path) + " must be an array of three numbers, not " + kind(n),
              "Write " + field_name(path) + " as [x, y, z].");
        return false;
    }
    if (a->size() != 3) {
        c.add(line_of(n), path, "bad_length",
              field_name(path) + " has " + std::to_string(a->size()) + " values; it must have three (x, y, z)",
              "Write " + field_name(path) + " as [x, y, z].");
        return false;
    }
    float v[3] = {0, 0, 0};
    bool ok = true;
    for (size_t i = 0; i < 3; ++i) ok = number(c, (*a)[i], at(path, i), -bound, bound, false, hint, v[i]) && ok;
    if (ok)
        for (int i = 0; i < 3; ++i) out[i] = v[i];
    return ok;
}

// The generators of a track (design D11). Each is one inline table (`expr`: three strings), so the file stays data:
// there is no code in it at any trust level.
void read_osc(Ctx& c, const toml::node& node, const std::string& path, double bound, const std::string& v_hint, Osc& o) {
    const toml::table* t = node.as_table();
    if (!t) {
        c.add(line_of(node), path, "bad_type", "`osc` must be a table, not " + std::string(kind(node)),
              "Write `osc = { amp = [5, 0, 0], freq = 3, decay = 4 }`.");
        return;
    }
    const int line = line_of(*t);
    check_keys(c, *t, path, {"centre", "amp", "freq", "phase", "decay"});
    if (const toml::node* n = t->get("centre")) read_vec3(c, *n, join(path, "centre"), bound, v_hint, o.centre);
    if (const toml::node* n = field(c, *t, line, path, "amp", "[5, 0, 0]")) read_vec3(c, *n, join(path, "amp"), bound, v_hint, o.amp);
    if (const toml::node* n = field(c, *t, line, path, "freq", "3"))
        number(c, *n, join(path, "freq"), 0, kMaxOscHz, true,
               "`freq` is cycles per second, above 0 and at most " + num(kMaxOscHz) + ".", o.freq_hz);
    if (const toml::node* n = t->get("phase"))
        number(c, *n, join(path, "phase"), -360, 360, false,
               "`phase` is degrees of the cycle at the start: 0 starts at the centre going up, 90 at the top.", o.phase_deg);
    if (const toml::node* n = t->get("decay"))
        number(c, *n, join(path, "decay"), 0, kMaxDecayPerS, false,
               "`decay` is 1/s: 0 never settles, 4 loses most of the swing in about half a second.", o.decay);
}

void read_spring(Ctx& c, const toml::node& node, const std::string& path, double bound, const std::string& v_hint, Spring& sp) {
    const toml::table* t = node.as_table();
    if (!t) {
        c.add(line_of(node), path, "bad_type", "`spring` must be a table, not " + std::string(kind(node)),
              "Write `spring = { to = [0, 0, -2], at = 0.5, freq = 3, damping = 0.3 }`.");
        return;
    }
    const int line = line_of(*t);
    check_keys(c, *t, path, {"from", "to", "at", "freq", "damping"});
    if (const toml::node* n = t->get("from")) read_vec3(c, *n, join(path, "from"), bound, v_hint, sp.from);
    if (const toml::node* n = field(c, *t, line, path, "to", "[0, 0, -2]")) read_vec3(c, *n, join(path, "to"), bound, v_hint, sp.to);
    if (const toml::node* n = t->get("at"))
        number(c, *n, join(path, "at"), 0, 1, false,
               "`at` is where the spring is let go, as a fraction of the clip's duration: 0 is the start.", sp.at);
    if (const toml::node* n = field(c, *t, line, path, "freq", "3"))
        number(c, *n, join(path, "freq"), 0, kMaxOscHz, true,
               "`freq` is the spring's own cycles per second, above 0 and at most " + num(kMaxOscHz) + ".", sp.freq_hz);
    if (const toml::node* n = t->get("damping"))
        number(c, *n, join(path, "damping"), 0, kMaxDampingRatio, true,
               "`damping` is the damping ratio, above 0 and at most " + num(kMaxDampingRatio) +
                   ": under 1 overshoots and rings, 1 settles fastest without overshoot.",
               sp.damping);
}

void read_expr(Ctx& c, const toml::node& node, const std::string& path, Track& tr) {
    const toml::array* a = node.as_array();
    if (!a) {
        c.add(line_of(node), path, "bad_type", "`expr` must be an array of three strings, not " + std::string(kind(node)),
              R"(Write `expr = ["sin(t * 0.5) * 4", "0", "0"]`: one expression for each of x, y and z.)");
        return;
    }
    if (a->size() != 3) {
        c.add(line_of(node), path, "bad_length", "`expr` has " + std::to_string(a->size()) + " values; it must have three (x, y, z)",
              R"(Write one expression for each axis; use "0" for an axis that does not move.)");
        return;
    }
    for (size_t i = 0; i < 3; ++i) {
        const toml::node& e = (*a)[i];
        const std::string ep = at(path, i);
        if (!e.is_string()) {
            c.add(line_of(e), ep, "bad_type", "an expression must be a string, not " + std::string(kind(e)),
                  R"(Write the expression in quotes, such as "sin(t * 0.5) * 4".)");
            continue;
        }
        ExprError err;
        if (!compile_expr(e.as_string()->get(), tr.expr[i], err))
            c.add(line_of(e), ep, err.code.c_str(), err.message, err.hint, err.column);
    }
}

void read_track(Ctx& c, const toml::table& t, const std::string& base, Track& tr) {
    tr.line = line_of(t);
    check_keys(c, t, base, {"part", "channel", "mode", "keys", "osc", "spring", "expr"});
    if (const toml::node* n = field(c, t, tr.line, base, "part", "\"weapon_r\""))
        named(c, *n, join(base, "part"), "unknown_part", "part", part_list(),
              [&](const std::string& s) { return part_from_name(s, tr.part); });
    // The channel comes before the keys: it sets the bound of `v`. An unknown channel leaves `v` without one.
    double bound = HUGE_VAL;
    std::string v_hint = "Write each axis of `v` as a plain number.";
    if (const toml::node* n = field(c, t, tr.line, base, "channel", "\"rot\"")) {
        if (named(c, *n, join(base, "channel"), "unknown_channel", "channel", channel_list(),
                  [&](const std::string& s) { return channel_from_name(s, tr.channel); })) {
            bool pos = tr.channel == Channel::Pos;
            bound = pos ? kMaxPos : kMaxRotDeg;
            v_hint = std::string(pos ? "A `pos` value is in voxels" : "A `rot` value is in degrees") + "; keep each axis from " +
                     num(-bound) + " to " + num(bound) + ".";
        }
    }
    if (const toml::node* n = field(c, t, tr.line, base, "mode"))
        named(c, *n, join(base, "mode"), "unknown_mode", "mode", "add, set", [&](const std::string& s) {
            if (s == "add") tr.mode = TrackMode::Add;
            else if (s == "set") tr.mode = TrackMode::Set;
            else return false;
            return true;
        });
    // One source: `keys`, or one generator (design D11). A second one is an error at its own line; the first is still read.
    static const char* const kSources[4] = {"keys", "osc", "spring", "expr"};
    const toml::node* src[4] = {t.get("keys"), t.get("osc"), t.get("spring"), t.get("expr")};
    int first = -1;
    for (int i = 0; i < 4; ++i) {
        if (!src[i]) continue;
        if (first < 0) {
            first = i;
            continue;
        }
        c.add(line_of(*src[i]), join(base, kSources[i]), "two_sources",
              std::string("a track has one source, `") + kSources[first] + "` or `" + kSources[i] + "`, not both",
              "Keep one. To put a generator over keyed motion, make a second track of the same part and channel: tracks stack.");
    }
    if (first > 0) {
        tr.source = static_cast<Source>(first);
        const std::string gpath = join(base, kSources[first]);
        if (first == 1) read_osc(c, *src[1], gpath, bound, v_hint, tr.osc);
        else if (first == 2) read_spring(c, *src[2], gpath, bound, v_hint, tr.spring);
        else read_expr(c, *src[3], gpath, tr);
        return;
    }
    const toml::node* n = field(c, t, tr.line, base, "keys", "[{ t = 0, v = [0, 0, 0] }]");
    if (!n) return;
    std::string path = join(base, "keys");
    const toml::array* a = n->as_array();
    if (!a) {
        c.add(line_of(*n), path, "bad_type", std::string("`keys` must be an array of keys, not ") + kind(*n),
              "Write `keys = [{ t = 0, v = [0, 0, 0] }]`.");
    } else if (a->empty()) {
        c.add(line_of(*n), path, "no_keys", "`keys` is empty; a track needs one key or more",
              "Add a key, such as `{ t = 0, v = [0, 0, 0] }`, or remove the track.");
    } else if (a->size() > static_cast<size_t>(kMaxKeysPerTrack)) {
        c.add(line_of(*n), path, "too_many_keys",
              "`keys` has " + std::to_string(a->size()) + " keys; the most is " + std::to_string(kMaxKeysPerTrack),
              "Remove keys, or split the motion over two clips.");
    } else {
        bool have_prev = false;
        float prev = 0;
        for (size_t i = 0; i < a->size(); ++i) {
            const toml::node& kn = (*a)[i];
            const toml::table* kt = kn.as_table();
            if (!kt) {
                c.add(line_of(kn), at(path, i), "bad_type", std::string("a key must be a table { t, v, ease }, not ") + kind(kn),
                      "Write the key as `{ t = 0, v = [0, 0, 0] }`.");
                continue;
            }
            Key k;
            if (read_key(c, *kt, at(path, i), bound, v_hint, k)) {
                if (have_prev && k.t < prev)
                    c.add(k.line, join(at(path, i), "t"), "keys_out_of_order",
                          "`t` is " + num(k.t) + ", below the key before it (" + num(prev) + ")",
                          "Sort the keys by `t`, lowest first. Two keys may share a `t`.");
                prev = k.t;
                have_prev = true;
            }
            tr.keys.push_back(k);
        }
    }
}

void read_hide(Ctx& c, const toml::table& t, const std::string& base, Hide& h) {
    h.line = line_of(t);
    check_keys(c, t, base, {"from", "to", "parts"});
    const std::string hint = "Give the time as a fraction of the clip's duration, from 0 to 1.";
    const toml::node* from = field(c, t, h.line, base, "from", "0");
    const toml::node* to = field(c, t, h.line, base, "to", "1");
    bool from_ok = from && number(c, *from, join(base, "from"), 0, 1, false, hint, h.from);
    bool to_ok = to && number(c, *to, join(base, "to"), 0, 1, false, hint, h.to);
    if (from_ok && to_ok && h.to <= h.from)
        c.add(line_of(*to), join(base, "to"), "empty_interval",
              "`to` is " + num(h.to) + ", not above `from` (" + num(h.from) + ")", "Give `to` a value above `from`.");
    const toml::node* n = field(c, t, h.line, base, "parts", "[\"head\"]");
    if (!n) return;
    std::string path = join(base, "parts");
    const toml::array* a = n->as_array();
    if (!a) {
        c.add(line_of(*n), path, "bad_type", std::string("`parts` must be an array of group names, not ") + kind(*n),
              "Write `parts = [\"head\"]`; the groups are " + group_list() + ".");
    } else if (a->empty()) {
        c.add(line_of(*n), path, "no_parts", "`parts` is empty; a hide interval needs one group or more",
              "Name a group: " + group_list() + ".");
    } else {
        for (size_t i = 0; i < a->size(); ++i) {
            uint32_t bit = 0;
            if (named(c, (*a)[i], at(path, i), "unknown_group", "group", group_list(),
                      [&](const std::string& s) { return hide_group_from_name(s, bit); }))
                h.groups |= bit;
        }
    }
}

void read_marker(Ctx& c, const toml::table& t, const std::string& base, Marker& m) {
    m.line = line_of(t);
    check_keys(c, t, base, {"t", "name"});
    if (const toml::node* n = field(c, t, m.line, base, "t", "0.5"))
        number(c, *n, join(base, "t"), 0, 1, false, "Give `t` as a fraction of the clip's duration: 0 is the start, 1 is the end.",
               m.t);
    const toml::node* n = field(c, t, m.line, base, "name", "\"hit\"");
    if (!n) return;
    const std::string hint = "Give the marker a short name, such as \"hit\".";
    if (!n->is_string()) {
        c.add(line_of(*n), join(base, "name"), "bad_type", std::string("`name` must be a string, not ") + kind(*n), hint);
        return;
    }
    const std::string& s = n->as_string()->get();
    if (s.empty())
        c.add(line_of(*n), join(base, "name"), "bad_name", "`name` is empty", hint);
    else if (s.size() > static_cast<size_t>(kMaxMarkerName))
        c.add(line_of(*n), join(base, "name"), "bad_name",
              "`name` has " + std::to_string(s.size()) + " bytes; the most is " + std::to_string(kMaxMarkerName), hint);
    else
        m.name = s;
}

void read_borrow(Ctx& c, const toml::node& node, Borrow& b) {
    const toml::table* t = node.as_table();
    if (!t) {
        c.add(line_of(node), "borrow", "bad_type", std::string("`borrow` must be a table, not ") + kind(node),
              "Write a `[borrow]` header, and `mode = <vanilla mode id>` under it.");
        return;
    }
    int line = line_of(*t);
    check_keys(c, *t, "borrow", {"mode", "speed", "offset_ms"});
    const std::string mode_hint = "Give a vanilla mode id, a whole number from 0 to " + std::to_string(kMaxBorrowMode) + ".";
    if (const toml::node* n = field(c, *t, line, "borrow", "mode", "<vanilla mode id>")) {
        if (!n->is_integer()) {
            c.add(line_of(*n), "borrow.mode", "bad_type", std::string("`mode` must be a whole number, not ") + kind(*n), mode_hint);
        } else {
            int64_t v = n->as_integer()->get();
            if (v < 0 || v > static_cast<int64_t>(kMaxBorrowMode))
                c.add(line_of(*n), "borrow.mode", "out_of_range",
                      "`mode` is " + std::to_string(v) + "; it must be from 0 to " + std::to_string(kMaxBorrowMode), mode_hint);
            else
                b.mode = static_cast<uint32_t>(v);
        }
    }
    if (const toml::node* n = field(c, *t, line, "borrow", "speed"))
        number(c, *n, "borrow.speed", -kMaxBorrowSpeed, kMaxBorrowSpeed, false,
               "`speed` multiplies the elapsed time: 1 is the game's own speed, 0 holds one frame.", b.speed);
    if (const toml::node* n = field(c, *t, line, "borrow", "offset_ms"))
        number(c, *n, "borrow.offset_ms", -kMaxBorrowOffsetMs, kMaxBorrowOffsetMs, false,
               "`offset_ms` is where the borrowed mode's time starts, in milliseconds.", b.offset_ms);
    b.present = true;
}

void read_clip(Ctx& c, const toml::table& t, Clip& clip) {
    check_keys(c, t, "", {"duration", "end", "blend_in", "blend_out", "track", "hide", "marker", "borrow"});
    const std::string duration_hint = "Write the duration in milliseconds, above 0 and at most " + num(kMaxDurationMs) +
                                      ", or \"ability\" to follow the bound ability.";
    if (const toml::node* n = field(c, t, 0, "", "duration", "500")) {
        if (n->is_string()) {
            const std::string& s = n->as_string()->get();
            if (s == "ability")
                clip.duration_from_ability = true;
            else
                c.add(line_of(*n), "duration", "unknown_duration",
                      "`duration` is " + in_quotes(s) + "; the only word it takes is \"ability\"", duration_hint);
        } else if (n->is_number()) {
            number(c, *n, "duration", 0, kMaxDurationMs, true, duration_hint, clip.duration_ms);
        } else {
            c.add(line_of(*n), "duration", "bad_type",
                  std::string("`duration` must be a number of milliseconds or \"ability\", not ") + kind(*n), duration_hint);
        }
    }
    if (const toml::node* n = field(c, t, 0, "", "end"))
        named(c, *n, "end", "unknown_end", "end", "hold, loop", [&](const std::string& s) {
            if (s == "hold") clip.end = End::Hold;
            else if (s == "loop") clip.end = End::Loop;
            else return false;
            return true;
        });
    const std::string blend_hint = "Write the ramp in milliseconds, from 0 to " + num(kMaxBlendMs) + ".";
    if (const toml::node* n = field(c, t, 0, "", "blend_in"))
        number(c, *n, "blend_in", 0, kMaxBlendMs, false, blend_hint, clip.blend_in_ms);
    if (const toml::node* n = field(c, t, 0, "", "blend_out"))
        number(c, *n, "blend_out", 0, kMaxBlendMs, false, blend_hint, clip.blend_out_ms);

    if (const toml::node* n = field(c, t, 0, "", "track"))
        if (const toml::array* a = rows(c, *n, "track", kMaxTracks, "too_many_tracks"))
            for (size_t i = 0; i < a->size(); ++i)
                read_track(c, *(*a)[i].as_table(), at("track", i), clip.tracks.emplace_back());
    if (const toml::node* n = field(c, t, 0, "", "hide"))
        if (const toml::array* a = rows(c, *n, "hide", kMaxHides, "too_many_hides"))
            for (size_t i = 0; i < a->size(); ++i)
                read_hide(c, *(*a)[i].as_table(), at("hide", i), clip.hides.emplace_back());
    if (const toml::node* n = field(c, t, 0, "", "marker"))
        if (const toml::array* a = rows(c, *n, "marker", kMaxMarkers, "too_many_markers"))
            for (size_t i = 0; i < a->size(); ++i)
                read_marker(c, *(*a)[i].as_table(), at("marker", i), clip.markers.emplace_back());
    if (const toml::node* n = field(c, t, 0, "", "borrow")) read_borrow(c, *n, clip.borrow);
}

}  // namespace

ClipLoad parse_clip(const std::string& text, const std::string& source) {
    ClipLoad r;
    Ctx c{source, {}};
    if (text.size() > kMaxTextBytes) {
        c.add(0, "", "too_large",
              "the text has " + std::to_string(text.size()) + " bytes; the most is " + std::to_string(kMaxTextBytes),
              "Remove keys or tracks; a clip is one short motion.");
    } else {
        auto res = toml::parse(text, source);
        if (!res)
            c.add(static_cast<int>(res.error().source().begin.line), "", "toml_syntax", std::string(res.error().description()),
                  "Correct the TOML at this line; a clip is a plain TOML file.");
        else
            read_clip(c, res.table(), r.clip);
    }
    if (c.errors.empty()) {
        r.ok = true;
        return r;
    }
    // Tables are read field by field, not top to bottom: put the errors in the order of the file.
    std::stable_sort(c.errors.begin(), c.errors.end(), [](const ClipError& a, const ClipError& b) { return a.line < b.line; });
    r.clip = Clip{};
    r.errors = std::move(c.errors);
    return r;
}

std::string format_error(const ClipError& e) {
    std::string s = e.source;
    if (e.line > 0) s += ":" + std::to_string(e.line);
    s += ": ";
    if (!e.path.empty()) s += e.path + ": ";
    if (e.column > 0) s += "column " + std::to_string(e.column) + ": ";
    return s + e.message;
}

std::string errors_to_json(const ClipLoad& r) {
    using json = nlohmann::ordered_json;
    json j;
    j["ok"] = r.ok;
    j["errors"] = json::array();
    for (const ClipError& e : r.errors) {
        json o;
        o["source"] = e.source;
        o["line"] = e.line;
        if (e.column > 0) o["column"] = e.column;
        o["path"] = e.path;
        o["code"] = e.code;
        o["message"] = e.message;
        o["hint"] = e.hint;
        j["errors"].push_back(std::move(o));
    }
    // `replace`: a name that is not UTF-8 must not make dump() throw.
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace kyubu::game::anim
