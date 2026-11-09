// The Animator's particles: the `[[emitter]]`s of a rig (rig.h) throwing small cubes over a clip, moved the way the game moves its own
// particles, so an effect a mod will make of the game's particles can be shaped and timed in the Animator first.
//
// The game's rule (docs/dev-tools.md, "Particles"; read from the game for the mapping, mapping/atlas/combat.md): a particle record
// throws `count` cubes from one point, each with the record's speed plus a random -spreading to +spreading on each axis; a cube then
// flies straight, falls under gravity (or not, by its type), spins, keeps its size and colour, and is gone at the end of its life. The
// numbers of that rule (gravity, the block) are the rig's; an emitter adds what a mod can do on top: a cone of ways, a shorter life,
// a size and a colour that change with age, stopping at the ground.
//
// Seen at a time, the particles are a function of that time only: an emitter's bursts over the whole clip are planned once (the clip
// at each burst's time gives the emitter's place and the amount), and a burst's particles take their random numbers from the emitter's
// seed and their own number. So scrubbing back, a sheet and the window show the same particles. Pure: no windows.h, no Direct3D.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cubmesh.h"
#include "rig.h"

namespace kanimator {

constexpr size_t kMaxLiveParticles = 20000;   // the most drawn at once (the view's bound, not the game's)

// One emitter's bursts over a clip, one every step_ms from time 0 up to the clip's end.
struct EmitterPlan {
    float step_ms = 16;
    std::vector<uint32_t> first;   // per burst: the number of its first particle (numbered from 0 over the clip)
    std::vector<uint32_t> count;   // per burst: how many particles it throws
    std::vector<Affine> frame;     // per burst: the emitter's frame in the figure's
};

struct ParticlePlan {
    float length_ms = 0;
    std::vector<EmitterPlan> emitters;   // one per Rig::emitters
};

// The times (ms, in order) of the clip's markers named `name`, for a clip of `length_ms`.
std::vector<float> marker_times(const Clip& clip, const std::string& name, float length_ms);

// The bursts of the rig's emitters over a clip of `length_ms`: the clip at each burst's time, over `base_at(time)` (empty: the
// neutral pose), places the emitter and gives its amount. The count of a burst carries the fractions over, so `rate` particles a
// second come out on average. No emitter, or no length: no bursts.
ParticlePlan plan_particles(const Rig& rig, const Clip& clip, float length_ms, const std::function<Pose(float)>& base_at);

struct LiveParticle {
    float pos[3] = {0, 0, 0};   // the cube's centre, voxels, in the figure's frame
    float size = 1;             // its side, voxels
    float rot[3] = {0, 0, 0};   // degrees about x, y and z; the cube is turned about z, then y, then x, as the game turns its own
    float rgb[3] = {1, 1, 1};   // 0 to 1
};

// The particles alive at `time_ms`: those of the bursts at or before it whose life has not run out. `wrap`: the clip loops, so the
// bursts at the end of the lap before are alive at its start too. At most `max` (the emitters in order, each burst's oldest first).
std::vector<LiveParticle> live_particles(const Rig& rig, const ParticlePlan& plan, float time_ms, bool wrap,
                                         size_t max = kMaxLiveParticles);

// The particles as shaded cubes, in the figure's frame: draw the mesh with no node matrix.
Mesh particle_mesh(const std::vector<LiveParticle>& particles);

}  // namespace kanimator
