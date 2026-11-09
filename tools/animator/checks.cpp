// The Animator's checks: see checks.h.
#include "checks.h"

#include <cstdio>

#include "animation_load.h"

namespace kanimator {

namespace anim = kyubu::game::anim;

namespace {

// The 1-based line of the first line of `text` whose first word is `key` and that goes on with '='. 0 when there is none.
int line_of_key(const std::string& text, const char* key) {
    const size_t klen = std::char_traits<char>::length(key);
    int line = 1;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        size_t i = pos;
        while (i < end && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (text.compare(i, klen, key) == 0) {
            size_t j = i + klen;
            while (j < end && (text[j] == ' ' || text[j] == '\t')) ++j;
            if (j < end && text[j] == '=') return line;
        }
        if (end >= text.size()) break;
        pos = end + 1;
        ++line;
    }
    return 0;
}

std::string ms_text(float ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", ms);
    return buf;
}

}  // namespace

std::vector<Check> run_checks(const std::string& text, const std::string& source, float ability_ms, const Rig* rig) {
    std::vector<Check> out;
    anim::ClipLoad cl = anim::parse_clip(text, source);
    if (!cl.ok) {
        for (const anim::ClipError& e : cl.errors) {
            Check c;
            c.severity = Severity::Error;
            c.line = e.line;
            c.path = e.path;
            c.code = e.code;
            c.message = e.message;
            c.hint = e.hint;
            out.push_back(std::move(c));
        }
        return out;   // the other two checks read a clip that is valid
    }
    const anim::Clip& clip = cl.clip;

    if (ability_ms > 0 && !clip.duration_from_ability && clip.duration_ms > ability_ms) {
        Check c;
        c.severity = Severity::Warning;
        c.line = line_of_key(text, "duration");
        c.path = "duration";
        c.code = "longer_than_ability";
        c.message = "the clip lasts " + ms_text(clip.duration_ms) + " ms and the ability's length is " + ms_text(ability_ms) +
                    " ms: the ability ends first, so the last " + ms_text(clip.duration_ms - ability_ms) + " ms are never drawn";
        c.hint = "shorten the clip, or write duration = \"ability\" to follow the ability's length";
        out.push_back(std::move(c));
    }

    if (rig) {
        const uint32_t used = rig->parts_used();
        uint32_t reported = 0;
        for (size_t i = 0; i < clip.tracks.size(); ++i) {
            const anim::Track& t = clip.tracks[i];
            const uint32_t bit = 1u << static_cast<int>(t.part);
            if ((used & bit) || (reported & bit)) continue;
            reported |= bit;
            Check c;
            c.severity = Severity::Warning;
            c.line = t.line;
            c.path = "track[" + std::to_string(i) + "].part";
            c.code = "rig_lacks_part";
            c.message = std::string("the rig '") + rig->name + "' has no node for the part " + anim::part_name(t.part) +
                        ": its tracks move nothing in this view";
            c.hint = "pose another part, or open a rig that has a node for it";
            out.push_back(std::move(c));
        }
    }
    return out;
}

std::string format_check(const Check& c) {
    std::string s;
    if (c.line > 0) s += "line " + std::to_string(c.line) + ": ";
    if (!c.path.empty()) s += c.path + ": ";
    return s + c.message;
}

size_t count_checks(const std::vector<Check>& checks, Severity s) {
    size_t n = 0;
    for (const Check& c : checks)
        if (c.severity == s) ++n;
    return n;
}

}  // namespace kanimator
