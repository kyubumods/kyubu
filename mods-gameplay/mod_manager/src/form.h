// The settings form of the details (design D9): what the page shows for a mod's options and how a click, an arrow or a typed
// character changes a draft value. No game calls here, so unit tests cover it. The glue (main.cpp) fills a Form from mods_option_get,
// validates each draft with mods_option_check and draws the rows this header lays out. Text is UTF-8; values are the loader's own
// text (strings without their quotes), so a draft goes to mods_option_check and mods_option_set as it is.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace form {

enum class Type { Bool, Int, Float, String };
// The control of an option: an ON/OFF or choice picker, a slider (int or float with both limits), a number field with - and +, or a text field.
enum class Kind { Picker, Slider, Number, Text };
enum class Zone { None, Left, Right, Value, Track, Reset };

constexpr size_t kTextMax = 90;
constexpr size_t kShortDescription = 44;   // a longer description is a hover hint, not a grey line
constexpr size_t kMaxDescription = 120;    // the loader refuses a longer one (modconfig.cpp), so a tooltip stays a few lines

// The hover tooltip is drawn over the bottom lines of the details column, over the options under it. `slots` is how many lines the
// column holds. It takes at most half of them (and at least one).
inline int tip_lines(int slots, int wanted) {
    return std::max(1, std::min(wanted, slots / 2));
}

// The line the hovered option is drawn on while a tooltip of `lines` lines covers the last ones: its own, or the one just above the
// tooltip when its own is covered, so the tooltip never hides the option it explains.
inline int tip_row_slot(int hovered, int slots, int lines) {
    const int top = slots - lines;   // the first covered line
    return hovered < top ? hovered : std::max(0, top - 1);
}

struct Field {
    std::string key, label, group, description;
    std::string def, value, draft;     // the declared default, the value in use, and the form's draft
    std::string min, max;              // limits as text, empty when not declared
    std::vector<std::string> choices;  // string options with choices
    Type type = Type::Bool;
    bool restart = false;
    bool bad = false;                  // the draft failed mods_option_check
    std::string reason;

    Kind kind() const {
        if (type == Type::Bool) return Kind::Picker;
        if (type == Type::String) return choices.empty() ? Kind::Text : Kind::Picker;
        return (!min.empty() && !max.empty()) ? Kind::Slider : Kind::Number;
    }
};

inline bool numeric(Type t) { return t == Type::Int || t == Type::Float; }

// Two texts of one option are the same value: numbers by value ("8.0" is "8.00"), the rest as text.
inline bool same(Type t, const std::string& a, const std::string& b) {
    if (numeric(t)) {
        char *ea = nullptr, *eb = nullptr;
        const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
        if (!a.empty() && !b.empty() && !*ea && !*eb) return std::fabs(x - y) < 1e-9;
    }
    return a == b;
}

inline bool dirty(const Field& f) { return !same(f.type, f.draft, f.value); }
inline bool differs_from_default(const Field& f) { return !same(f.type, f.draft, f.def); }

// The loader writes a string value as a quoted TOML string; the form works on the text inside.
inline std::string unquote(const std::string& s) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return s;
    std::string o;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == '\\' && i + 2 < s.size()) {
            ++i;
            switch (s[i]) {
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                default: o += s[i];
            }
        } else {
            o += s[i];
        }
    }
    return o;
}

inline std::vector<std::string> split_choices(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size() && !s.empty()) {
        size_t end = s.find(", ", pos);
        if (end == std::string::npos) end = s.size();
        out.push_back(s.substr(pos, end - pos));
        pos = end + 2;
    }
    return out;
}

// The text for the label: the option's own, or its key in words ("ring_alpha" is "Ring alpha").
inline std::string label_of(const Field& f) {
    if (!f.label.empty()) return f.label;
    std::string s = f.key;
    for (char& c : s) if (c == '_') c = ' ';
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

// A field from what mods_option_get gave (type, texts as the loader prints them).
inline Field make_field(const char* key, const char* type, const char* label, const char* group, const char* description, const char* def,
                        const char* value, const char* min, const char* max, const char* choices, bool restart) {
    Field f;
    f.key = key;
    const std::string t = type;
    f.type = t == "bool" ? Type::Bool : (t == "int" ? Type::Int : (t == "float" ? Type::Float : Type::String));
    f.label = label;
    f.group = group;
    f.description = description;
    f.min = min;
    f.max = max;
    f.restart = restart;
    if (f.type == Type::String) {
        f.def = unquote(def);
        f.value = unquote(value);
        f.choices = split_choices(choices);
    } else {
        f.def = def;
        f.value = value;
    }
    f.draft = f.value;
    return f;
}

inline std::string fmt_float(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}
inline std::string fmt_int(long long v) { return std::to_string(v); }

// The draft as the form shows it: floats with two decimals, a bool as ON or OFF.
inline std::string shown(const Field& f) {
    if (f.type == Type::Bool) return f.draft == "true" ? "ON" : "OFF";
    if (f.type == Type::Float) {
        char* end = nullptr;
        const double v = std::strtod(f.draft.c_str(), &end);
        if (!f.draft.empty() && !*end) return fmt_float(v);
    }
    return f.draft;
}

inline double num(const std::string& s, double fallback) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return (!s.empty() && !*end) ? v : fallback;
}

