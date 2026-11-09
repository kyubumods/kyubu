// The commands of `kanim`, the clip checker (openspec add-ability-animation, design D11).
// Text in, text out: no file access, no game, no windows.h. main.cpp reads the file and prints; the unit tests call these
// functions and compare with golden files. Every command parses with `parse_clip` and evaluates with `apply_clip`, the loader's
// own source, so `kanim` and the game cannot disagree.
#pragma once
#include <string>
#include <vector>

#include "animation_load.h"

namespace kyubu::kanim {

// What a command prints and how the process ends: 0 = done (a `check` of a good clip too), 1 = the clip is invalid (the errors are
// in `text`, as text or as the JSON of `errors_to_json`).
struct Result {
    int code = 0;
    std::string text;
};

struct Options {
    bool json = false;                 // `check`: the JSON form with a fix hint
    float ability_ms = 0;              // the bound ability's duration; 0 = not given
    std::vector<float> times;          // `sample`: fractions of the clip (0..1)
    game::anim::Signals signals;       // what an `expr` track reads from the game (--charge, --speed, --rank): 0, 0 and 1 when not given
};

// The duration of 1000 ms that `sample` and `describe` assume for `duration = "ability"` when `--ability-ms` is not given.
constexpr float kAssumedAbilityMs = 1000.0f;

// `check`: the validator. A good clip prints "<source>: ok" (JSON: {"ok":true,"errors":[]}).
Result check(const std::string& text, const std::string& source, const Options& o);
// `describe`: range per part, the fastest motion, a pop at the start or end, the duration against the ability's, the hide
// intervals and the markers.
Result describe(const std::string& text, const std::string& source, const Options& o);
// `sample`: the evaluated pose of each animated part at each time, from the base pose of all zeros.
Result sample(const std::string& text, const std::string& source, const Options& o);
// `bake`: the clip as plain TOML with every key written out. A generator track (`osc`, `spring`, `expr`) is expanded into keys, one
// per 1/60 s of the clip, over the duration (`--ability-ms` for "ability") and with the signals of the options. Comments of the
// source are not kept.
Result bake(const std::string& text, const std::string& source, const Options& o);

// The slope of `apply_ease` at f (0..1), by the easing's own formula; `describe` uses it for the fastest motion. The unit test
// compares it with differences of `apply_ease`.
double ease_slope(game::anim::Ease e, double f);

// `--t 0,0.25,0.5` to times. False when an item is not a number in 0..1.
bool parse_times(const std::string& s, std::vector<float>& out);

}  // namespace kyubu::kanim
