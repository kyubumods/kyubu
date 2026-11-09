#include "rig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string_view>

#include "toml_config.h"

namespace kanimator {

using kyubu::game::anim::Channel;
using kyubu::game::anim::kMaxErrors;
using kyubu::game::anim::kPartCount;
using kyubu::game::anim::part_from_name;
using kyubu::game::anim::part_name;

const char* step_name(StepKind k) {
    switch (k) {
        case StepKind::Translate: return "translate";
        case StepKind::RotX: return "rot_x";
        case StepKind::RotY: return "rot_y";
        case StepKind::RotZ: return "rot_z";
        case StepKind::Scale: return "scale";
    }
    return "?";
}

int Rig::find(const std::string& node_name) const {
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].name == node_name) return static_cast<int>(i);
    return -1;
}

uint32_t Rig::parts_used() const {
    uint32_t used = 0;
    auto steps_use = [&used](const std::vector<Step>& steps) {
        for (const Step& s : steps)
            for (const Arg& a : s.args)
                if (a.is_ref) used |= 1u << static_cast<int>(a.part);
    };
    for (const Node& n : nodes) {
        if (n.has_part) used |= 1u << static_cast<int>(n.part);
        steps_use(n.steps);
    }
    for (const Emitter& e : emitters) {
        steps_use(e.steps);
        if (e.amount.is_ref) used |= 1u << static_cast<int>(e.amount.part);
    }
    for (const Shockwave& s : shockwaves) steps_use(s.steps);
    for (const Pillars& p : pillars) steps_use(p.steps);
    return used;
}

