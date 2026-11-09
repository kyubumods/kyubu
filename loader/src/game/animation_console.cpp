#include "animation_console.h"

#include <cstdio>
#include <cstring>

#include "kanim_core.h"

namespace kyubu::game::anim {
namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        out.push_back(text.substr(at, nl - at));
        at = nl + 1;
    }
    return out;
}

std::string fmt_ms(float ms) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", (double)ms);
    return b;
}

std::vector<std::string> list_lines(const ClipSet& set) {
    if (set.clips.empty()) return {"no clips (put <name>.anim.toml in assets/animations/ of an enabled mod, then anim reload)"};
    std::vector<std::string> out;
    char b[96];
    std::snprintf(b, sizeof b, "%u clip(s), hash %.12s:", (unsigned)set.clips.size(), set.hash.c_str());
    out.push_back(b);
    for (auto& [id, e] : set.clips) {
        const Clip& c = e->clip;
        const std::string dur = c.duration_from_ability ? "ability" : fmt_ms(c.duration_ms) + " ms";
        std::snprintf(b, sizeof b, "%u track(s)", (unsigned)c.tracks.size());
        out.push_back("  " + id + "  " + dur + "  " + b + "  " + e->file);
    }
    return out;
}

constexpr const char* kAnimExtensionTag = ":anim.";

bool is_builtin_sub(const std::string& sub) { return sub == "list" || sub == "show" || sub == "sample" || sub == "reload"; }

}  // namespace

std::vector<AnimExtension> anim_extensions(const std::vector<std::string>& command_names) {
    std::vector<AnimExtension> out;
    for (const std::string& name : command_names) {
        const size_t at = name.find(kAnimExtensionTag);
        if (at == std::string::npos || at == 0 || name.find(':') != at) continue;   // `<id>:anim.<sub>`, the colon is the mod id's
        const std::string sub = name.substr(at + std::strlen(kAnimExtensionTag));
        if (sub.empty() || sub.find_first_of(" \t") != std::string::npos || is_builtin_sub(sub)) continue;
        out.push_back({sub, name});
    }
    return out;
}

void split_anim_args(const std::string& args, std::string& sub, std::string& rest) {
    const std::string a = trim(args);
    const size_t sp = a.find_first_of(" \t");
    sub = a.substr(0, sp);
    rest = sp == std::string::npos ? "" : trim(a.substr(sp));
}

std::string anim_extension_command(const std::vector<std::string>& command_names, const std::string& sub) {
    for (const AnimExtension& e : anim_extensions(command_names))
        if (e.sub == sub) return e.command;
    return "";
}

std::vector<std::string> anim_command_lines(const ClipSet& set, const std::string& args) {
    const std::string a = trim(args);
    const size_t sp = a.find_first_of(" \t");
    const std::string sub = a.substr(0, sp);
    const std::string rest = sp == std::string::npos ? "" : trim(a.substr(sp));
    if (sub == "list" && rest.empty()) return list_lines(set);
    if (sub != "show" && sub != "sample") return {kAnimUsage};

    const size_t sp2 = rest.find_first_of(" \t");
    const std::string id = rest.substr(0, sp2);
    const std::string tail = sp2 == std::string::npos ? "" : trim(rest.substr(sp2));
    if (id.empty() || (sub == "show" && !tail.empty())) return {kAnimUsage};
    const ClipEntry* e = set.find(id);
    if (!e) return {"no clip '" + id + "' (anim list shows the loaded clips)"};

    kanim::Options o;
    if (sub == "sample" && !tail.empty() && !kanim::parse_times(tail, o.times))
        return {"'" + tail + "' is not a list of times: use fractions of the clip from 0 to 1, like 0,0.25,1"};
    const kanim::Result r = sub == "show" ? kanim::describe(e->text, id, o) : kanim::sample(e->text, id, o);
    return split_lines(r.text);
}

}  // namespace kyubu::game::anim