inline double clamp_to_limits(const Field& f, double v) {
    if (!f.min.empty()) v = std::max(v, num(f.min, v));
    if (!f.max.empty()) v = std::min(v, num(f.max, v));
    return v;
}

inline void set_number(Field& f, double v) {
    v = clamp_to_limits(f, v);
    f.draft = f.type == Type::Int ? fmt_int(std::llround(v)) : fmt_float(v);
}

// The arrows: one step down (-1) or up (+1). A bool toggles, a choice wraps, a number moves by its step and stops at its limits.
inline void step(Field& f, int dir) {
    if (f.type == Type::Bool) {
        f.draft = f.draft == "true" ? "false" : "true";
    } else if (f.type == Type::String) {
        if (f.choices.empty()) return;
        const int n = static_cast<int>(f.choices.size());
        int i = 0;
        for (int k = 0; k < n; ++k) if (f.choices[k] == f.draft) i = k;
        const bool known = f.choices[i] == f.draft;
        f.draft = f.choices[known ? ((i + dir) % n + n) % n : 0];
    } else {
        const double cur = num(f.draft, num(f.def, 0.0));
        double st = 1.0;
        if (f.type == Type::Float) {
            st = f.kind() == Kind::Slider ? std::max(0.01, std::round((num(f.max, 1.0) - num(f.min, 0.0)) * 5.0) / 100.0) : 0.1;
        }
        set_number(f, cur + dir * st);
    }
}

// A click on the track at fraction 0 to 1: the nearest value (an integer, or two decimals).
inline void set_fraction(Field& f, double frac) {
    const double lo = num(f.min, 0.0), hi = num(f.max, 1.0);
    set_number(f, lo + std::clamp(frac, 0.0, 1.0) * (hi - lo));
}

inline double fraction(const Field& f) {
    const double lo = num(f.min, 0.0), hi = num(f.max, 1.0);
    return hi > lo ? std::clamp((num(f.draft, lo) - lo) / (hi - lo), 0.0, 1.0) : 0.0;
}

inline void reset(Field& f) {
    f.draft = f.def;
    f.bad = false;
    f.reason.clear();
}

