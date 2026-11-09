// The particles primitive: one of the game's own particle effects (openspec add-particle-effects, D1; docs/game-api.md, particle_spawn).
//
// The game spawns a particle effect by building a cube::ParticleData (0x48 bytes: cuwo's particle packet) and pushing it on the particle
// list of the World step's output (`step_out`, list at +0x10) with a std::list push. The controller then plays the output once per frame. A
// mod's effect is queued here and pushed by the cube::World::update detour once the step has run, so that it lands on the output the
// controller plays next and the push runs where the game's own pushes run: the game's thread, under the World's lock.
#include "particles.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

#include "call.h"
#include "cubeworld/functions.h"
#include "cubeworld/types.h"
#include "game.h"
#include "log.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

constexpr uint32_t kMaxQueued = 512;   // effects waiting for the next step
constexpr uint32_t kMinSize = offsetof(KyubuParticle, spreading) + sizeof(float);
constexpr float kMaxScale = 16.0f;
constexpr float kMaxSpreading = 64.0f;

std::mutex g_mu;
std::vector<cube_ParticleData> g_queue;

bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

KyubuResult KYUBU_CALL k_particle_spawn(KyubuModHandle self, const KyubuParticle* p) {
    GameCall g(self, Prim::particles, "particle_spawn");   // client-only (kSideRules)
    if (!g) return g.result();
    if (!p || p->size < kMinSize)
        return g.refuse(KY_E_INVALID_ARG, p ? fmt("p->size is %u, below %u", p->size, kMinSize) : "p is NULL");
    if (p->type > KY_PARTICLE_NO_GRAVITY) return g.refuse(KY_E_INVALID_ARG, fmt("type is %u, above %u", p->type, KY_PARTICLE_NO_GRAVITY));
    if (p->count < 1 || p->count > KY_PARTICLE_COUNT_MAX)
        return g.refuse(KY_E_INVALID_ARG, fmt("count is %u, outside 1 to %u", p->count, KY_PARTICLE_COUNT_MAX));
    if (!finite3(p->accel) || !std::isfinite(p->scale) || !std::isfinite(p->spreading))
        return g.refuse(KY_E_INVALID_ARG, "accel, scale or spreading is not a finite number");
    if (!(p->scale > 0.0f && p->scale <= kMaxScale)) return g.refuse(KY_E_INVALID_ARG, fmt("scale is %g, outside above 0 to %g", p->scale, kMaxScale));
    if (!(p->spreading >= 0.0f && p->spreading <= kMaxSpreading))
        return g.refuse(KY_E_INVALID_ARG, fmt("spreading is %g, outside 0 to %g", p->spreading, kMaxSpreading));
    for (int i = 0; i < 4; ++i)
        if (!(p->color[i] >= 0.0f && p->color[i] <= 1.0f)) return g.refuse(KY_E_INVALID_ARG, fmt("color[%d] is outside 0 to 1", i));

    cube_ParticleData d;
    memset(&d, 0, sizeof d);
    d.pos.x = p->pos[0];
    d.pos.y = p->pos[1];
    d.pos.z = p->pos[2];
    for (int i = 0; i < 3; ++i) d.accel[i] = p->accel[i];
    for (int i = 0; i < 4; ++i) d.color[i] = p->color[i];
    d.scale = p->scale;
    d.count = p->count;
    d.particle_type = p->type;
    d.spreading = p->spreading;

    std::lock_guard<std::mutex> lock(g_mu);
    if (g_queue.size() >= kMaxQueued)
        return g.refuse(KY_E_REFUSED, fmt("%u effects are waiting for the next World step already", kMaxQueued));
    g_queue.push_back(d);
    return KY_OK;
}

}  // namespace

void fill_particles(KyubuApi& a) { a.particle_spawn = k_particle_spawn; }

void flush_particles(void* step_out) {
    if (!step_out) return;
    std::vector<cube_ParticleData> now;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_queue.empty()) return;
        now.swap(g_queue);
    }
    // The list is cube::StepOutput.particles; the push is the game's own, a thiscall on the list. Checked here, not at spawn: the loader
    // may have been asked before the first step.
    const cw_step_output_push_particle_fn push = (cw_step_output_push_particle_fn)verified_symbol(CW_SYM_step_output_push_particle);
    if (!push) return;
    void* list = (char*)step_out + offsetof(cube_StepOutput, particles);
    for (auto& d : now) push(list, nullptr, &d);
}

unsigned queued_particles_for_test() {
    std::lock_guard<std::mutex> lock(g_mu);
    return (unsigned)g_queue.size();
}

void clear_particles_for_test() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_queue.clear();
}

}  // namespace kyubu::game
