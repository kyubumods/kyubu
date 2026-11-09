// The Animator's key inspector and curve view (openspec add-ability-animation, task 6.6): the edits behind the numeric entry, the
// ease picker, Reset and Mirror L/R, and the samples the curve view draws. Pure: no windows.h, no ImGui, no game. app.cpp calls it
// and draws what it returns; tests/unit/test_animator_inspector.cpp runs it without a window.
//
// Every edit is a function of the clip's text and gives the new text, as the edits of timeline.h do, and the loader's validator
// has the last word: an edit that would make an invalid clip changes nothing.
#pragma once
#include <string>
#include <vector>

#include "animation_eval.h"
#include "timeline.h"

namespace kanimator {

// ---------------------------------------------------------------------------------------------------------------------------------
// Mirror L/R

// The part that mirrors `part`: `hand_r` and `hand_l`. False for a part that has no pair (root, body, head, tail).
bool mirror_partner(anim::Part part, anim::Part& other);

// Whether the mirror flips the sign of one axis of a channel. The pair of the figure is the reflection through the plane that
// splits it left from right, x = 0 of the frame in `docs/animations.md` (x toward the player's right). A position flips its x. A
// rotation is a turn about an axis, and a reflection keeps the turn about the axis that is the plane's normal and reverses the
// other two: `rot` keeps x and flips y and z.
bool mirror_flips_axis(anim::Channel channel, int axis);
void mirror_vec(anim::Channel channel, const float in[3], float out[3]);

// Gives `from`'s tracks to its partner, mirrored: the same times and eases, the vectors through mirror_vec, the mode kept. The
// partner's own tracks on a channel `from` has a track on are removed first, so afterwards the partner moves as `from` does on that
// channel; its tracks on the other channels stay. Refused when `from` has no pair or no track. `selection` is the new tracks' keys,
// `made_tracks` their number and `replaced` the keys of the tracks that were removed. The tracks must be `[[track]]` tables.
KeyEditResult mirror_part(const std::string& clip_text, anim::Part from);

// ---------------------------------------------------------------------------------------------------------------------------------
// The key

// The bounds the loader gives a vector component of a channel: the inspector's sliders stop there.
float value_limit(anim::Channel channel);

// Writes the whole vector of one key. A component past the loader's bounds gives the loader's refusal.
KeyEditResult set_key_vec(const std::string& clip_text, KeyRef key, const float v[3]);
// Gives every selected key the ease (the one that shapes the segment arriving at it).
KeyEditResult set_keys_ease(const std::string& clip_text, const Selection& sel, anim::Ease ease);
// Puts every selected key at the neutral value: zero for an `add` track (no offset), the base pose's vector for a `set` track.
KeyEditResult reset_keys(const std::string& clip_text, const Selection& sel, const anim::Pose& base);

// ---------------------------------------------------------------------------------------------------------------------------------
// The curve

// One component of a track at `n` + 1 times from 0 to 1 of the clip, through anim::sample_track: the evaluator the game uses.
// A generator track (osc, spring, expr) needs the clip's duration in ms, and reads the signals given (default: charge 0, speed 0, rank 1).
void curve_samples(const anim::Track& track, int axis, int n, std::vector<float>& out, float duration_ms = 0,
                   const anim::Signals& signals = anim::Signals());

struct CurveRange {
    float lo = -1;
    float hi = 1;
};
// The values the three components of the track take. No ease overshoots its keys, so the keys' own values bound the curve. Padded,
// and at least 1 wide, so a flat track still has a line to draw. A generator has no keys: its range is read from samples
// (`duration_ms` and `signals` as for curve_samples).
CurveRange curve_range(const anim::Track& track, float duration_ms = 0, const anim::Signals& signals = anim::Signals());

}  // namespace kanimator