bool valid_sound_file(const std::string& name) {
    if (name.size() < 5 || name.size() > kMaxSoundFile) return false;   // at least one byte before ".wav"
    for (char ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
            return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext == ".wav";
}

namespace {

constexpr size_t kMaxQuoted = 40;   // a name from the file is cut to this many bytes in a message

struct Ctx {
    const std::string& source;
    std::vector<ClipError> errors;

    void add(int line, std::string path, const char* code, std::string message, std::string hint) {
        if (static_cast<int>(errors.size()) >= kMaxErrors) return;
        errors.push_back({source, line, std::move(path), code, std::move(message), std::move(hint)});
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

std::string cut(std::string_view s) {
    if (s.size() <= kMaxQuoted) return std::string(s);
    size_t n = kMaxQuoted;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return std::string(s.substr(0, n)) + "...";
}
std::string in_quotes(std::string_view s) { return "\"" + cut(s) + "\""; }

std::string at(const std::string& base, size_t i) { return base + "[" + std::to_string(i) + "]"; }
std::string join(const std::string& base, std::string_view key) {
    return base.empty() ? std::string(key) : base + "." + std::string(key);
}
std::string last_step(const std::string& path) {   // "node[0].parent" gives "`parent`"
    size_t dot = path.rfind('.');
    return "`" + (dot == std::string::npos ? path : path.substr(dot + 1)) + "`";
}

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

// A string field. Null when it is missing (an error when `required`) or not a string (an error).
const std::string* string_field(Ctx& c, const toml::table& t, int line, const std::string& base, const char* key, bool required,
                                const char* example) {
    const toml::node* n = t.get(key);
    std::string path = join(base, key);
    if (!n) {
        if (required)
            c.add(line, path, "missing_field", "`" + std::string(key) + "` is missing",
                  "Add `" + std::string(key) + " = " + example + "`.");
        return nullptr;
    }
    if (!n->is_string()) {
        c.add(line_of(*n), path, "bad_type", last_step(path) + " must be a string, not " + kind(*n),
              "Write it as a quoted string, such as " + std::string(example) + ".");
        return nullptr;
    }
    return &n->as_string()->get();
}

// A finite number with |v| <= kMaxConstant, integer or float.
bool constant(Ctx& c, const toml::node& n, const std::string& path, float& out) {
    if (!n.is_number()) {
        c.add(line_of(n), path, "bad_type", "a step's argument must be a number or the name of a pose value, not " + std::string(kind(n)),
              "Write a number, or a name such as \"head.rot.x\".");
        return false;
    }
    double d = n.is_integer() ? static_cast<double>(n.as_integer()->get()) : n.as_floating_point()->get();
    if (!std::isfinite(d)) {
        c.add(line_of(n), path, "not_finite", "the argument must be a finite number, not nan or inf", "Write a finite number.");
        return false;
    }
    if (std::fabs(d) > kMaxConstant) {
        c.add(line_of(n), path, "out_of_range", "the argument is " + num(d) + "; it must be from -" + num(kMaxConstant) + " to " + num(kMaxConstant),
              "Use a smaller number.");
        return false;
    }
    out = static_cast<float>(d);
    return true;
}

// "[-]<part>.pos|rot[.x|y|z]". The message is empty when the name is good.
std::string parse_ref(const std::string& text, Arg& a) {
    std::string s = text;
    a = Arg{};
    a.is_ref = true;
    if (!s.empty() && s[0] == '-') {
        a.negate = true;
        s.erase(0, 1);
    }
    size_t d1 = s.find('.');
    if (d1 == std::string::npos) return "the name " + in_quotes(text) + " has no `.pos` or `.rot`";
    if (!part_from_name(s.substr(0, d1), a.part)) {
        std::string list;
        for (int i = 0; i < kPartCount; ++i) list += (i ? ", " : "") + std::string(part_name(static_cast<Part>(i)));
        return "unknown part " + in_quotes(s.substr(0, d1)) + " in " + in_quotes(text) + "; the parts are " + list;
    }
    std::string rest = s.substr(d1 + 1);
    std::string chan = rest, axis;
    size_t d2 = rest.find('.');
    if (d2 != std::string::npos) {
        chan = rest.substr(0, d2);
        axis = rest.substr(d2 + 1);
    }
    if (chan == "pos") a.channel = Channel::Pos;
    else if (chan == "rot") a.channel = Channel::Rot;
    else return "unknown channel " + in_quotes(chan) + " in " + in_quotes(text) + "; the channels are pos, rot";
    if (d2 == std::string::npos) return "";
    if (axis == "x") a.axis = 0;
    else if (axis == "y") a.axis = 1;
    else if (axis == "z") a.axis = 2;
    else return "unknown axis " + in_quotes(axis) + " in " + in_quotes(text) + "; the axes are x, y, z";
    return "";
}

bool step_kind_from_name(const std::string& s, StepKind& out) {
    for (StepKind k : {StepKind::Translate, StepKind::RotX, StepKind::RotY, StepKind::RotZ, StepKind::Scale})
        if (s == step_name(k)) {
            out = k;
            return true;
        }
    return false;
}

void read_step(Ctx& c, const toml::node& n, const std::string& path, Step& st) {
    st.line = line_of(n);
    const toml::array* a = n.as_array();
    if (!a) {
        c.add(st.line, path, "bad_type", "a step must be an array such as [\"rot_x\", \"head.rot.x\"], not " + std::string(kind(n)),
              "Write each step as [\"<kind>\", argument...].");
        return;
    }
    if (a->empty() || !(*a)[0].is_string()) {
        c.add(st.line, at(path, 0), "bad_type", "the first item of a step must be its kind, a string",
              "Start the step with \"translate\", \"rot_x\", \"rot_y\", \"rot_z\" or \"scale\".");
        return;
    }
    const std::string& kname = (*a)[0].as_string()->get();
    if (!step_kind_from_name(kname, st.kind)) {
        c.add(line_of((*a)[0]), at(path, 0), "unknown_step", "unknown step " + in_quotes(kname) + "; the steps are translate, rot_x, rot_y, rot_z, scale",
              "Change the first item to one of the five steps.");
        return;
    }
    int total = 0;
    bool good = true;
    for (size_t i = 1; i < a->size(); ++i) {
        const toml::node& an = (*a)[i];
        std::string ap = at(path, i);
        Arg arg;
        if (an.is_string()) {
            std::string msg = parse_ref(an.as_string()->get(), arg);
            if (!msg.empty()) {
                c.add(line_of(an), ap, "unknown_ref", msg, "Write `<part>.pos`, `<part>.rot` or one axis of them, such as \"head.rot.x\".");
                good = false;
                continue;
            }
        } else if (!constant(c, an, ap, arg.value)) {
            good = false;
            continue;
        }
        total += arg.count();
        st.args.push_back(arg);
    }
    if (!good) return;
    bool right = st.kind == StepKind::Translate ? total == 3 : st.kind == StepKind::Scale ? (total == 1 || total == 3) : total == 1;
    if (!right) {
        const char* want = st.kind == StepKind::Translate ? "three numbers" : st.kind == StepKind::Scale ? "one number or three" : "one number";
        c.add(st.line, path, "bad_arg_count",
              "`" + std::string(kname) + "` takes " + want + " but its arguments make " + std::to_string(total),
              std::string("Give `") + kname + "` " + want + "; a name such as \"head.pos\" is three numbers.");
        st.args.clear();
    }
}

bool valid_node_name(const std::string& s);

// A `steps` array: at most kMaxSteps steps.
void read_steps(Ctx& c, const toml::node& sn, const std::string& path, std::vector<Step>& out) {
    const toml::array* sa = sn.as_array();
    if (!sa) {
        c.add(line_of(sn), path, "bad_type", "`steps` must be an array of steps, not " + std::string(kind(sn)), "Write `steps = [[\"rot_x\", 10]]`.");
    } else if (sa->size() > static_cast<size_t>(kMaxSteps)) {
        c.add(line_of(sn), path, "too_many_steps", "there are " + std::to_string(sa->size()) + " steps; the most is " + std::to_string(kMaxSteps),
              "Combine or remove steps.");
    } else {
        out.resize(sa->size());
        for (size_t k = 0; k < sa->size(); ++k) read_step(c, (*sa)[k], at(path, k), out[k]);
    }
}

// A finite number from lo to hi.
bool number_in(Ctx& c, const toml::node& n, const std::string& path, double lo, double hi, float& out) {
    if (!n.is_number()) {
        c.add(line_of(n), path, "bad_type", last_step(path) + " must be a number, not " + std::string(kind(n)),
              "Write a number from " + num(lo) + " to " + num(hi) + ".");
        return false;
    }
    double d = n.is_integer() ? static_cast<double>(n.as_integer()->get()) : n.as_floating_point()->get();
    if (!std::isfinite(d)) {
        c.add(line_of(n), path, "not_finite", last_step(path) + " must be a finite number, not nan or inf", "Write a finite number.");
        return false;
    }
    if (d < lo || d > hi) {
        c.add(line_of(n), path, "out_of_range", last_step(path) + " is " + num(d) + "; it must be from " + num(lo) + " to " + num(hi),
              "Write a number from " + num(lo) + " to " + num(hi) + ".");
        return false;
    }
    out = static_cast<float>(d);
    return true;
}

// One number, or [least, most], each from lo to hi.
bool range_in(Ctx& c, const toml::node& n, const std::string& path, double lo, double hi, Range& out) {
    if (const toml::array* a = n.as_array()) {
        if (a->size() != 2) {
            c.add(line_of(n), path, "bad_value", last_step(path) + " must be one number or two, [least, most]", "Write `[500, 700]` or `600`.");
            return false;
        }
        Range r;
        if (!number_in(c, (*a)[0], at(path, 0), lo, hi, r.lo) || !number_in(c, (*a)[1], at(path, 1), lo, hi, r.hi)) return false;
        if (r.hi < r.lo) {
            c.add(line_of(n), path, "bad_value", last_step(path) + " is [" + num(r.lo) + ", " + num(r.hi) + "]: the least comes first",
                  "Write the smaller number first.");
            return false;
        }
        out = r;
        return true;
    }
    float v = 0;
    if (!number_in(c, n, path, lo, hi, v)) return false;
    out = {v, v};
    return true;
}

// [r, g, b] or [r, g, b, a], each 0 to 1. The alpha is read and dropped.
bool color_in(Ctx& c, const toml::node& n, const std::string& path, float out[3]) {
    const toml::array* a = n.as_array();
    if (!a || (a->size() != 3 && a->size() != 4)) {
        c.add(line_of(n), path, "bad_value", last_step(path) + " must be three numbers (red, green, blue) or four, each 0 to 1",
              "Write `[1, 0.5, 0]`.");
        return false;
    }
    float v[4] = {0, 0, 0, 1};
    for (size_t i = 0; i < a->size(); ++i)
        if (!number_in(c, (*a)[i], at(path, i), 0, 1, v[i])) return false;
    std::memcpy(out, v, 3 * sizeof(float));
    return true;
}

// [r, g, b, a], each 0 to 1 (a shockwave's colour: the alpha is drawn).
bool color4_in(Ctx& c, const toml::node& n, const std::string& path, float out[4]) {
    const toml::array* a = n.as_array();
    if (!a || a->size() != 4) {
        c.add(line_of(n), path, "bad_value", last_step(path) + " must be four numbers (red, green, blue, alpha), each 0 to 1",
              "Write `[1, 0.6, 0.1, 0.9]`.");
        return false;
    }
    float v[4];
    for (size_t i = 0; i < 4; ++i)
        if (!number_in(c, (*a)[i], at(path, i), 0, 1, v[i])) return false;
    std::memcpy(out, v, sizeof v);
    return true;
}

// A whole number from lo to hi.
bool whole_in(Ctx& c, const toml::node& n, const std::string& path, int64_t lo, int64_t hi, int64_t& out) {
    const toml::value<int64_t>* v = n.as_integer();
    if (!v) {
        c.add(line_of(n), path, "bad_type", last_step(path) + " must be a whole number, not " + std::string(kind(n)),
              "Write a whole number from " + std::to_string(lo) + " to " + std::to_string(hi) + ".");
        return false;
    }
    if (v->get() < lo || v->get() > hi) {
        c.add(line_of(n), path, "out_of_range", last_step(path) + " is " + std::to_string(v->get()) + "; it must be from " +
              std::to_string(lo) + " to " + std::to_string(hi), "Write a whole number from " + std::to_string(lo) + " to " + std::to_string(hi) + ".");
        return false;
    }
    out = v->get();
    return true;
}

// A `seed`: 0 to 2147483647.
void seed_in(Ctx& c, const toml::table& t, const std::string& base, uint32_t& out) {
    int64_t v = 0;
    if (const toml::node* n = t.get("seed"))
        if (whole_in(c, *n, join(base, "seed"), 0, 0x7FFFFFFF, v)) out = static_cast<uint32_t>(v);
}

// A marker's name (an `at`): 1 to kMaxMarkerName bytes.
void marker_in(Ctx& c, const toml::table& t, int line, const std::string& base, bool required, std::string& out) {
    if (const std::string* s = string_field(c, t, line, base, "at", required, "\"strike\"")) {
        if (s->empty() || s->size() > static_cast<size_t>(kyubu::game::anim::kMaxMarkerName))
            c.add(line_of(*t.get("at")), join(base, "at"), "bad_value",
                  "`at` must be a marker's name, 1 to " + std::to_string(kyubu::game::anim::kMaxMarkerName) + " bytes",
                  "Write the name of a `[[marker]]` of the clip, such as \"strike\".");
        else
            out = *s;
    }
}

// The `[particles]` table.
void read_particles(Ctx& c, const toml::node& n, ParticleWorld& w) {
    const toml::table* t = n.as_table();
    if (!t) {
        c.add(line_of(n), "particles", "bad_type", "`particles` must be a table, not " + std::string(kind(n)),
              "Write a `[particles]` table with `block` and `gravity`.");
        return;
    }
    w.present = true;
    check_keys(c, *t, "particles", {"block", "gravity"});
    if (const toml::node* bn = t->get("block")) {
        float v = 0;
        if (number_in(c, *bn, "particles.block", 0, kMaxConstant, v)) {
            if (v > 0) w.block = v;
            else c.add(line_of(*bn), "particles.block", "out_of_range", "`block` must be above 0", "Write the voxels in one block, such as 11.63.");
        }
    } else {
        c.add(line_of(n), "particles.block", "missing_field", "`block` is missing", "Add `block = 11.63`: the voxels of the rig in one block.");
    }
    if (const toml::node* gn = t->get("gravity")) number_in(c, *gn, "particles.gravity", -kMaxConstant, kMaxConstant, w.gravity);
    else c.add(line_of(n), "particles.gravity", "missing_field", "`gravity` is missing", "Add `gravity = 30`: blocks a second squared.");
}

void read_emitter(Ctx& c, const toml::table& t, const std::string& base, const Rig& rig, std::vector<Emitter>& done, Emitter& e) {
    e.line = line_of(t);
    check_keys(c, t, base, {"name", "parent", "steps", "amount", "rate", "step_ms", "speed", "cone", "spreading", "life", "size",
                            "size_end", "color", "color_random", "color_end", "gravity", "ground", "spin", "seed", "at", "for_ms",
                            "cone_min", "offset"});
    if (const std::string* nm = string_field(c, t, e.line, base, "name", true, "\"breath\"")) {
        if (!valid_node_name(*nm))
            c.add(line_of(*t.get("name")), join(base, "name"), "bad_name",
                  "the emitter name " + in_quotes(*nm) + " is not 1 to " + std::to_string(kMaxNodeName) + " of a-z, 0-9, '_'",
                  "Use lower-case letters, digits and '_'.");
        else if (std::any_of(done.begin(), done.end(), [&](const Emitter& o) { return o.name == *nm; }))
            c.add(line_of(*t.get("name")), join(base, "name"), "duplicate_node", "an emitter named " + in_quotes(*nm) + " is already defined",
                  "Give each emitter its own name.");
        else
            e.name = *nm;
    }
    if (const std::string* p = string_field(c, t, e.line, base, "parent", true, "\"head\"")) {
        e.parent = rig.find(*p);
        if (e.parent < 0)
            c.add(line_of(*t.get("parent")), join(base, "parent"), "unknown_parent", "the parent " + in_quotes(*p) + " is not the name of any node",
                  "Name the node the emitter rides on.");
    }
    if (const toml::node* sn = t.get("steps")) read_steps(c, *sn, join(base, "steps"), e.steps);
    e.amount.value = 1;
    if (const toml::node* an = t.get("amount")) {
        std::string path = join(base, "amount");
        if (an->is_string()) {
            std::string msg = parse_ref(an->as_string()->get(), e.amount);
            if (!msg.empty())
                c.add(line_of(*an), path, "unknown_ref", msg, "Write one value of the pose, such as \"tail.pos.x\", or a number.");
            else if (e.amount.axis < 0)
                c.add(line_of(*an), path, "bad_value", "`amount` must be one value of the pose, such as \"tail.pos.x\", not three",
                      "Add the axis: \".x\", \".y\" or \".z\".");
        } else {
            constant(c, *an, path, e.amount.value);
        }
    }
    if (const toml::node* rn = t.get("rate")) number_in(c, *rn, join(base, "rate"), 0, kMaxEmitRate, e.rate);
    else c.add(e.line, join(base, "rate"), "missing_field", "`rate` is missing", "Add `rate = 200`: particles a second.");
    if (const toml::node* n = t.get("step_ms")) number_in(c, *n, join(base, "step_ms"), 1, 1000, e.step_ms);
    if (const toml::node* n = t.get("speed")) range_in(c, *n, join(base, "speed"), 0, kMaxConstant, e.speed);
    if (const toml::node* n = t.get("cone")) number_in(c, *n, join(base, "cone"), 0, 180, e.cone);
    if (const toml::node* n = t.get("spreading")) number_in(c, *n, join(base, "spreading"), 0, 64, e.spreading);
    if (const toml::node* n = t.get("life")) range_in(c, *n, join(base, "life"), 1, kMaxParticleLife, e.life);
    else c.add(e.line, join(base, "life"), "missing_field", "`life` is missing", "Add `life = 600`: milliseconds.");
    bool size_given = false;
    if (const toml::node* n = t.get("size")) {
        size_given = number_in(c, *n, join(base, "size"), 0, 16, e.size);
        if (size_given && e.size <= 0) {
            c.add(line_of(*n), join(base, "size"), "out_of_range", "`size` must be above 0", "Write the cube's side in blocks, such as 0.25.");
            size_given = false;
        }
    }
    e.size_end = e.size;
    if (const toml::node* n = t.get("size_end")) number_in(c, *n, join(base, "size_end"), 0, 16, e.size_end);
    if (const toml::node* n = t.get("color")) color_in(c, *n, join(base, "color"), e.color);
    if (const toml::node* n = t.get("color_random")) e.has_color_random = color_in(c, *n, join(base, "color_random"), e.color_random);
    if (const toml::node* n = t.get("color_end")) e.has_color_end = color_in(c, *n, join(base, "color_end"), e.color_end);
    if (const toml::node* n = t.get("gravity")) {
        if (const toml::value<bool>* b = n->as_boolean()) e.gravity = b->get();
        else c.add(line_of(*n), join(base, "gravity"), "bad_type", "`gravity` must be true or false", "Write true or false.");
    }
    if (const toml::node* n = t.get("ground")) {
        const std::string* s = n->is_string() ? &n->as_string()->get() : nullptr;
        if (s && (*s == "pass" || *s == "stop")) e.ground_stop = *s == "stop";
        else c.add(line_of(*n), join(base, "ground"), "bad_value", "`ground` must be \"pass\" or \"stop\"", "Write `ground = \"stop\"`.");
    }
    if (const toml::node* n = t.get("spin")) number_in(c, *n, join(base, "spin"), 0, 3600, e.spin);
    if (const toml::node* n = t.get("seed")) {
        const toml::value<int64_t>* v = n->as_integer();
        if (!v) c.add(line_of(*n), join(base, "seed"), "bad_type", "`seed` must be a whole number, not " + std::string(kind(*n)), "Write `seed = 1`.");
        else if (v->get() < 0 || v->get() > 0x7FFFFFFF)
            c.add(line_of(*n), join(base, "seed"), "out_of_range", "`seed` must be 0 to 2147483647", "Write a smaller whole number.");
        else e.seed = static_cast<uint32_t>(v->get());
    }
    marker_in(c, t, e.line, base, false, e.at);
    if (const toml::node* n = t.get("for_ms")) {
        number_in(c, *n, join(base, "for_ms"), 1, kMaxParticleLife, e.for_ms);
        if (!t.get("at"))
            c.add(line_of(*n), join(base, "for_ms"), "missing_field", "`for_ms` is how long the emitter throws from a marker: it needs `at`",
                  "Add `at = \"<marker>\"`, or remove `for_ms`.");
    }
    if (const toml::node* n = t.get("cone_min")) {
        if (number_in(c, *n, join(base, "cone_min"), 0, 180, e.cone_min) && e.cone_min > e.cone)
            c.add(line_of(*n), join(base, "cone_min"), "bad_value",
                  "`cone_min` is " + num(e.cone_min) + ", past the `cone` of " + num(e.cone) + ": the ways lie between the two",
                  "Make `cone_min` at most `cone`.");
    }
    if (const toml::node* n = t.get("offset")) range_in(c, *n, join(base, "offset"), 0, kMaxConstant, e.offset);
}

// The keys every effect has: its name (unique among `done`), the node it rides on, its steps and the marker that starts it.
template <class T>
void read_effect_head(Ctx& c, const toml::table& t, const std::string& base, const Rig& rig, const std::vector<T>& done, const char* what,
                      T& e) {
    e.line = line_of(t);
    if (const std::string* nm = string_field(c, t, e.line, base, "name", true, "\"slam\"")) {
        if (!valid_node_name(*nm))
            c.add(line_of(*t.get("name")), join(base, "name"), "bad_name",
                  std::string("the ") + what + " name " + in_quotes(*nm) + " is not 1 to " + std::to_string(kMaxNodeName) + " of a-z, 0-9, '_'",
                  "Use lower-case letters, digits and '_'.");
        else if (std::any_of(done.begin(), done.end(), [&](const T& o) { return o.name == *nm; }))
            c.add(line_of(*t.get("name")), join(base, "name"), "duplicate_node", std::string("a ") + what + " named " + in_quotes(*nm) +
                  " is already defined", std::string("Give each ") + what + " its own name.");
        else
            e.name = *nm;
    }
    if (const std::string* p = string_field(c, t, e.line, base, "parent", true, "\"ground\"")) {
        e.parent = rig.find(*p);
        if (e.parent < 0)
            c.add(line_of(*t.get("parent")), join(base, "parent"), "unknown_parent", "the parent " + in_quotes(*p) + " is not the name of any node",
                  std::string("Name the node the ") + what + " rides on.");
    }
    if (const toml::node* sn = t.get("steps")) read_steps(c, *sn, join(base, "steps"), e.steps);
    marker_in(c, t, e.line, base, true, e.at);
}

void read_shockwave(Ctx& c, const toml::table& t, const std::string& base, const Rig& rig, const std::vector<Shockwave>& done, Shockwave& s) {
    check_keys(c, t, base, {"name", "parent", "steps", "at", "inner_radius", "thickness", "expansion", "life_ticks", "fade_on_tick",
                            "fade_speed", "color", "pixel_size"});
    read_effect_head(c, t, base, rig, done, "shockwave", s);
    if (const toml::node* n = t.get("inner_radius")) number_in(c, *n, join(base, "inner_radius"), 0, kMaxConstant, s.inner_radius);
    if (const toml::node* n = t.get("thickness")) number_in(c, *n, join(base, "thickness"), 0.01, kMaxConstant, s.thickness);
    if (const toml::node* n = t.get("expansion")) number_in(c, *n, join(base, "expansion"), -kMaxConstant, kMaxConstant, s.expansion);
    int64_t v = 0;
    if (const toml::node* n = t.get("life_ticks"))
        if (whole_in(c, *n, join(base, "life_ticks"), 1, 6000, v)) s.life_ticks = static_cast<uint32_t>(v);
    if (const toml::node* n = t.get("fade_on_tick"))
        if (whole_in(c, *n, join(base, "fade_on_tick"), 0, 6000, v)) s.fade_on_tick = static_cast<uint32_t>(v);
    if (const toml::node* n = t.get("fade_speed")) number_in(c, *n, join(base, "fade_speed"), 0, 1, s.fade_speed);
    if (const toml::node* n = t.get("color")) color4_in(c, *n, join(base, "color"), s.color);
    if (const toml::node* n = t.get("pixel_size")) number_in(c, *n, join(base, "pixel_size"), -1, 16, s.pixel_size);
}

void read_pillars(Ctx& c, const toml::table& t, const std::string& base, const Rig& rig, const std::vector<Pillars>& done, Pillars& p) {
    check_keys(c, t, base, {"name", "parent", "steps", "at", "count", "radius", "clear", "wave", "delay", "width", "height", "taper", "cell",
                            "lean", "turn", "rise_ms", "hold_ms", "sink_ms", "color", "color_random", "color_top", "seed", "stones",
                            "rubble"});
    read_effect_head(c, t, base, rig, done, "pillars", p);
    int64_t v = 0;
    if (const toml::node* n = t.get("count")) {
        if (whole_in(c, *n, join(base, "count"), 1, kMaxPillars, v)) p.count = static_cast<int>(v);
    } else {
        c.add(p.line, join(base, "count"), "missing_field", "`count` is missing", "Add `count = 20`: the pillars of a set.");
    }
    if (const toml::node* n = t.get("radius")) range_in(c, *n, join(base, "radius"), 0, kMaxConstant, p.radius);
    else c.add(p.line, join(base, "radius"), "missing_field", "`radius` is missing", "Add `radius = [3, 12]`: blocks from the centre.");
    if (const toml::node* n = t.get("clear")) {
        const toml::array* a = n->as_array();
        bool good = a && a->size() == 4;
        for (size_t i = 0; good && i < 4; ++i) good = number_in(c, (*a)[i], at(join(base, "clear"), i), -kMaxConstant, kMaxConstant, p.clear[i]);
        if (!a || a->size() != 4)
            c.add(line_of(*n), join(base, "clear"), "bad_value", "`clear` must be four numbers: least x, least y, most x, most y",
                  "Write `clear = [-3, -14, 3, 2]`: blocks in the figure's frame.");
        else if (good && (p.clear[2] < p.clear[0] || p.clear[3] < p.clear[1]))
            c.add(line_of(*n), join(base, "clear"), "bad_value", "`clear` is least x, least y, most x, most y: a most is below its least",
                  "Write the smaller numbers first.");
        else
            p.has_clear = good;
    }
    if (const toml::node* n = t.get("wave")) number_in(c, *n, join(base, "wave"), 0, kMaxConstant, p.wave);
    if (const toml::node* n = t.get("delay")) range_in(c, *n, join(base, "delay"), 0, kMaxParticleLife, p.delay);
    auto above_zero = [&](const char* key, double hi, Range& out) {
        if (const toml::node* n = t.get(key))
            if (range_in(c, *n, join(base, key), 0, hi, out) && out.lo <= 0)
                c.add(line_of(*n), join(base, key), "out_of_range", "`" + std::string(key) + "` must be above 0", "Write blocks, such as 1.");
    };
    above_zero("width", 16, p.width);
    above_zero("height", 64, p.height);
    if (const toml::node* n = t.get("stones")) range_in(c, *n, join(base, "stones"), 1, 8, p.stones);
    if (const toml::node* n = t.get("rubble")) range_in(c, *n, join(base, "rubble"), 0, 8, p.rubble);
    if (const toml::node* n = t.get("taper")) number_in(c, *n, join(base, "taper"), 0, 1, p.taper);
    if (const toml::node* n = t.get("cell")) number_in(c, *n, join(base, "cell"), 0.02, 16, p.cell);
    if (const toml::node* n = t.get("lean")) number_in(c, *n, join(base, "lean"), 0, 60, p.lean);
    if (const toml::node* n = t.get("turn")) number_in(c, *n, join(base, "turn"), 0, 180, p.turn);
    if (const toml::node* n = t.get("rise_ms")) number_in(c, *n, join(base, "rise_ms"), 1, kMaxParticleLife, p.rise_ms);
    if (const toml::node* n = t.get("hold_ms")) range_in(c, *n, join(base, "hold_ms"), 0, kMaxParticleLife, p.hold_ms);
    if (const toml::node* n = t.get("sink_ms")) number_in(c, *n, join(base, "sink_ms"), 1, kMaxParticleLife, p.sink_ms);
    if (const toml::node* n = t.get("color")) color_in(c, *n, join(base, "color"), p.color);
    if (const toml::node* n = t.get("color_random")) p.has_color_random = color_in(c, *n, join(base, "color_random"), p.color_random);
    if (const toml::node* n = t.get("color_top")) p.has_color_top = color_in(c, *n, join(base, "color_top"), p.color_top);
    seed_in(c, t, base, p.seed);
}

void read_sound(Ctx& c, const toml::table& t, const std::string& base, const Rig&, const std::vector<RigSound>&, RigSound& s) {
    check_keys(c, t, base, {"file", "at", "volume", "pitch", "delay_ms"});
    s.line = line_of(t);
    if (const std::string* f = string_field(c, t, s.line, base, "file", true, "\"hit.wav\"")) {
        if (!valid_sound_file(*f))
            c.add(line_of(*t.get("file")), join(base, "file"), "bad_value",
                  "the sound file " + in_quotes(*f) + " is not 1 to " + std::to_string(kMaxSoundFile) +
                  " of letters, digits, '_', '-', '.' ending in .wav", "Write the name of a .wav file, such as \"hit.wav\".");
        else
            s.file = *f;
    }
    marker_in(c, t, s.line, base, true, s.at);
    if (const toml::node* n = t.get("volume")) number_in(c, *n, join(base, "volume"), 0, kMaxSoundVolume, s.volume);
    if (const toml::node* n = t.get("pitch")) number_in(c, *n, join(base, "pitch"), kMinSoundPitch, kMaxSoundPitch, s.pitch);
    if (const toml::node* n = t.get("delay_ms")) number_in(c, *n, join(base, "delay_ms"), 0, kMaxParticleLife, s.delay_ms);
}

// An array of effect tables (`[[shockwave]]`, `[[pillars]]`, `[[sound]]`): at most `max`.
template <class T, class Read>
void read_effects(Ctx& c, const toml::node& en, const char* key, int max, const Rig& rig, std::vector<T>& dst, Read read) {
    const toml::array* arr = en.as_array();
    if (!arr || !(arr->empty() || arr->is_array_of_tables())) {
        c.add(line_of(en), key, "bad_type", "`" + std::string(key) + "` must be an array of tables, not " +
              std::string(arr ? "an array of other values" : kind(en)), "Write each under its own `[[" + std::string(key) + "]]` header.");
        return;
    }
    if (arr->size() > static_cast<size_t>(max)) {
        c.add(line_of(en), key, "too_many_nodes", "there are " + std::to_string(arr->size()) + " `" + key + "` tables; the most is " +
              std::to_string(max), "Remove some.");
        return;
    }
    std::vector<T> out;
    for (size_t i = 0; i < arr->size(); ++i) {
        T e;
        read(c, *(*arr)[i].as_table(), at(key, i), rig, out, e);
        out.push_back(std::move(e));
    }
    dst = std::move(out);
}

void read_emitters(Ctx& c, const toml::node& en, Rig& rig) {
    const toml::array* arr = en.as_array();
    if (!arr || !(arr->empty() || arr->is_array_of_tables())) {
        c.add(line_of(en), "emitter", "bad_type", "`emitter` must be an array of tables, not " + std::string(arr ? "an array of other values" : kind(en)),
              "Write each emitter under its own `[[emitter]]` header.");
        return;
    }
    if (arr->size() > static_cast<size_t>(kMaxEmitters)) {
        c.add(line_of(en), "emitter", "too_many_nodes", "there are " + std::to_string(arr->size()) + " emitters; the most is " +
              std::to_string(kMaxEmitters), "Remove emitters.");
        return;
    }
    std::vector<Emitter> out;
    for (size_t i = 0; i < arr->size(); ++i) {
        Emitter e;
        read_emitter(c, *(*arr)[i].as_table(), at("emitter", i), rig, out, e);
        out.push_back(std::move(e));
    }
    if (!out.empty() && !rig.particles.present)
        c.add(line_of(en), "particles", "missing_field", "a rig with an emitter needs a `[particles]` table",
              "Add `[particles]` with `block` (voxels in a block) and `gravity` (blocks a second squared).");
    rig.emitters = std::move(out);
}

void read_model(Ctx& c, const toml::node& n, const std::string& path, Model& m) {
    if (!n.is_string()) {
        c.add(line_of(n), path, "bad_type", "`model` must be a string, not " + std::string(kind(n)), "Write \"box 8 8 8\" or the name of a model.");
        return;
    }
    const std::string& s = n.as_string()->get();
    if (s.rfind("box", 0) == 0 && (s.size() == 3 || s[3] == ' ')) {
        float v[3];
        char tail;
        if (std::sscanf(s.c_str(), "box %f %f %f%c", &v[0], &v[1], &v[2], &tail) != 3 || !(v[0] > 0 && v[1] > 0 && v[2] > 0) ||
            v[0] > 255 || v[1] > 255 || v[2] > 255) {
            c.add(line_of(n), path, "bad_model", "the box " + in_quotes(s) + " needs three sizes, each above 0 and at most 255",
                  "Write `model = \"box 8 8 8\"`: the x, y and z size in voxels.");
            return;
        }
        m.kind = Model::Kind::Box;
        std::memcpy(m.box, v, sizeof v);
        return;
    }
    bool ok = !s.empty() && s.size() <= 64;
    for (char ch : s) ok = ok && ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == '/');
    if (!ok) {
        c.add(line_of(n), path, "bad_model", "the model name " + in_quotes(s) + " is not 1 to 64 of a-z, 0-9, '_', '-', '.', '/'",
              "Use a model's file name without its extension, or `box W D H`.");
        return;
    }
    m.kind = Model::Kind::Named;
    m.name = s;
}

bool valid_node_name(const std::string& s) {
    if (s.empty() || s.size() > kMaxNodeName) return false;
    for (char ch : s)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    return true;
}

// The optional `[capture]` table.
void read_capture(Ctx& c, const toml::node& n, CaptureMap& m) {
    const toml::table* t = n.as_table();
    if (!t) {
        c.add(line_of(n), "capture", "bad_type", "`capture` must be a table, not " + std::string(kind(n)), "Write a `[capture]` table with `unit`.");
        return;
    }
    m.present = true;
    check_keys(c, *t, "capture", {"unit", "yaw", "origin"});
    if (const toml::node* un = t->get("unit")) {
        float v = 0;
        if (constant(c, *un, "capture.unit", v)) {
            if (v > 0) m.unit = v;
            else c.add(line_of(*un), "capture.unit", "out_of_range", "`unit` must be above 0", "Write the capture's length unit per voxel, such as 0.086.");
        }
    } else {
        c.add(line_of(n), "capture.unit", "missing_field", "`unit` is missing", "Add `unit = 0.086`: the capture's length unit per voxel.");
    }
    if (const toml::node* yn = t->get("yaw")) {
        const std::string* s = yn->is_string() ? &yn->as_string()->get() : nullptr;
        if (s && (*s == "ccw" || *s == "cw")) m.yaw_sign = *s == "ccw" ? 1.0f : -1.0f;
        else c.add(line_of(*yn), "capture.yaw", "bad_value", "`yaw` must be \"ccw\" or \"cw\"", "Write `yaw = \"ccw\"`.");
    }
    if (const toml::node* on = t->get("origin")) {
        const std::string* s = on->is_string() ? &on->as_string()->get() : nullptr;
        if (s && (*s == "corner" || *s == "centre")) m.corner = *s == "corner";
        else c.add(line_of(*on), "capture.origin", "bad_value", "`origin` must be \"corner\" or \"centre\"", "Write `origin = \"corner\"`.");
    }
}

void read_rig(Ctx& c, const toml::table& t, Rig& rig) {
    check_keys(c, t, "", {"name", "node", "capture", "particles", "emitter", "shockwave", "pillars", "sound"});
    if (const toml::node* cn = t.get("capture")) read_capture(c, *cn, rig.capture);
    if (const toml::node* pn = t.get("particles")) read_particles(c, *pn, rig.particles);
    if (const std::string* nm = string_field(c, t, 0, "", "name", true, "\"stand-in rig\"")) {
        if (nm->empty() || nm->size() > 64)
            c.add(line_of(*t.get("name")), "name", "bad_name", "`name` must be 1 to 64 bytes", "Give the rig a short name.");
        else
            rig.name = *nm;
    }
    const toml::node* nn = t.get("node");
    const toml::array* arr = nn ? nn->as_array() : nullptr;
    if (!nn) {
        c.add(0, "node", "missing_field", "the rig has no `[[node]]` table", "Add at least a root `[[node]]` with a `name`.");
        return;
    }
    if (!arr || !(arr->empty() || arr->is_array_of_tables())) {
        c.add(line_of(*nn), "node", "bad_type", "`node` must be an array of tables, not " + std::string(arr ? "an array of other values" : kind(*nn)),
              "Write each node under its own `[[node]]` header.");
        return;
    }
    if (arr->empty()) {
        c.add(line_of(*nn), "node", "missing_field", "the rig has no `[[node]]` table", "Add at least a root `[[node]]` with a `name`.");
        return;
    }
    if (arr->size() > static_cast<size_t>(kMaxNodes)) {
        c.add(line_of(*nn), "node", "too_many_nodes", "there are " + std::to_string(arr->size()) + " nodes; the most is " + std::to_string(kMaxNodes),
              "Remove nodes.");
        return;
    }
    const size_t count = arr->size();
    rig.nodes.resize(count);
    std::vector<const toml::table*> tabs(count);
    // Pass 1: the names, so a parent can be found whatever the order of the file.
    for (size_t i = 0; i < count; ++i) {
        const toml::table& nt = *(*arr)[i].as_table();
        tabs[i] = &nt;
        Node& nd = rig.nodes[i];
        std::string base = at("node", i);
        nd.line = line_of(nt);
        check_keys(c, nt, base, {"name", "parent", "part", "model", "approximate", "slot", "steps"});
        if (const std::string* nm = string_field(c, nt, nd.line, base, "name", true, "\"head\"")) {
            if (!valid_node_name(*nm)) {
                c.add(line_of(*nt.get("name")), join(base, "name"), "bad_name",
                      "the node name " + in_quotes(*nm) + " is not 1 to " + std::to_string(kMaxNodeName) + " of a-z, 0-9, '_'",
                      "Use lower-case letters, digits and '_'.");
            } else if (rig.find(*nm) >= 0) {
                c.add(line_of(*nt.get("name")), join(base, "name"), "duplicate_node", "a node named " + in_quotes(*nm) + " is already defined",
                      "Give each node its own name.");
            } else {
                nd.name = *nm;
            }
        }
    }
    // Pass 2: everything else.
    int roots = 0;
    for (size_t i = 0; i < count; ++i) {
        const toml::table& nt = *tabs[i];
        Node& nd = rig.nodes[i];
        std::string base = at("node", i);
        if (const toml::node* pn = nt.get("parent")) {
            if (const std::string* p = string_field(c, nt, nd.line, base, "parent", false, "\"body\"")) {
                int idx = rig.find(*p);
                if (idx < 0)
                    c.add(line_of(*pn), join(base, "parent"), "unknown_parent",
                          "the parent " + in_quotes(*p) + " is not the name of any node", "Name a node that is defined in this file.");
                else
                    nd.parent = idx;
            }
        } else if (++roots == 1) {
            rig.root = static_cast<int>(i);
        } else {
            c.add(nd.line, base, "two_roots", "the rig has a second node with no `parent`; a rig has one root",
                  "Add `parent = \"<node>\"` to this node.");
        }
        if (const toml::node* pn = nt.get("part")) {
            std::string path = join(base, "part");
            if (!pn->is_string())
                c.add(line_of(*pn), path, "bad_type", "`part` must be a string, not " + std::string(kind(*pn)), "Write a clip part name, such as \"head\".");
            else if (!part_from_name(pn->as_string()->get(), nd.part)) {
                std::string list;
                for (int k = 0; k < kPartCount; ++k) list += (k ? ", " : "") + std::string(part_name(static_cast<Part>(k)));
                c.add(line_of(*pn), path, "unknown_part", "unknown part " + in_quotes(pn->as_string()->get()) + "; the parts are " + list,
                      "Change `part` to one of: " + list + ".");
            } else {
                nd.has_part = true;
            }
        }
        if (const toml::node* mn = nt.get("model")) read_model(c, *mn, join(base, "model"), nd.model);
        if (const toml::node* an = nt.get("approximate")) {
            if (const toml::value<bool>* b = an->as_boolean()) nd.approximate = b->get();
            else c.add(line_of(*an), join(base, "approximate"), "bad_type", "`approximate` must be true or false", "Write true or false.");
        }
        if (const toml::node* sl = nt.get("slot")) {
            const toml::value<int64_t>* v = sl->as_integer();
            if (!v) c.add(line_of(*sl), join(base, "slot"), "bad_type", "`slot` must be a whole number, not " + std::string(kind(*sl)), "Write `slot = 3`.");
            else if (v->get() < 0 || v->get() >= kCaptureSlots)
                c.add(line_of(*sl), join(base, "slot"), "out_of_range", "`slot` must be 0 to " + std::to_string(kCaptureSlots - 1),
                      "Use the index of a part matrix of a capture (docs/dev-tools.md, \"Capture files\").");
            else nd.slot = static_cast<int>(v->get());
        }
        if (const toml::node* sn = nt.get("steps")) read_steps(c, *sn, join(base, "steps"), nd.steps);
    }
    if (const toml::node* en = t.get("emitter")) read_emitters(c, *en, rig);
    if (const toml::node* sn = t.get("shockwave")) read_effects(c, *sn, "shockwave", kMaxShockwaves, rig, rig.shockwaves, read_shockwave);
    if (const toml::node* pn = t.get("pillars")) read_effects(c, *pn, "pillars", kMaxPillarSets, rig, rig.pillars, read_pillars);
    if (const toml::node* sn = t.get("sound")) read_effects(c, *sn, "sound", kMaxSounds, rig, rig.sounds, read_sound);
    if (rig.has_effects() && !rig.particles.present)
        c.add(0, "particles", "missing_field", "a rig with a `[[shockwave]]` or `[[pillars]]` needs a `[particles]` table: their lengths are in blocks",
              "Add `[particles]` with `block` (voxels in a block) and `gravity` (blocks a second squared).");
    if (!c.errors.empty()) return;
    if (roots == 0) {
        c.add(0, "node", "no_root", "no node is the root; every node has a `parent`", "Remove the `parent` of one node.");
        return;
    }
    // Every node must reach the root through its parents, and a parent comes before its children in `order`.
    rig.order.push_back(rig.root);
    for (size_t head = 0; head < rig.order.size(); ++head)
        for (size_t i = 0; i < count; ++i)
            if (rig.nodes[i].parent == rig.order[head]) rig.order.push_back(static_cast<int>(i));
    for (size_t i = 0; i < count; ++i)
        if (std::find(rig.order.begin(), rig.order.end(), static_cast<int>(i)) == rig.order.end())
            c.add(rig.nodes[i].line, join(at("node", i), "parent"), "parent_cycle",
                  "the parents of " + in_quotes(rig.nodes[i].name) + " loop and never reach the root", "Break the loop: one node in it must hang from the root's tree.");
}

}  // namespace

RigLoad parse_rig(const std::string& text, const std::string& source) {
    RigLoad r;
    Ctx c{source, {}};
    if (text.size() > kMaxRigBytes) {
        c.add(0, "", "too_large", "the text has " + std::to_string(text.size()) + " bytes; the most is " + std::to_string(kMaxRigBytes),
              "A rig is a short list of nodes.");
    } else {
        auto res = toml::parse(text, source);
        if (!res)
            c.add(static_cast<int>(res.error().source().begin.line), "", "toml_syntax", std::string(res.error().description()),
                  "Correct the TOML at this line; a rig is a plain TOML file.");
        else
            read_rig(c, res.table(), r.rig);
    }
    if (c.errors.empty()) {
        r.ok = true;
        return r;
    }
    std::stable_sort(c.errors.begin(), c.errors.end(), [](const ClipError& a, const ClipError& b) { return a.line < b.line; });
    r.rig = Rig{};
    r.errors = std::move(c.errors);
    return r;
}

// ---- the solver ----

void Affine::apply(const float p[3], float out[3]) const {
    for (int i = 0; i < 3; ++i) out[i] = r[i][0] * p[0] + r[i][1] * p[1] + r[i][2] * p[2] + t[i];
}

Affine operator*(const Affine& a, const Affine& b) {
    Affine o;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
        o.t[i] = a.r[i][0] * b.t[0] + a.r[i][1] * b.t[1] + a.r[i][2] * b.t[2] + a.t[i];
    }
    return o;
}

namespace {

// The numbers a step's arguments stand for, in order. At most 3 (the parser keeps to that).
int values(const Step& s, const Pose& pose, float out[3]) {
    int n = 0;
    for (const Arg& a : s.args) {
        if (n + a.count() > 3) break;   // a rig built in code, not through parse_rig
        if (!a.is_ref) {
            out[n++] = a.value;
            continue;
        }
        const kyubu::game::anim::PartPose& pp = pose.parts[static_cast<int>(a.part)];
        const float* v = a.channel == Channel::Pos ? pp.pos : pp.rot;
        float sign = a.negate ? -1.0f : 1.0f;
        if (a.axis >= 0) {
            out[n++] = sign * v[a.axis];
        } else {
            for (int k = 0; k < 3; ++k) out[n++] = sign * v[k];
        }
    }
    return n;
}

Affine step_matrix(const Step& s, const Pose& pose) {
    float v[3] = {0, 0, 0};
    int n = values(s, pose, v);
    Affine m;
    switch (s.kind) {
        case StepKind::Translate:
            if (n == 3) std::memcpy(m.t, v, sizeof m.t);
            break;
        case StepKind::Scale:
            if (n == 1 || n == 3)
                for (int i = 0; i < 3; ++i) m.r[i][i] = v[n == 1 ? 0 : i];
            break;
        default: {
            if (n != 1) break;
            const float rad = v[0] * 3.14159265358979f / 180.0f;
            const float cs = std::cos(rad), sn = std::sin(rad);
            if (s.kind == StepKind::RotX) {
                m.r[1][1] = cs; m.r[1][2] = -sn; m.r[2][1] = sn; m.r[2][2] = cs;
            } else if (s.kind == StepKind::RotY) {
                m.r[0][0] = cs; m.r[0][2] = sn; m.r[2][0] = -sn; m.r[2][2] = cs;
            } else {
                m.r[0][0] = cs; m.r[0][1] = -sn; m.r[1][0] = sn; m.r[1][1] = cs;
            }
        }
    }
    return m;
}

}  // namespace

Affine steps_prefix(const Node& node, const Pose& pose, size_t count) {
    Affine m;
    for (size_t i = 0; i < node.steps.size() && i < count; ++i) m = m * step_matrix(node.steps[i], pose);
    return m;
}

Affine steps_matrix(const std::vector<Step>& steps, const Pose& pose) {
    Affine m;
    for (const Step& st : steps) m = m * step_matrix(st, pose);
    return m;
}

float arg_value(const Arg& a, const Pose& pose) {
    if (!a.is_ref) return a.value;
    const kyubu::game::anim::PartPose& pp = pose.parts[static_cast<int>(a.part)];
    const float v = (a.channel == Channel::Pos ? pp.pos : pp.rot)[a.axis < 0 ? 0 : a.axis];
    return a.negate ? -v : v;
}

Solved solve_pose(const Rig& rig, const Pose& pose) {
    Solved s;
    s.pose = pose;
    s.world.assign(rig.nodes.size(), Affine{});
    for (int idx : rig.order) {
        const Node& nd = rig.nodes[idx];
        Affine m = nd.parent >= 0 ? s.world[nd.parent] : Affine{};
        for (const Step& st : nd.steps) m = m * step_matrix(st, pose);
        s.world[idx] = m;
    }
    return s;
}

Solved solve(const Rig& rig, const Pose& base, const Clip& clip, const Timing& at) {
    Pose p = base;
    Applied ap = kyubu::game::anim::apply_clip(clip, at, p);
    Solved s = solve_pose(rig, p);
    s.applied = ap;
    s.missing_parts = (ap.pos_parts | ap.rot_parts) & ~rig.parts_used();
    return s;
}

}  // namespace kanimator
