// The words of `anim preview`, and the line the console answers with (openspec add-ability-animation, task 7.1). Pure: no game and no
// loader, so a unit test builds it (tests/unit/test_anim_capture.cpp).
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "kyubu_api.h"

namespace kyubu::anim_capture {

constexpr const char* kPreviewUsage = "usage: anim preview <clip> <t_ms> | anim preview off";

struct PreviewArgs {
    enum class Kind { Show, Off, Error } kind = Kind::Error;
    std::string clip;      // Show
    double t_ms = 0;       // Show
    std::string error;     // Error: the line to print
};

// `args` is what follows "anim preview". `off` ends the preview; otherwise two words: a clip id and a time in milliseconds (0 or more,
// finite, as the whole word).
inline PreviewArgs parse_preview_args(const std::string& args) {
    PreviewArgs p;
    std::string words[3];
    size_t n = 0, at = 0;
    while (true) {
        at = args.find_first_not_of(" \t\r\n", at);
        if (at == std::string::npos) break;
        const size_t end = args.find_first_of(" \t\r\n", at);
        if (n < 3) words[n] = args.substr(at, end == std::string::npos ? std::string::npos : end - at);
        ++n;
        if (end == std::string::npos) break;
        at = end;
    }
    if (n == 1 && words[0] == "off") {
        p.kind = PreviewArgs::Kind::Off;
        return p;
    }
    if (n != 2) {
        p.error = kPreviewUsage;
        return p;
    }
    char* stop = nullptr;
    const double t = std::strtod(words[1].c_str(), &stop);
    if (stop == words[1].c_str() || *stop != 0 || !std::isfinite(t) || t < 0) {
        p.error = "'" + words[1] + "' is not a time: give milliseconds, 0 or more (" + kPreviewUsage + ")";
        return p;
    }
    p.kind = PreviewArgs::Kind::Show;
    p.clip = words[0];
    p.t_ms = t;
    return p;
}

// The console line for the result of the call that `p` asked for (KyubuResult as an int).
inline std::string preview_reply(const PreviewArgs& p, int result) {
    if (p.kind == PreviewArgs::Kind::Error) return p.error;
    char b[64];
    if (p.kind == PreviewArgs::Kind::Off) {
        if (result == KY_OK) return "anim preview: off, the game draws the player again";
        std::snprintf(b, sizeof b, "%d", result);
        return std::string("anim preview off: the call failed (result ") + b + ", see the log)";
    }
    switch (result) {
        case KY_OK:
            std::snprintf(b, sizeof b, "%g", p.t_ms);
            return "anim preview: " + p.clip + " at " + b + " ms on the local player (`anim preview off` ends it)";
        case KY_E_NOT_FOUND: return "anim preview: no clip '" + p.clip + "' (anim list shows the loaded clips)";
        case KY_E_DEV_ONLY: return "anim preview: needs dev mode";
        case KY_E_TRUST: return "anim preview: the mod needs trust \"full\"";
        case KY_E_UNAVAILABLE: return "anim preview: this loader has no animation_preview";
        default:
            std::snprintf(b, sizeof b, "%d", result);
            return std::string("anim preview: the call failed (result ") + b + ", see the log)";
    }
}

}  // namespace kyubu::anim_capture
