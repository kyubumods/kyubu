// The Animator's checks panel (openspec add-ability-animation, task 6.7). One function: the clip's text in, a list of findings out.
// Pure: no windows.h, no ImGui. The findings are the loader's validator (anim::parse_clip, linked in, so the Animator says what the
// game would say) and two of the Animator's own, which the loader cannot make: a clip longer than the ability's length that the
// author typed in the window, and a part the rig has no node for.
#pragma once
#include <string>
#include <vector>

#include "rig.h"

namespace kanimator {

enum class Severity : uint8_t { Error, Warning };

struct Check {
    Severity severity = Severity::Error;
    int line = 0;           // 1-based; 0 when the finding is about the whole clip
    std::string path;       // field path, as the validator's: "track[1].part", "duration"; may be empty
    // The validator's code (unknown_part, keys_out_of_order, ...), or one of the Animator's own: `longer_than_ability`, `rig_lacks_part`.
    std::string code;
    std::string message;    // one sentence
    std::string hint;       // how to fix it; may be empty
};

// Runs the checks on `text`. `source` labels the validator's errors (the clip's name). `ability_ms` is the ability's length typed in
// the window; 0 or less means none was typed, and the length check does not run. `rig` may be null (no rig: its check does not run).
// Errors come first, in the validator's order; warnings follow in the order of the clip. Never throws.
std::vector<Check> run_checks(const std::string& text, const std::string& source, float ability_ms, const Rig* rig);

// "line 12: track[1].part: message" (the line and the path are left out when they are empty). Without the hint.
std::string format_check(const Check& c);

// The number of findings of one severity.
size_t count_checks(const std::vector<Check>& checks, Severity s);

}  // namespace kanimator