// Typing into a text field, or into a number once a click gave it the keyboard. `fresh` makes the first character replace the draft.
inline void type_char(Field& f, unsigned cp, bool& fresh) {
    if (f.kind() != Kind::Text && !numeric(f.type)) return;
    if (cp < 32 || cp == 0x7F) return;
    if (numeric(f.type)) {
        const bool ok = (cp >= '0' && cp <= '9') || cp == '-' || cp == '+' || (f.type == Type::Float && (cp == '.' || cp == 'e' || cp == 'E'));
        if (!ok) return;
    }
    if (fresh) { f.draft.clear(); fresh = false; }
    if (f.draft.size() >= kTextMax) return;
    if (cp < 0x80) f.draft += static_cast<char>(cp);
    else if (cp < 0x800) { f.draft += static_cast<char>(0xC0 | (cp >> 6)); f.draft += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        f.draft += static_cast<char>(0xE0 | (cp >> 12));
        f.draft += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        f.draft += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

inline void backspace(Field& f, bool& fresh) {
    if (fresh) { f.draft.clear(); fresh = false; return; }
    while (!f.draft.empty() && (static_cast<unsigned char>(f.draft.back()) & 0xC0) == 0x80) f.draft.pop_back();
    if (!f.draft.empty()) f.draft.pop_back();
}

// `check(key, draft, reason)` is mods_option_check; false sets the red outline and the reason.
using CheckFn = std::function<bool(const Field&, std::string& reason)>;
inline void validate(Field& f, const CheckFn& check) {
    f.bad = false;
    f.reason.clear();
    if (!check) return;
    std::string why;
    if (!check(f, why)) { f.bad = true; f.reason = why.empty() ? "This value is not allowed." : why; }
}

struct Form {
    std::string id;
    std::vector<Field> fields;
    CheckFn check;
    int editing = -1;       // the field that has the keyboard
    bool fresh = false;     // the next typed character replaces the draft

    int changes() const {
        int n = 0;
        for (const Field& f : fields) n += dirty(f) ? 1 : 0;
        return n;
    }
    bool invalid() const {
        for (const Field& f : fields) if (f.bad) return true;
        return false;
    }
    void revalidate(int i) { validate(fields[i], check); }
    void revalidate_all() { for (size_t i = 0; i < fields.size(); ++i) revalidate(static_cast<int>(i)); }
    void discard() {
        for (Field& f : fields) { f.draft = f.value; f.bad = false; f.reason.clear(); }
        editing = -1;
    }
    void arrow(int i, int dir) { step(fields[i], dir); revalidate(i); }
    void to_default(int i) { reset(fields[i]); revalidate(i); if (editing == i) editing = -1; }
    void focus(int i) { editing = i; fresh = true; }
    void blur() { editing = -1; fresh = false; }
    void type(unsigned cp) { if (editing >= 0) { type_char(fields[editing], cp, fresh); revalidate(editing); } }
    void back() { if (editing >= 0) { backspace(fields[editing], fresh); revalidate(editing); } }
};

// The lines of the form, one per row of the details (the glue draws them at the details' line pitch).
struct Row {
    enum Kind { Header, Option, Note } kind = Header;
    int field = -1;          // the option a row belongs to
    std::string text;        // a header's title, a note's text
    int tone = 0;            // a note: 0 grey description, 1 error, 2 warning
};

inline std::string upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// OPTIONS, then the options without a group, then one header per group in the order the groups first appear (the options keep their order inside).
// A note follows an option for its error, for "takes effect at the next start" once changed, and for a short description.
inline std::vector<Row> rows(const Form& fm) {
    std::vector<Row> out;
    if (fm.fields.empty()) return out;
    auto add_option = [&](int i) {
        const Field& f = fm.fields[i];
        out.push_back({Row::Option, i, "", 0});
        if (f.bad) out.push_back({Row::Note, i, f.reason, 1});
        else if (f.restart && dirty(f)) out.push_back({Row::Note, i, "Takes effect at the next start.", 2});
        else if (!f.description.empty() && f.description.size() <= kShortDescription) out.push_back({Row::Note, i, f.description, 0});
    };
    out.push_back({Row::Header, -1, "OPTIONS", 0});
    for (size_t i = 0; i < fm.fields.size(); ++i) if (fm.fields[i].group.empty()) add_option(static_cast<int>(i));
    std::vector<std::string> groups;
    for (const Field& f : fm.fields)
        if (!f.group.empty() && std::find(groups.begin(), groups.end(), f.group) == groups.end()) groups.push_back(f.group);
    for (const std::string& g : groups) {
        out.push_back({Row::Header, -1, upper(g), 0});
        for (size_t i = 0; i < fm.fields.size(); ++i) if (fm.fields[i].group == g) add_option(static_cast<int>(i));
    }
    return out;
}

// Where the parts of one option row sit. `row` is the row's rectangle in window pixels; the label has its left 45 percent.
struct Rect { float x = 0, y = 0, w = 0, h = 0; bool has(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; } };

constexpr float kArrowW = 34.0f, kResetW = 44.0f, kValueW = 74.0f;

struct Geometry {
    Rect control, left, right, value, track, reset;
};

inline Geometry geometry(const Rect& row, Kind kind) {
    Geometry g;
    const float x0 = row.x + row.w * 0.45f;
    g.control = {x0, row.y, row.x + row.w - x0, row.h};
    g.reset = {g.control.x + g.control.w - kResetW, row.y, kResetW, row.h};
    g.left = {g.control.x, row.y, kArrowW, row.h};
    g.right = {g.reset.x - kArrowW, row.y, kArrowW, row.h};
    const Rect mid = {g.left.x + kArrowW, row.y, std::max(0.0f, g.right.x - g.left.x - kArrowW), row.h};
    if (kind == Kind::Slider) {
        g.value = {mid.x, row.y, std::min(kValueW, mid.w), row.h};
        g.track = {mid.x + g.value.w, row.y, std::max(0.0f, mid.w - g.value.w), row.h};
    } else {
        g.value = mid;
        g.track = {};
    }
    return g;
}

inline Zone zone_at(const Rect& row, Kind kind, float px, float py) {
    const Geometry g = geometry(row, kind);
    if (!row.has(px, py)) return Zone::None;
    if (g.reset.has(px, py)) return Zone::Reset;
    if (kind == Kind::Text) return g.value.has(px, py) ? Zone::Value : Zone::None;   // a text field has no arrows
    if (g.left.has(px, py)) return Zone::Left;
    if (g.right.has(px, py)) return Zone::Right;
    if (kind == Kind::Slider && g.track.has(px, py)) return Zone::Track;
    if (g.value.has(px, py)) return (kind == Kind::Picker) ? Zone::None : Zone::Value;
    return Zone::None;
}

// Applies a click on an option row: arrows step, the track sets the value, the value (and a Ctrl+click anywhere on a slider) takes the keyboard.
// Returns true when the draft changed or the keyboard moved.
inline bool click(Form& fm, int i, const Rect& row, float px, float py, bool ctrl) {
    Field& f = fm.fields[i];
    const Kind kind = f.kind();
    const Zone z = zone_at(row, kind, px, py);
    if (z == Zone::None) return false;
    if (z == Zone::Reset) { fm.to_default(i); return true; }
    if (kind == Kind::Text) { fm.focus(i); return true; }
    if (z == Zone::Left || z == Zone::Right) { fm.blur(); fm.arrow(i, z == Zone::Left ? -1 : 1); return true; }
    if (z == Zone::Value || (kind == Kind::Slider && z == Zone::Track && ctrl)) { fm.focus(i); return true; }
    if (z == Zone::Track) {
        const Geometry g = geometry(row, kind);
        fm.blur();
        set_fraction(f, g.track.w > 0.0f ? (px - g.track.x) / g.track.w : 0.0);
        fm.revalidate(i);
        return true;
    }
    return false;
}

}  // namespace form
