// The TOML loader and the validator of ability clips (openspec add-ability-animation, design D2).
// Text in, a `Clip` (animation_eval.h) or a list of errors out. No file access, no game, no Lua: the registry (3.3), `kanim` (3.6)
// and the editor's text path (3.5) all call `parse_clip`, so they report the same messages.
//
// A clip is rejected as a whole: `ok` is false and `clip` is empty whenever `errors` is not. Every error carries the source name,
// the line (0 when the text has none) and the field path, such as `track[1].keys[2].t`. The checker's JSON form adds a fix hint.
// Every error of a clip is listed (up to kMaxErrors), in the order of the lines.
//
// Rules the design leaves open (recorded in the notes of 3.2):
//  - `duration` is required, and a number must be above 0. Every other top-level field is optional.
//  - A key not named by the format is an error, in every table: a misspelled `ease` must not be dropped in silence.
//  - Two keys may share a `t` (a jump); a key with a lower `t` than the one before it is out of order.
//  - A track needs one key or more, or one generator (`osc`, `spring` or `expr`, design D11) and no keys. A hide interval needs `to`
//    above `from`, and one group or more.
//  - `[borrow]` takes a `speed` and an `offset_ms` below 0 too: the evaluator keeps the borrowed time at 0 or above.
//
// Not checked here: `duration = "ability"` against the ability it is bound to. That needs the ability's duration, so the
// binding (3.4) reports it.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "animation_eval.h"

namespace kyubu::game::anim {

// Bounds of a valid clip. A clip over any of them is rejected.
constexpr int kMaxTracks = 64;
constexpr int kMaxKeysPerTrack = 256;
constexpr int kMaxHides = 32;
constexpr int kMaxMarkers = 64;
constexpr int kMaxMarkerName = 48;
constexpr float kMaxDurationMs = 600000.0f;   // ten minutes
constexpr float kMaxBlendMs = 60000.0f;
// kMaxPos and kMaxRotDeg (voxels and degrees, any axis) are in animation_eval.h: the evaluator holds a generator to them.
constexpr float kMaxOscHz = 30.0f;            // a frequency above half the draw rate only aliases
constexpr float kMaxDecayPerS = 100.0f;
constexpr float kMaxDampingRatio = 4.0f;
constexpr uint32_t kMaxBorrowMode = 110;      // above this the game draws pose 0
constexpr float kMaxBorrowSpeed = 10.0f;
constexpr float kMaxBorrowOffsetMs = kMaxDurationMs;
constexpr size_t kMaxTextBytes = 4u << 20;    // the text of one clip; the largest valid clip is about 1 MB
constexpr int kMaxErrors = 32;                // errors listed for one clip; the ones after are dropped

struct ClipError {
    std::string source;     // the file name (or any label) given to parse_clip
    int line = 0;           // 1-based; 0 when the error is about the whole clip
    std::string path;       // field path: "duration", "track[1].part", "track[0].keys[2].t", "hide[0].parts[1]"
    // Stable id. One of: toml_syntax, too_large, unknown_key, missing_field, bad_type, bad_length, not_finite, out_of_range,
    // unknown_duration, unknown_end, unknown_part, unknown_channel, unknown_mode, unknown_ease, unknown_group,
    // keys_out_of_order, no_keys, no_parts, empty_interval, bad_name, too_many_tracks, too_many_keys, too_many_hides,
    // too_many_markers, two_sources; and for an `expr`: bad_expression, unknown_name, unknown_function, bad_arg_count, expr_too_big
    // (each with a `column`).
    std::string code;
    std::string message;    // one sentence, without the source, line or path
    std::string hint;       // how to fix it: the checker's `--json` form carries it
    int column = 0;         // 1-based, in the text of an `expr`; 0 for any other error (the JSON form leaves it out)
};

struct ClipLoad {
    bool ok = false;
    Clip clip;
    std::vector<ClipError> errors;
};

// Parses and validates one clip. `source` labels the errors (a file name). Never throws.
ClipLoad parse_clip(const std::string& text, const std::string& source);

// "source:line: path: message". The line is left out when it is 0, the path when it is empty (a syntax error). The loader's
// log line.
std::string format_error(const ClipError& e);

// The checker's JSON: {"ok":bool,"errors":[{"source","line","path","code","message","hint"}]}, one line.
std::string errors_to_json(const ClipLoad& r);

}  // namespace kyubu::game::anim
