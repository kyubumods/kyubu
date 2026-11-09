// The words of `anim capture <clip> [times_ms] [views]` (openspec add-ability-animation, task 7.3; design D11), of `anim rig` and of
// `anim bake <pose id> [duration_ms]` (task 7.4; design D12). Pure: no game and no loader, so a unit test builds it
// (tests/unit/test_anim_capture_sheet.cpp).
#pragma once
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace kyubu::anim_capture {

// The two fixed views of D11. The camera looks along +y whatever the player's yaw (mapping atlas, creatures), so body_yaw 180 shows the
// face and 90 the left profile. The Animator's Front and Left views are the same two.
struct CaptureView {
    const char* name;
    float body_yaw;   // degrees counter-clockwise from +y
};
constexpr CaptureView kViews[2] = {{"front", 180.0f}, {"left", 90.0f}};
constexpr int kViewCount = 2;
constexpr size_t kMaxTimes = 12;

constexpr const char* kCaptureUsage = "usage: anim capture <clip> [times_ms] [views]   e.g. anim capture demo:slam 0,250,500 front,left";

struct CaptureArgs {
    bool ok = false;
    std::string clip;
    std::vector<double> times_ms;   // 0 or more, in the order given; default 0, 500 and 1000
    std::vector<int> views;         // indexes of kViews, in the order given; default both
    std::string error;              // when !ok: the line to print
};

namespace detail {
inline std::vector<std::string> split_words(const std::string& s, const char* seps) {
    std::vector<std::string> out;
    size_t at = 0;
    while (true) {
        at = s.find_first_not_of(seps, at);
        if (at == std::string::npos) break;
        const size_t end = s.find_first_of(seps, at);
        out.push_back(s.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos) break;
        at = end;
    }
    return out;
}
// A list with commas and nothing else between the items; an empty item ("1,,2", a trailing comma) is an error.
inline bool split_list(const std::string& word, std::vector<std::string>* items) {
    size_t at = 0;
    while (true) {
        const size_t comma = word.find(',', at);
        const std::string item = word.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (item.empty()) return false;
        items->push_back(item);
        if (comma == std::string::npos) return true;
        at = comma + 1;
    }
}
}  // namespace detail

// `args` is what follows "anim capture": a clip id, then optionally a comma list of times in milliseconds (the unit of `anim preview`),
// then optionally a comma list of view names. Times may repeat; a view may not.
inline CaptureArgs parse_capture_args(const std::string& args) {
    CaptureArgs p;
    const std::vector<std::string> words = detail::split_words(args, " \t\r\n");
    if (words.empty() || words.size() > 3) {
        p.error = kCaptureUsage;
        return p;
    }
    p.clip = words[0];
    if (words.size() >= 2) {
        std::vector<std::string> items;
        if (!detail::split_list(words[1], &items)) {
            p.error = "'" + words[1] + "' is not a list of times (" + kCaptureUsage + ")";
            return p;
        }
        if (items.size() > kMaxTimes) {
            p.error = "at most " + std::to_string(kMaxTimes) + " times in one capture";
            return p;
        }
        for (const std::string& it : items) {
            char* stop = nullptr;
            const double t = std::strtod(it.c_str(), &stop);
            if (stop == it.c_str() || *stop != 0 || !std::isfinite(t) || t < 0) {
                p.error = "'" + it + "' is not a time: give milliseconds, 0 or more (" + kCaptureUsage + ")";
                return p;
            }
            p.times_ms.push_back(t);
        }
    } else {
        p.times_ms = {0.0, 500.0, 1000.0};
    }
    if (words.size() == 3) {
        std::vector<std::string> items;
        if (!detail::split_list(words[2], &items)) {
            p.error = "'" + words[2] + "' is not a list of views (" + kCaptureUsage + ")";
            return p;
        }
        for (const std::string& it : items) {
            int found = -1;
            for (int v = 0; v < kViewCount; ++v)
                if (it == kViews[v].name) found = v;
            if (found < 0) {
                p.error = "no view '" + it + "': the views are front and left";
                return p;
            }
            for (int seen : p.views)
                if (seen == found) {
                    p.error = "the view '" + it + "' is given twice";
                    return p;
                }
            p.views.push_back(found);
        }
    } else {
        p.views = {0, 1};
    }
    p.ok = true;
    return p;
}

// ---- anim rig, anim bake (task 7.4)
constexpr const char* kRigUsage = "usage: anim rig   (no arguments: the local player stands still)";

// `anim rig` takes nothing. Returns "" when `args` is blank, else the usage line.
inline std::string rig_args_error(const std::string& args) {
    return detail::split_words(args, " \t\r\n").empty() ? std::string() : std::string(kRigUsage);
}

constexpr int kMaxPoseId = 73;               // the game draws pose ids 0 to 73 (docs/animations.md, "Borrowing a vanilla pose")
constexpr double kBakeDefaultMs = 2000;
constexpr double kBakeMinMs = 100;
constexpr double kBakeMaxMs = 8000;          // 30 keys a second must fit the 256 keys of a track, with the last frame
constexpr const char* kBakeUsage = "usage: anim bake <pose id 0-73> [duration_ms]   e.g. anim bake 18 1500";

struct BakeArgs {
    bool ok = false;
    int pose = 0;
    double duration_ms = kBakeDefaultMs;
    std::string error;                       // when !ok: the line to print
};

inline BakeArgs parse_bake_args(const std::string& args) {
    BakeArgs p;
    const std::vector<std::string> words = detail::split_words(args, " \t\r\n");
    if (words.empty() || words.size() > 2) {
        p.error = kBakeUsage;
        return p;
    }
    char* stop = nullptr;
    const long id = std::strtol(words[0].c_str(), &stop, 10);
    if (stop == words[0].c_str() || *stop != 0) {
        p.error = "'" + words[0] + "' is not a pose id (" + kBakeUsage + ")";
        return p;
    }
    if (id < 0 || id > kMaxPoseId) {
        p.error = "pose id " + words[0] + " is out of range: the game draws pose ids 0 to " + std::to_string(kMaxPoseId);
        return p;
    }
    p.pose = int(id);
    if (words.size() == 2) {
        const double d = std::strtod(words[1].c_str(), &stop);
        if (stop == words[1].c_str() || *stop != 0 || !std::isfinite(d)) {
            p.error = "'" + words[1] + "' is not a duration: give milliseconds (" + kBakeUsage + ")";
            return p;
        }
        if (d < kBakeMinMs || d > kBakeMaxMs) {
            p.error = "the duration must be " + std::to_string(int(kBakeMinMs)) + " to " + std::to_string(int(kBakeMaxMs)) + " ms";
            return p;
        }
        p.duration_ms = d;
    }
    p.ok = true;
    return p;
}

}  // namespace kyubu::anim_capture
