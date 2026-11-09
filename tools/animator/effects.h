// The Animator's effects: the `[[shockwave]]`s and `[[pillars]]` of a rig (rig.h), started by the clip's markers, so an impact a mod
// will draw (EMTweaks' shockwave, earth pillars) is shaped and timed against the clip first.
//
// A start is a marker of the effect's `at` name: the effect is placed by its frame at that marker's time and stays there, as a mod
// spawns it in the world in an ability's step. A shockwave is EMTweaks' own (its shockwave.hpp, copied in emtweaks/, builds its
// cells here as in the game): a flat ring that grows, fades and is drawn see-through. A set of pillars is `count` rock pillars at
// random spots round the centre, each a stack of chipped voxel stones with loose stones at its foot; each slides up out of the
// ground (fast, a little past its height, then settling), stays, and sinks back. Seen at a time, both are a function of that time only (the starts are planned once per clip edit; a pillar's random
// numbers come from the seed, the set's place in the rig, the start's number and its own), so scrubbing works. Pure: no windows.h,
// no Direct3D.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "cubmesh.h"
#include "rig.h"

namespace kanimator {

constexpr float kEffectTickMs = 20;        // a shockwave's tick: one step of the game's World
constexpr float kDefaultRingPixel = 0.25f; // blocks: EMTweaks' `pixel_size` option, for a ring with `pixel_size = 0`

// One start of an effect: a marker's time and the effect's frame there, in the figure's.
struct EffectStart {
    float t_ms = 0;
    uint32_t index = 0;                    // its number among the effect's starts, in time order
    Affine frame;
};

struct EffectPlan {
    float length_ms = 0;
    std::vector<std::vector<EffectStart>> rings;     // per Rig::shockwaves
    std::vector<std::vector<EffectStart>> pillars;   // per Rig::pillars
};

// The starts of the rig's effects over a clip of `length_ms`: each marker of an effect's `at`, the clip there over `base_at(time)`
// (empty: the neutral pose) placing it. No effect, no length or no such marker: no starts.
EffectPlan plan_effects(const Rig& rig, const Clip& clip, float length_ms, const std::function<Pose(float)>& base_at);

// A ring alive at a time, as EMTweaks has it at that age.
struct LiveRing {
    size_t effect = 0;                     // its index in Rig::shockwaves
    float centre[3] = {0, 0, 0};           // voxels, the figure's frame
    float facing[3] = {0, 0, 1};           // its normal, a unit vector
    float age_ticks = 0;
    float inner = 0, outer = 0;            // blocks
    float alpha = 1;
};

// A pillar alive at a time.
struct LivePillar {
    size_t effect = 0;                     // its index in Rig::pillars
    uint32_t number = 0;                   // its start's number * kMaxPillars + its own: its stones' random numbers come from it
    float foot[3] = {0, 0, 0};             // voxels, the figure's frame: where its axis meets the frame's ground plane
    float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // its own x, up (its axis) and z, unit vectors in the figure's frame
    float flat[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // the same before the lean: the rubble lies on the ground in these
    float width = 1;                       // blocks, the side of its foot
    float height = 1;                      // blocks above the ground when it is all out
    float lean = 0;                        // degrees from the frame's +y
    float out = 0;                         // how much of it is out: 0 to 1, a little past 1 just after it rises
    float rgb[3] = {1, 1, 1};              // its stones, before each is shaded off it
};

struct LiveEffects {
    std::vector<LiveRing> rings;
    std::vector<LivePillar> pillars;
};

// The rings and pillars alive at `time_ms`. `wrap`: the clip loops, so the starts of the lap before are alive at its start too.
LiveEffects live_effects(const Rig& rig, const EffectPlan& plan, float time_ms, bool wrap);

// How much of a pillar is out `age_ms` after it starts to rise: up over `rise_ms` (fast, about 5 % past, settling), all out for
// `hold_ms`, then in over `sink_ms` (slow, faster, slow). 0 before and after.
float pillar_out(float rise_ms, float hold_ms, float sink_ms, float age_ms);

// The pillars as voxel rocks (opaque), in the figure's frame: draw with no node matrix. A pillar is stacked stones on its own voxel
// grid (axes, cell): each narrower than the one below by the taper, shifted a voxel or so, its corners chipped, its top stepped in,
// a bulge on some sides, a dark seam under it, a light rim round its top, now and then a pale crack. Its rubble is a few small
// stones on the ground grid round its foot (flat). All of it comes from the pillar's number, so it is the same at every time;
// `out` slides it up through the ground, and what is under the ground is cut off.
Mesh pillar_mesh(const Rig& rig, const std::vector<LivePillar>& pillars);
// The rings as EMTweaks draws them, in the figure's frame: square cells (or a smooth ring), the alpha in each vertex colour. Draw
// them after everything opaque, blended, tested against the depth and not written to it.
Mesh ring_mesh(const Rig& rig, const std::vector<LiveRing>& rings);

}  // namespace kanimator
