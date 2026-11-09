#include "modconfig.h"

#include <cerrno>
#include <cmath>
#include <set>
#include <sstream>

#include "events.h"
#include "loader.h"
#include "log.h"
#include "modmanager.h"
#include "toml_config.h"

namespace kyubu {

const char* opt_type_name(OptType t) {
    switch (t) {
        case OptType::Bool: return "bool";
        case OptType::Int: return "int";
        case OptType::Float: return "float";
        case OptType::String: return "string";
    }
    return "?";
}

namespace {

constexpr size_t kMaxOptionDescription = 120;   // keep in step with form::kMaxDescription in the ModManager

bool valid_option_key(const std::string& k) {
    if (k.empty() || k.size() > 32 || !(k[0] >= 'a' && k[0] <= 'z')) return false;
    for (char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool node_value(const toml::node& n, OptValue* out) {
    if (auto b = n.as_boolean()) { *out = b->get(); return true; }
    if (auto i = n.as_integer()) { *out = (int64_t)i->get(); return true; }
    if (auto f = n.as_floating_point()) { *out = f->get(); return true; }
    if (auto s = n.as_string()) { *out = s->get(); return true; }
    return false;
}

// "a string", "an integer", ... for messages about what a node is.
const char* node_kind(const toml::node& n) {
    switch (n.type()) {
        case toml::node_type::boolean: return "a boolean";
        case toml::node_type::integer: return "an integer";
        case toml::node_type::floating_point: return "a float";
        case toml::node_type::string: return "a string";
        case toml::node_type::array: return "an array";
        case toml::node_type::table: return "a table";
        default: return "a date or time";
    }
}

// The node as it would be written, for messages.
std::string node_text(const toml::node& n) {
    OptValue v;
    if (node_value(n, &v)) return format_value(v);
    if (n.is_table()) return "(a table)";
    std::ostringstream os;
    n.visit([&](auto&& x) { os << x; });
    return os.str();
}

std::string format_float(double d) {
    // the shortest text that reads back as d; exponents only for very large or very small numbers
    char buf[64];
    bool plain = d == 0 || (std::fabs(d) >= 1e-5 && std::fabs(d) < 1e16);
    for (int p = 1; p <= 17; ++p) {
        snprintf(buf, sizeof buf, "%.*g", p, d);
        if (strtod(buf, nullptr) == d && (!plain || !strchr(buf, 'e'))) break;
    }
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) o += fmt("\\u%04X", c);
                else o += (char)c;
        }
    }
    return o + "\"";
}

// Orders two values of one numeric option (both int, or compared as doubles).
int cmp(const OptValue& a, const OptValue& b) {
    if (std::holds_alternative<int64_t>(a) && std::holds_alternative<int64_t>(b)) {
        auto x = std::get<int64_t>(a), y = std::get<int64_t>(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    auto dbl = [](const OptValue& v) {
        return std::holds_alternative<int64_t>(v) ? (double)std::get<int64_t>(v) : std::get<double>(v);
    };
    double x = dbl(a), y = dbl(b);
    return x < y ? -1 : x > y ? 1 : 0;
}

// Converts to the option's type, without checking limits.
bool convert(OptType t, const OptValue& v, OptValue* out) {
    switch (t) {
        case OptType::Bool:
            if (!std::holds_alternative<bool>(v)) return false;
            *out = v;
            return true;
        case OptType::Int:
            if (!std::holds_alternative<int64_t>(v)) return false;
            *out = v;
            return true;
        case OptType::Float:
            if (std::holds_alternative<int64_t>(v)) *out = (double)std::get<int64_t>(v);
            else if (std::holds_alternative<double>(v)) *out = v;
            else return false;
            return std::isfinite(std::get<double>(*out));
        case OptType::String:
            if (!std::holds_alternative<std::string>(v)) return false;
            *out = v;
            return true;
    }
    return false;
}

std::string range_text(const OptionDecl& d, const char* both, const char* lo, const char* hi) {
    if (d.min && d.max) return fmt(both, format_value(*d.min).c_str(), format_value(*d.max).c_str());
    if (d.min) return fmt(lo, format_value(*d.min).c_str());
    if (d.max) return fmt(hi, format_value(*d.max).c_str());
    return {};
}

std::string choices_text(const OptionDecl& d) {
    std::string s;
    for (auto& c : d.choices) s += (s.empty() ? "" : ", ") + quote(c);
    return s;
}

std::string eol_of(const std::string& text) { return text.find("\r\n") != std::string::npos ? "\r\n" : "\n"; }

std::string with_eol(std::string s, const std::string& eol) {
    if (eol == "\n") return s;
    std::string o;
    for (char c : s) o += c == '\n' ? eol : std::string(1, c);
    return o;
}

// End of the TOML value that starts at t[i] (strings, arrays and inline tables may span lines).
size_t value_end(const std::string& t, size_t i) {
    if (i >= t.size()) return i;
    for (const char* q : {"\"\"\"", "'''"}) {
        if (t.compare(i, 3, q) == 0) {
            size_t e = t.find(q, i + 3);
            if (e == std::string::npos) return t.size();
            e += 3;
            while (e < t.size() && t[e] == q[0]) ++e;
            return e;
        }
    }
    if (t[i] == '"') {
        for (size_t j = i + 1; j < t.size(); ++j) {
            if (t[j] == '\\') { ++j; continue; }
            if (t[j] == '"') return j + 1;
            if (t[j] == '\n') return j;
        }
        return t.size();
    }
    if (t[i] == '\'') {
        size_t e = t.find_first_of("'\n", i + 1);
        return e == std::string::npos ? t.size() : t[e] == '\'' ? e + 1 : e;
    }
    if (t[i] == '[' || t[i] == '{') {
        int depth = 0;
        for (size_t j = i; j < t.size(); ++j) {
            char c = t[j];
            if (c == '"' || c == '\'') { j = value_end(t, j) - 1; continue; }
            if (c == '#') { j = t.find('\n', j); if (j == std::string::npos) return t.size(); continue; }
            if (c == '[' || c == '{') ++depth;
            if ((c == ']' || c == '}') && --depth == 0) return j + 1;
        }
        return t.size();
    }
    size_t j = i;
    while (j < t.size() && !strchr("\r\n# \t", t[j])) ++j;
    return j;
}

bool save_text(const std::string& path, const std::string& text) {
    make_dirs(dir_of(path));
    std::string tmp = path + ".tmp";
    if (!write_text(tmp, text)) return false;
    if (MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(widen(tmp).c_str());
    return false;
}

std::string first_run_text(const ModConfig& c) {
    std::string s = "# Options of " + c.mod_name + " (" + c.mod_id + "). Kyubu wrote this file from the mod's config.toml.\n"
                    "# Change the values here: they are kept when the mod is updated. See docs/config.md.\n";
    for (auto& d : c.decls) s += "\n" + option_block(d);
    return s;
}

// Keys the player's file has but the mod does not declare are reported once per process.
bool first_time(const std::string& what) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> g(mu);
    return seen.insert(what).second;
}

std::mutex g_queue_mu;
std::vector<std::pair<Mod*, std::string>> g_queue;   // (mod, key) waiting for config.changed (mods live until exit)

void queue_changed(Mod* m, const std::string& key) {
    if (L().phase.load() < KY_PHASE_REGISTER) return;   // nothing is announced before the mod's init has run
    std::lock_guard<std::mutex> g(g_queue_mu);
    g_queue.emplace_back(m, key);
}

}  // namespace

std::string format_value(const OptValue& v) {
    if (auto b = std::get_if<bool>(&v)) return *b ? "true" : "false";
    if (auto i = std::get_if<int64_t>(&v)) return std::to_string(*i);
    if (auto f = std::get_if<double>(&v)) return format_float(*f);
    return quote(std::get<std::string>(v));
}

std::string describe_allowed(const OptionDecl& d) {
    std::string r;
    switch (d.type) {
        case OptType::Bool: return "true or false";
        case OptType::Int: r = "an integer"; break;
        case OptType::Float: r = "a number"; break;
        case OptType::String: return d.choices.empty() ? "a string" : "one of " + choices_text(d);
    }
    std::string range = range_text(d, "from %s to %s", "of at least %s", "of at most %s");
    return range.empty() ? r : r + " " + range;
}

bool check_value(const OptionDecl& d, const OptValue& v, OptValue* out) {
    OptValue c;
    if (!convert(d.type, v, &c)) return false;
    if (d.min && cmp(c, *d.min) < 0) return false;
    if (d.max && cmp(c, *d.max) > 0) return false;
    if (!d.choices.empty() && std::find(d.choices.begin(), d.choices.end(), std::get<std::string>(c)) == d.choices.end())
        return false;
    if (out) *out = c;
    return true;
}

bool parse_value_text(const OptionDecl& d, const std::string& text, OptValue* out) {
    std::string t = d.type == OptType::String ? text : trim(text);
    OptValue v;
    switch (d.type) {
        case OptType::Bool:
            if (t != "true" && t != "false") return false;
            v = t == "true";
            break;
        case OptType::Int: {
            if (t.empty()) return false;
            char* end = nullptr;
            errno = 0;
            long long n = strtoll(t.c_str(), &end, 10);
            if (*end || errno) return false;
            v = (int64_t)n;
            break;
        }
        case OptType::Float: {
            if (t.empty()) return false;
            char* end = nullptr;
            double f = strtod(t.c_str(), &end);
            if (*end) return false;
            v = f;
            break;
        }
        case OptType::String: v = t; break;
    }
    return check_value(d, v, out);
}

std::string option_block(const OptionDecl& d) {
    std::string s;
    for (auto& line : split(d.description, '\n')) {
        std::string l = trim(line);
        if (!l.empty()) s += "# " + l + "\n";
    }
    std::string limits = opt_type_name(d.type);
    if (d.type == OptType::Int || d.type == OptType::Float) {
        std::string r = range_text(d, "%s to %s", "at least %s", "at most %s");
        if (!r.empty()) limits += ", " + r;
    }
    if (!d.choices.empty()) limits += ", one of " + choices_text(d);
    s += "# " + limits + ", default " + format_value(d.def) + "\n";
    return s + d.key + " = " + format_value(d.def) + "\n";
}

namespace {

// Finds top-level `key`'s value span [*vs, *ve). Returns false when the key is missing; *top_end is then where the
// top level ends (the first table header, or the end of the text).
bool find_key(const std::string& text, const std::string& key, size_t* vs_out, size_t* ve_out, size_t* top_end) {
    size_t pos = 0;
    *top_end = text.size();
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        size_t line_end = nl == std::string::npos ? text.size() : nl;
        size_t next = nl == std::string::npos ? text.size() : nl + 1;
        size_t i = pos;
        while (i < line_end && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i < line_end && text[i] == '[') { *top_end = pos; return false; }   // a table header: top level ends here
        std::string k;
        size_t j = i;
        if (j < line_end && (text[j] == '"' || text[j] == '\'')) {
            size_t e = text.find(text[j], j + 1);
            if (e != std::string::npos && e < line_end) { k = text.substr(j + 1, e - j - 1); j = e + 1; }
        } else {
            while (j < line_end && (isalnum((unsigned char)text[j]) || text[j] == '_' || text[j] == '-')) ++j;
            k = text.substr(i, j - i);
        }
        while (j < line_end && (text[j] == ' ' || text[j] == '\t')) ++j;
        if (!k.empty() && j < line_end && text[j] == '=') {
            size_t vs = j + 1;
            while (vs < line_end && (text[vs] == ' ' || text[vs] == '\t')) ++vs;
            size_t ve = value_end(text, vs);
            if (k == key) { *vs_out = vs; *ve_out = ve; return true; }
            size_t after = text.find('\n', ve);   // a multi-line value: carry on after it
            next = after == std::string::npos ? text.size() : after + 1;
        }
        pos = next;
    }
    return false;
}

// Inserts `block` (whole lines) at the end of the top level, after a blank line.
std::string insert_top_level(const std::string& text, const std::string& block) {
    const std::string eol = eol_of(text);
    size_t vs, ve, at;
    find_key(text, "", &vs, &ve, &at);
    std::string head = text.substr(0, at), tail = text.substr(at);
    if (!head.empty() && head.back() != '\n') head += eol;
    if (!head.empty()) head += eol;
    return head + with_eol(block, eol) + (tail.empty() ? "" : eol) + tail;
}

}  // namespace

std::string rewrite_key(const std::string& text, const std::string& key, const std::string& value) {
    size_t vs, ve, at;
    if (find_key(text, key, &vs, &ve, &at)) return text.substr(0, vs) + value + text.substr(ve);
    const std::string eol = eol_of(text);
    std::string head = text.substr(0, at), tail = text.substr(at);
    if (!head.empty() && head.back() != '\n') head += eol;
    return head + key + " = " + value + eol + (tail.empty() ? "" : eol) + tail;
}


std::optional<std::vector<OptionDecl>> parse_config_decl(const std::string& text, const std::string& src,
                                                         std::vector<std::string>& errors) {
    auto res = toml::parse(text, src);
    if (!res) { errors.push_back(fmt_toml_error(res.error())); return std::nullopt; }
    auto& t = res.table();
    size_t before = errors.size();
    for (auto& [k, v] : t)
        if (k.str() != "options")
            errors.push_back(src + ": key '" + std::string(k.str()) + "': unknown key (config.toml holds [options.<key>] tables)");
    std::vector<std::pair<uint32_t, OptionDecl>> found;
    auto opts = t["options"];
    if (opts && !opts.as_table()) errors.push_back(src + ": key 'options': expected tables [options.<key>]");
    if (auto ot = opts.as_table()) {
        for (auto& [k, node] : *ot) {
            OptionDecl d;
            d.key = std::string(k.str());
            std::string at = src + ": key 'options." + d.key;
            if (!valid_option_key(d.key)) { errors.push_back(at + "': option names must match ^[a-z][a-z0-9_]{0,31}$"); continue; }
            auto o = node.as_table();
            if (!o) { errors.push_back(at + "': expected a table with type and default"); continue; }
            size_t mine = errors.size();
            for (auto& [f, fv] : *o) {
                std::string fs(f.str());
                if (fs != "type" && fs != "default" && fs != "description" && fs != "min" && fs != "max" &&
                    fs != "choices" && fs != "label" && fs != "group" && fs != "restart")
                    errors.push_back(at + "." + fs +
                                     "': unknown field (type, default, description, min, max, choices, label, group, restart)");
            }
            auto type = (*o)["type"].value<std::string>();
            if (!type) { errors.push_back(at + ".type': required: \"bool\", \"int\", \"float\" or \"string\""); continue; }
            if (*type == "bool") d.type = OptType::Bool;
            else if (*type == "int") d.type = OptType::Int;
            else if (*type == "float") d.type = OptType::Float;
            else if (*type == "string") d.type = OptType::String;
            else { errors.push_back(at + ".type': '" + *type + "' is not \"bool\", \"int\", \"float\" or \"string\""); continue; }
            const char* tn = opt_type_name(d.type);
            auto typed = [&](const char* field, OptValue* out) {
                const toml::node* n = o->get(field);
                OptValue raw;
                if (n && node_value(*n, &raw) && convert(d.type, raw, out)) return true;
                if (n) errors.push_back(at + "." + field + "': expected " + (d.type == OptType::Int ? "an " : "a ") + tn +
                                        " (type = \"" + tn + "\"), got " + node_kind(*n));
                return false;
            };
            if (!o->get("default")) errors.push_back(at + ".default': required");
            else typed("default", &d.def);
            if (auto desc = o->get("description")) {
                if (auto s = desc->value<std::string>()) {
                    d.description = *s;
                    if (s->size() > kMaxOptionDescription)   // the Mods page shows it as a hover tooltip that must stay short
                        errors.push_back(at + ".description': " + std::to_string(s->size()) + " characters, at most " +
                                         std::to_string(kMaxOptionDescription) + " (write it for a player who knows nothing about the game's inner workings)");
                } else errors.push_back(at + ".description': expected a string");
            }
            for (const char* f : {"min", "max"}) {
                if (!o->get(f)) continue;
                if (d.type != OptType::Int && d.type != OptType::Float) {
                    errors.push_back(at + "." + f + "': only int and float options have min and max");
                    continue;
                }
                OptValue v;
                if (typed(f, &v)) (strcmp(f, "min") == 0 ? d.min : d.max) = v;
            }
            if (auto ch = o->get("choices")) {
                auto arr = ch->as_array();
                if (d.type != OptType::String) errors.push_back(at + ".choices': only string options have choices");
                else if (!arr || arr->empty()) errors.push_back(at + ".choices': expected a non-empty array of strings");
                else {
                    for (auto& e : *arr) {
                        auto s = e.value<std::string>();
                        if (!s) { errors.push_back(at + ".choices': expected a non-empty array of strings"); break; }
                        if (std::find(d.choices.begin(), d.choices.end(), *s) != d.choices.end())
                            errors.push_back(at + ".choices': \"" + *s + "\" is listed twice");
                        d.choices.push_back(*s);
                    }
                }
            }
            if (auto lbl = o->get("label")) {
                auto s = lbl->value<std::string>();
                if (!s) errors.push_back(at + ".label': expected a string");
                else if (s->size() > 40) errors.push_back(at + ".label': at most 40 characters");
                else d.label = *s;
            }
            if (auto grp = o->get("group")) {
                auto s = grp->value<std::string>();
                if (!s) errors.push_back(at + ".group': expected a string");
                else d.group = *s;
            }
            if (auto rst = o->get("restart")) {
                auto b = rst->value<bool>();
                if (!b) errors.push_back(at + ".restart': expected a bool");
                else d.restart = *b;
            }
            if (errors.size() != mine) continue;
            if (d.min && d.max && cmp(*d.min, *d.max) > 0)
                errors.push_back(at + ".min': " + format_value(*d.min) + " is greater than max " + format_value(*d.max));
            else if (!check_value(d, d.def, nullptr))
                errors.push_back(at + ".default': " + format_value(d.def) + " is not " + describe_allowed(d));
            if (errors.size() != mine) continue;
            found.emplace_back(node.source().begin.line, std::move(d));
        }
    }
    if (errors.size() != before) return std::nullopt;
    std::stable_sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<OptionDecl> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

// ------------------------------------------------------------------ the player's file
const OptionDecl* ModConfig::decl(const std::string& key) const {
    for (auto& d : decls) if (d.key == key) return &d;
    return nullptr;
}

void ModConfig::load(bool write, std::vector<std::string>& warnings) {
    std::lock_guard<std::recursive_mutex> g(mu);
    values.clear();
    from_file.clear();
    for (auto& d : decls) { values[d.key] = d.def; from_file[d.key] = false; }
    file_broken = false;
    auto data = read_file(path);
    if (!data) {
        if (!write) return;
        if (!save_text(path, first_run_text(*this))) { warnings.push_back(display + ": could not be written"); return; }
        for (auto& d : decls) from_file[d.key] = true;   // "source: file" means the file holds the value in use
        return;
    }
    std::string text(data->begin(), data->end());
    auto res = toml::parse(text, display);
    if (!res) {
        file_broken = true;
        warnings.push_back(fmt_toml_error(res.error()) + "; every option of " + mod_id + " uses its default until the file is fixed");
        return;
    }
    auto& t = res.table();
    std::vector<const OptionDecl*> missing;
    for (auto& d : decls) {
        const toml::node* n = t.get(d.key);
        if (!n) { missing.push_back(&d); continue; }
        OptValue raw, v;
        if (node_value(*n, &raw) && check_value(d, raw, &v)) {
            values[d.key] = v;
            from_file[d.key] = true;
        } else {
            warnings.push_back(display + ": " + d.key + " = " + node_text(*n) + " is not allowed (" + describe_allowed(d) +
                               "); using the default " + format_value(d.def));
        }
    }
    for (auto& [k, n] : t) {
        std::string key(k.str());
        if (!decl(key) && first_time(path + "|" + key))
            warnings.push_back(display + ": key '" + key + "' is not an option of " + mod_id + " (any more); it stays in the file, unused");
    }
    if (!write || missing.empty()) return;
    std::string out = text;
    for (auto* d : missing) out = insert_top_level(out, option_block(*d));
    if (!save_text(path, out)) { warnings.push_back(display + ": could not be written"); return; }
    for (auto* d : missing) from_file[d->key] = true;   // the file holds them now, as it will after a restart
}

KyubuResult ModConfig::set(const std::string& key, const OptValue& v, std::string* err, bool* changed) {
    std::lock_guard<std::recursive_mutex> g(mu);
    if (changed) *changed = false;
    const OptionDecl* d = decl(key);
    if (!d) { if (err) *err = mod_id + " has no option '" + key + "'"; return KY_E_NOT_FOUND; }
    OptValue c;
    if (!check_value(*d, v, &c)) {
        if (err) *err = key + " = " + format_value(v) + " is not allowed (" + describe_allowed(*d) + ")";
        return KY_E_INVALID_ARG;
    }
    std::string text;
    if (auto data = read_file(path)) {
        text.assign(data->begin(), data->end());
        if (!toml::parse(text, display)) {
            if (err) *err = display + " is not valid TOML; fix it (or delete it) first";
            return KY_E_UNSAFE;
        }
    } else {
        text = first_run_text(*this);
    }
    std::string out = rewrite_key(text, key, format_value(c));
    auto res = toml::parse(out, display);
    OptValue back;
    const toml::node* n = res ? res.table().get(key) : nullptr;
    if (!n || !node_value(*n, &back) || format_value(back) != format_value(c)) {
        if (err) *err = "could not change " + key + " in " + display + " safely; edit the file by hand";
        return KY_E_UNSAFE;
    }
    if (!save_text(path, out)) { if (err) *err = display + ": could not be written"; return KY_E_INTERNAL; }
    if (changed) *changed = format_value(values[key]) != format_value(c);
    values[key] = c;
    from_file[key] = true;
    return KY_OK;
}

// ------------------------------------------------------------------ per mod
bool config_attach(Mod* m, const std::string& config_dir, std::string* error) {
    m->config.reset();
    // the boot path (load_native) opens the package first; a management caller found a Mod of the
    // boot listing without one, so open it here as the 3.3 manage helpers do
    if (!m->pkg) m->pkg = Package::open(m->path);
    if (!m->pkg || !m->pkg->exists("config.toml")) return true;
    auto text = m->pkg->read("config.toml");
    std::string src = "mods/" + file_name(m->path) + "/config.toml";
    std::vector<std::string> errors;
    auto decls = parse_config_decl(text ? std::string(text->begin(), text->end()) : std::string(), src, errors);
    if (!decls) {
        std::string all;
        for (auto& e : errors) all += (all.empty() ? "" : "; ") + e;
        if (error) *error = all;
        return false;
    }
    if (decls->empty()) return true;
    auto c = std::make_shared<ModConfig>();
    c->mod_id = m->id;
    c->mod_name = m->manifest.name.empty() ? m->id : m->manifest.name;
    c->path = join_path(config_dir, m->id + ".toml");
    c->display = "kyubu/config/" + m->id + ".toml";
    c->decls = std::move(*decls);
    bool existed = file_exists(c->path);
    std::vector<std::string> warnings;
    c->load(true, warnings);
    for (auto& w : warnings) Log::get().write(KY_LOG_WARN, m->id, KY_SIDE_NONE, w);
    if (!existed && file_exists(c->path))
        Log::get().write(KY_LOG_INFO, m->id, KY_SIDE_NONE, fmt("wrote %s with %u option(s)", c->display.c_str(), (unsigned)c->decls.size()));
    m->config = c;
    return true;
}

KyubuResult config_get(Mod* m, const std::string& key, std::optional<OptType> want, OptValue* out, std::string* err) {
    if (!m->config) { if (err) *err = m->id + " declares no options"; return KY_E_NOT_FOUND; }
    ModConfig& c = *m->config;
    std::lock_guard<std::recursive_mutex> g(c.mu);
    const OptionDecl* d = c.decl(key);
    if (!d) { if (err) *err = m->id + " has no option '" + key + "'"; return KY_E_NOT_FOUND; }
    if (want && *want != d->type) {
        if (err) *err = "option '" + key + "' is a " + opt_type_name(d->type) + ", not a " + opt_type_name(*want);
        return KY_E_INVALID_ARG;
    }
    if (out) *out = c.values[key];
    return KY_OK;
}

KyubuResult config_set(Mod* m, const std::string& key, const OptValue& v, std::string* err) {
    if (!m->config) { if (err) *err = m->id + " declares no options"; return KY_E_NOT_FOUND; }
    bool changed = false;
    std::string why;
    KyubuResult r = m->config->set(key, v, &why, &changed);
    if (r != KY_OK) {
        Log::get().write(KY_LOG_WARN, m->id, current_side_tls(), "config: " + why);
        if (err) *err = why;
        return r;
    }
    OptValue now;
    config_get(m, key, std::nullopt, &now, nullptr);
    Log::get().write(KY_LOG_INFO, m->id, current_side_tls(),
                     "config: " + key + " = " + format_value(now) + " (saved to " + m->config->display + ")");
    if (changed) queue_changed(m, key);
    return KY_OK;
}

KyubuResult config_set_text(Mod* m, const std::string& key, const std::string& text, std::string* err) {
    if (!m->config) { if (err) *err = m->id + " declares no options"; return KY_E_NOT_FOUND; }
    const OptionDecl* d = m->config->decl(key);
    if (!d) { if (err) *err = m->id + " has no option '" + key + "'"; return KY_E_NOT_FOUND; }
    OptValue v;
    if (!parse_value_text(*d, text, &v)) {
        std::string why = key + " = " + text + " is not allowed (" + describe_allowed(*d) + ")";
        Log::get().write(KY_LOG_WARN, m->id, current_side_tls(), "config: " + why);
        if (err) *err = why;
        return KY_E_INVALID_ARG;
    }
    return config_set(m, key, v, err);
}

std::vector<std::string> config_reload(Mod* m) {
    if (!m->config) return {m->id + " declares no options"};
    ModConfig& c = *m->config;
    std::lock_guard<std::recursive_mutex> g(c.mu);
    auto before = c.values;
    std::vector<std::string> lines;
    c.load(false, lines);
    for (auto& w : lines) Log::get().write(KY_LOG_WARN, m->id, KY_SIDE_NONE, w);
    int n = 0;
    for (auto& d : c.decls) {
        if (format_value(before[d.key]) == format_value(c.values[d.key])) continue;
        ++n;
        lines.push_back("  " + d.key + " = " + format_value(c.values[d.key]) + " (was " + format_value(before[d.key]) + ")");
        queue_changed(m, d.key);
    }
    lines.push_back(fmt("%s: reloaded %s, %d option(s) changed", m->id.c_str(), c.display.c_str(), n));
    Log::get().write(KY_LOG_INFO, m->id, KY_SIDE_NONE, lines.back());
    return lines;
}

void config_pump() {
    std::vector<std::pair<Mod*, std::string>> q;
    {
        std::lock_guard<std::mutex> g(g_queue_mu);
        q.swap(g_queue);
    }
    std::set<std::pair<Mod*, std::string>> done;
    for (auto& item : q) {
        if (!done.insert(item).second) continue;   // one event per option and pump
        Mod* m = item.first;
        if (!m->active()) continue;
        KyubuConfigChangedData d{m->id.c_str(), item.second.c_str()};
        for (KyubuSide s : {KY_SIDE_CLIENT, KY_SIDE_SERVER})
            if (m->has_side(s) && side_active(s)) Events::get().emit("config.changed", s, &d, sizeof d);
    }
}

}  // namespace kyubu
