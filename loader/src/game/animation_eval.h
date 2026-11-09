// The clip model and the pure evaluator of ability animation (openspec add-ability-animation, design D1, D5, D9).
// A clip and a time go in, an adjustment of a base pose comes out. No game, no hooks, no Lua, no ImGui, no windows.h: the
// loader, the validator (3.2), `kanim` (3.6) and the Animator's curve view all link this one source, so they cannot disagree.
//
// Two rules fix what a key means (design D1 leaves them open; recorded in the notes of 3.1):
//  - The `ease` of a key shapes the segment that ARRIVES at it, from the previous key. The first key's ease is unused.
//    `hold` keeps the previous key's value until the arriving key's `t`, then jumps.
//  - `blend_out` is measured back from the end of the RUN (`Timing::run_ms`), not from the end of the clip, because the
//    game draws its own pose again the moment the mode ends, and a held clip may be shorter than the ability.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "animation_expr.h"

namespace kyubu::game::anim {

// The parts of a body a track can move: the six that have a pose vector in the game's draw record.
// NOT parts: `foot_r`, `foot_l`, `shoulder_r`, `shoulder_l`, `wings`. The game
// draws them, but moves them from the walk phase and the turn twist, with no vector to write. To pose them, map a second
// write site (each part's matrix block in draw_creature) first, then add the names back here and in kPartNames.
// A weapon has no vector of its own: it follows its hand. So `weapon_r` and `weapon_l` are accepted as other names of
// `hand_r` and `hand_l`, and part_name gives the hand's name back.
enum class Part : uint8_t {
    Root, Body, Head, HandR, HandL, Tail,
    Count
};
constexpr int kPartCount = static_cast<int>(Part::Count);
const char* part_name(Part p);                      // "root", "body", "head", "hand_r", ...
bool part_from_name(const std::string& s, Part& out);   // also "weapon_r" (HandR) and "weapon_l" (HandL)

enum class Channel : uint8_t { Pos, Rot };         // pos: x y z in voxels; rot: x y z in degrees, Euler per axis
const char* channel_name(Channel c);
bool channel_from_name(const std::string& s, Channel& out);

enum class TrackMode : uint8_t { Add, Set };       // add: an offset on the game's base pose (the default); set: the clip alone
enum class Ease : uint8_t { Linear, Smooth, EaseIn, EaseOut, Hold };
const char* ease_name(Ease e);
bool ease_from_name(const std::string& s, Ease& out);
// The shaped fraction of a segment: `f` in 0..1 (clamped) in, 0..1 out. Hold gives 0 for every f below 1.
float apply_ease(Ease e, float f);

enum class End : uint8_t { Hold, Loop };

struct Key {
    float t = 0;                 // 0..1 of the clip's duration
    float v[3] = {0, 0, 0};
    Ease ease = Ease::Linear;    // shapes the segment that arrives at this key
    int line = 0;                // source line, for the validator's messages (0 when built in code)
};

// The bound of a value, any axis: a key or a generator's result. The validator rejects a key beyond it; a generator is held to it.
constexpr float kMaxPos = 100.0f;             // voxels
constexpr float kMaxRotDeg = 720.0f;          // degrees

// Where a track's value comes from (design D11, "Procedural tracks", milestone F). A generator is data, not code: a few numbers or one
// bounded expression per axis, so a clip that has one is a data file at every trust level. A generator track has no keys.
enum class Source : uint8_t { Keys, Osc, Spring, Expr };
const char* source_name(Source s);                  // "keys", "osc", "spring", "expr"

// `osc`: centre + amp * sin(2 pi freq tau + phase) * e^(-decay tau), per axis, tau in seconds from the start of the clip's lap.
struct Osc {
    float centre[3] = {0, 0, 0};
    float amp[3] = {0, 0, 0};
    float freq_hz = 1;           // cycles per second, above 0
    float phase_deg = 0;         // degrees of the cycle at tau 0: 90 starts at the top
    float decay = 0;             // 1/s: the amplitude is multiplied by e^(-decay tau); 0 never settles
};
// `spring`: the value sits at `from` until `at`, then moves to `to` as a damped spring does from rest (a strike's overshoot and
// settle). The closed form of the step response, so any time can be asked for with no state: seeking and scrubbing agree.
struct Spring {
    float from[3] = {0, 0, 0};
    float to[3] = {0, 0, 0};
    float at = 0;                // 0..1 of the duration, where the spring is let go
    float freq_hz = 2;           // the undamped natural frequency, above 0
    float damping = 0.3f;        // the damping ratio: under 1 overshoots and rings, 1 is critical, above 1 creeps
};

struct Track {
    Part part = Part::Root;
    Channel channel = Channel::Rot;
    TrackMode mode = TrackMode::Add;
    Source source = Source::Keys;
    std::vector<Key> keys;       // in order of t (the validator checks it; the evaluator copes with ties); empty for a generator
    Osc osc;                     // source == Osc
    Spring spring;               // source == Spring
    ExprProgram expr[3];         // source == Expr: one program per axis
    int line = 0;
};

// Groups of the game's hide mask (design D1). The bits are the loader's own; the provider (4.x) maps them to the game's.
enum HideGroup : uint32_t {
    kHideHead = 1u << 0, kHideHair = 1u << 1, kHideBody = 1u << 2, kHideShoulders = 1u << 3, kHideWeapons = 1u << 4,
};
const char* hide_group_name(uint32_t bit);          // one bit only; "?" for anything else
bool hide_group_from_name(const std::string& s, uint32_t& bit);

struct Hide {
    float from = 0, to = 0;      // 0..1 of the duration
    uint32_t groups = 0;         // HideGroup bits
    int line = 0;
};

struct Marker {
    float t = 0;                 // 0..1 of the duration
    std::string name;
    int line = 0;
};

// `[borrow]` (phase 1): wear a vanilla mode, retimed. The clip's time maps to `offset_ms + elapsed * speed`.
struct Borrow {
    bool present = false;
    uint32_t mode = 0;           // the vanilla mode id
    float speed = 1;
    float offset_ms = 0;
};

struct Clip {
    bool duration_from_ability = false;   // `duration = "ability"`
    float duration_ms = 0;                // used when not from the ability
    End end = End::Hold;
    float blend_in_ms = 0;
    float blend_out_ms = 0;
    std::vector<Track> tracks;
    std::vector<Hide> hides;
    std::vector<Marker> markers;
    Borrow borrow;
};

// One part's pose: the vectors the game builds (position in voxels, rotation in degrees).
struct PartPose {
    float pos[3] = {0, 0, 0};
    float rot[3] = {0, 0, 0};
};
struct Pose {
    PartPose parts[kPartCount];
};

// The clip's duration in ms: its own, or the ability's for `"ability"`. 0 when it cannot be known (an open-ended ability).
float resolve_duration(const Clip& c, float ability_duration_ms);

struct Timing {
    float elapsed_ms = 0;        // the mode time: ms since the clip started
    float duration_ms = 0;       // from resolve_duration; <= 0 means there is nothing to draw
    float run_ms = 0;            // when the pose stops being drawn, for blend_out; <= 0: the clip's duration for `hold`, never for `loop`
    Signals signals;             // what the game says at this draw: for `expr` tracks only (keys, osc and spring ignore it)
};

struct Applied {
    bool ok = false;             // false: duration <= 0, or no tracks; the pose is untouched
    uint32_t pos_parts = 0;      // bit (1 << Part) of each part whose position a track wrote
    uint32_t rot_parts = 0;      // the same for rotation
    float weight = 1;            // the blend weight used, 0..1
};

// The blend weight at a time: the ramp in from 0, the ramp out to 0 (blend_in and blend_out), 0..1. The ramps multiply.
float blend_weight(const Clip& c, const Timing& t);

// A track alone: its value at `u` in 0..1 of the clip's duration (clamped); before the first key the first value, after the last the
// last. No keys: all zero. A generator track needs the clip's duration in ms (its time is u * duration) and the signals: this form
// gives it a duration of 0 and signals of 0, so it reads the generator at time 0. Use the form below for a track that may be one.
void sample_track(const Track& tr, float u, float out[3]);
// The same with the clip's duration in ms and the draw's signals: the form every generator needs. For a keys track the two forms
// agree. The value of a generator track is held to the channel's bound (kMaxPos or kMaxRotDeg).
void sample_track(const Track& tr, float u, float duration_ms, const Signals& signals, float out[3]);

// Applies every track to `pose` (the base pose, in place): `add` adds the sampled value, `set` replaces the vector, and the
// result is blended with the base by blend_weight. Tracks apply in order, so two tracks on one part and channel stack.
// Past the duration a `hold` clip stays at its last pose and a `loop` clip wraps.
Applied apply_clip(const Clip& c, const Timing& t, Pose& pose);

// The ExprUse bits of the names the clip's `expr` tracks mention (animation_expr.h); 0 when no track reads anything from the game.
uint32_t clip_expr_uses(const Clip& c);

// The hide groups that are on at this time (from <= u < to; an interval that ends at 1 stays on while the clip holds).
uint32_t hide_mask(const Clip& c, const Timing& t);

// Markers crossed in (from_ms, to_ms], as indexes into c.markers, in the order crossed. A `loop` clip crosses each marker once
// per lap. Empty when to_ms <= from_ms (a new activation is the caller's to detect) or the duration is <= 0.
std::vector<int> markers_crossed(const Clip& c, float from_ms, float to_ms, float duration_ms);

// Phase 1: the time to give the borrowed vanilla mode, in ms, never below 0. 0 when the clip borrows nothing.
float borrowed_mode_time(const Clip& c, float elapsed_ms);

}  // namespace kyubu::game::anim
