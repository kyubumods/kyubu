#include "particles.h"

#include <algorithm>
#include <cmath>

namespace kanimator {

namespace {

constexpr float kPi = 3.14159265358979f;

// A particle's random numbers: splitmix64 from the emitter's seed, the emitter's place in the rig and the particle's number.
struct Rng {
    uint64_t s;
    Rng(uint32_t seed, size_t emitter, uint32_t particle)
        : s((static_cast<uint64_t>(seed) << 32 ^ static_cast<uint64_t>(emitter) << 24 ^ particle) * 0x9E3779B97F4A7C15ull + 1) {}
    float next() {   // 0 to 1, 1 left out
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<float>(z >> 40) / static_cast<float>(1ull << 24);
    }
};

float lerp(float a, float b, float u) { return a + (b - a) * u; }

// A local vector turned by the frame (its scale taken out), as a unit vector; `fallback` when the frame flattens it.
void turned(const Affine& f, const float local[3], const float fallback[3], float out[3]) {
    float len = 0;
    for (int i = 0; i < 3; ++i) {
        out[i] = f.r[i][0] * local[0] + f.r[i][1] * local[1] + f.r[i][2] * local[2];
        len += out[i] * out[i];
    }
    len = std::sqrt(len);
    for (int i = 0; i < 3; ++i) out[i] = len > 0 ? out[i] / len : fallback[i];
}

// The emitter's frame at a burst for the particle's way: +y turned by `cone_min` to `cone` degrees (evenly over that band of the
// sphere), then by the frame's turn, as a unit vector. `across` is the way's lean across the frame's x-z plane, turned the same.
void way(const Affine& f, float cone_min, float cone, float u, float v, float out[3], float across[3]) {
    const float c0 = std::cos(cone_min * kPi / 180), c1 = std::cos(cone * kPi / 180);
    const float c = c0 - u * (c0 - c1), s = std::sqrt(std::max(0.0f, 1 - c * c)), phi = 2 * kPi * v;
    const float local[3] = {s * std::cos(phi), c, s * std::sin(phi)}, side[3] = {std::cos(phi), 0, std::sin(phi)};
    const float up[3] = {0, 1, 0}, x[3] = {1, 0, 0};
    turned(f, local, up, out);
    turned(f, side, x, across);
}

// When a particle at height z0, rising at vz and falling by g (voxels a second squared, down), first comes down to z 0; -1 for
// never. One at or under the ground and not rising is there at once; one rising from under it comes down on its way back.
float landing(float z0, float vz, float g) {
    if (z0 <= 0) {
        if (vz <= 0) return 0;
        if (g <= 0) return -1;
        const float d = vz * vz + 2 * g * z0;
        return d < 0 ? 0 : (vz + std::sqrt(d)) / g;   // the later root; none: it never comes up, so the ground holds it at once
    }
    if (g == 0) return vz < 0 ? -z0 / vz : -1.0f;
    const float d = vz * vz + 2 * g * z0;   // 0 = z0 + vz t - g t^2 / 2
    if (d < 0) return -1;
    const float r = std::sqrt(d), t1 = (vz - r) / g, t2 = (vz + r) / g;
    if (t1 > 0 && t2 > 0) return std::min(t1, t2);
    return std::max(t1, t2) > 0 ? std::max(t1, t2) : -1.0f;
}

// The particles of one burst `age_ms` old.
void add_burst(const Rig& rig, size_t ei, const EmitterPlan& ep, size_t k, float age_ms, size_t max, std::vector<LiveParticle>& out) {
    const Emitter& e = rig.emitters[ei];
    const float block = rig.particles.block;
    const float g = e.gravity ? rig.particles.gravity * block : 0.0f;
    const Affine& f = ep.frame[k];
    for (uint32_t j = 0; j < ep.count[k] && out.size() < max; ++j) {
        Rng r(e.seed, ei, ep.first[k] + j);
        const float life = lerp(e.life.lo, e.life.hi, r.next());
        const float speed = lerp(e.speed.lo, e.speed.hi, r.next());
        const float cu = r.next(), cv = r.next();
        float spread[3];
        for (float& s : spread) s = (r.next() * 2 - 1) * e.spreading;
        const float mix = r.next();
        float spin[3];
        for (float& s : spin) s = (r.next() * 2 - 1) * e.spin;
        const float away = lerp(e.offset.lo, e.offset.hi, r.next());  // drawn last, so a rig without `offset` keeps its particles
        if (age_ms >= life) continue;
        float dir[3], across[3], v[3], p0[3];
        way(f, e.cone_min, e.cone, cu, cv, dir, across);
        for (int i = 0; i < 3; ++i) {
            v[i] = (dir[i] * speed + spread[i]) * block;
            p0[i] = f.t[i] + across[i] * away * block;
        }
        float t = age_ms / 1000;   // seconds of flight: up to the landing when it stops at the ground
        if (e.ground_stop) {
            const float land = landing(p0[2], v[2], g);
            if (land >= 0 && land < t) t = land;
        }
        LiveParticle p;
        for (int i = 0; i < 3; ++i) {
            p.pos[i] = p0[i] + v[i] * t;
            p.rot[i] = spin[i] * t;
        }
        p.pos[2] -= 0.5f * g * t * t;
        if (e.ground_stop && p.pos[2] < 0) p.pos[2] = 0;
        const float age = age_ms / life;
        p.size = std::max(0.0f, lerp(e.size, e.size_end, age)) * block;
        for (int i = 0; i < 3; ++i) {
            float c = e.has_color_random ? lerp(e.color[i], e.color_random[i], mix) : e.color[i];
            if (e.has_color_end) c = lerp(c, e.color_end[i], age);
            p.rgb[i] = std::clamp(c, 0.0f, 1.0f);
        }
        if (p.size > 0) out.push_back(p);
    }
}

}  // namespace

std::vector<float> marker_times(const Clip& clip, const std::string& name, float length_ms) {
    std::vector<float> out;
    for (const kyubu::game::anim::Marker& m : clip.markers)
        if (m.name == name) out.push_back(m.t * length_ms);
    std::sort(out.begin(), out.end());
    return out;
}

ParticlePlan plan_particles(const Rig& rig, const Clip& clip, float length_ms, const std::function<Pose(float)>& base_at) {
    ParticlePlan plan;
    plan.length_ms = length_ms;
    if (rig.emitters.empty() || !(length_ms > 0)) return plan;
    plan.emitters.resize(rig.emitters.size());
    for (size_t ei = 0; ei < rig.emitters.size(); ++ei) {
        const Emitter& e = rig.emitters[ei];
        EmitterPlan& ep = plan.emitters[ei];
        ep.step_ms = std::max(e.step_ms, 1.0f);
        const size_t bursts = static_cast<size_t>(std::floor(length_ms / ep.step_ms)) + 1;
        const std::vector<float> marks = e.at.empty() ? std::vector<float>{} : marker_times(clip, e.at, length_ms);
        const bool loop = clip.end == kyubu::game::anim::End::Loop;
        // with `at`: on from each marker for for_ms (a loop's window runs on past its end into the lap's start)
        auto gated = [&](float ms) {
            if (e.at.empty()) return true;
            for (float m : marks) {
                const float d = ms - m;
                if ((d >= 0 && d < e.for_ms) || (loop && d < 0 && d + length_ms < e.for_ms)) return true;
            }
            return false;
        };
        double thrown = 0;   // particles due so far, fractions kept
        uint32_t made = 0;
        for (size_t k = 0; k < bursts; ++k) {
            Timing at;
            at.elapsed_ms = static_cast<float>(k) * ep.step_ms;
            at.duration_ms = length_ms;
            at.run_ms = length_ms;
            const Solved s = solve(rig, base_at ? base_at(at.elapsed_ms) : Pose{}, clip, at);
            const float amount = gated(at.elapsed_ms) ? std::max(0.0f, arg_value(e.amount, s.pose)) : 0.0f;
            thrown += static_cast<double>(e.rate) * amount * ep.step_ms / 1000.0;
            const uint32_t upto = static_cast<uint32_t>(std::floor(thrown + 1e-6));
            ep.first.push_back(made);
            ep.count.push_back(upto - made);
            made = upto;
            const Affine parent = e.parent >= 0 && static_cast<size_t>(e.parent) < s.world.size() ? s.world[e.parent] : Affine{};
            ep.frame.push_back(parent * steps_matrix(e.steps, s.pose));
        }
    }
    return plan;
}

std::vector<LiveParticle> live_particles(const Rig& rig, const ParticlePlan& plan, float time_ms, bool wrap, size_t max) {
    std::vector<LiveParticle> out;
    for (size_t ei = 0; ei < plan.emitters.size() && ei < rig.emitters.size(); ++ei) {
        const EmitterPlan& ep = plan.emitters[ei];
        const float oldest = rig.emitters[ei].life.hi;
        const size_t n = ep.frame.size();
        if (n == 0) continue;
        // the lap before: bursts after length - (oldest - time), before the clip's end (its end is this lap's start)
        if (wrap && time_ms < oldest && plan.length_ms > 0) {
            for (size_t k = 0; k < n; ++k) {
                const float tk = static_cast<float>(k) * ep.step_ms;
                if (tk >= plan.length_ms - 1e-3f) continue;
                const float age = time_ms + plan.length_ms - tk;
                if (age < oldest) add_burst(rig, ei, ep, k, age, max, out);
            }
        }
        const float lo = std::max(0.0f, std::ceil((time_ms - oldest) / ep.step_ms));
        for (size_t k = static_cast<size_t>(lo); k < n; ++k) {
            const float tk = static_cast<float>(k) * ep.step_ms;
            if (tk > time_ms) break;
            add_burst(rig, ei, ep, k, time_ms - tk, max, out);
        }
    }
    return out;
}

Mesh particle_mesh(const std::vector<LiveParticle>& particles) {
    Mesh m;
    m.tris.reserve(particles.size() * 36);
    for (const LiveParticle& p : particles) {
        const uint32_t rgb = static_cast<uint32_t>(std::lround(p.rgb[0] * 255)) << 16 |
                             static_cast<uint32_t>(std::lround(p.rgb[1] * 255)) << 8 | static_cast<uint32_t>(std::lround(p.rgb[2] * 255));
        const Mesh cube = mesh_box(p.size, p.size, p.size, rgb);
        // Rz * Ry * Rx, as the rig's rot steps
        const float ax = p.rot[0] * kPi / 180, ay = p.rot[1] * kPi / 180, az = p.rot[2] * kPi / 180;
        const float cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay), cz = std::cos(az), sz = std::sin(az);
        const float r[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                               {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                               {-sy, cy * sx, cy * cx}};
        for (MeshVertex v : cube.tris) {
            const float x = v.x, y = v.y, z = v.z;
            v.x = r[0][0] * x + r[0][1] * y + r[0][2] * z + p.pos[0];
            v.y = r[1][0] * x + r[1][1] * y + r[1][2] * z + p.pos[1];
            v.z = r[2][0] * x + r[2][1] * y + r[2][2] * z + p.pos[2];
            m.tris.push_back(v);
        }
    }
    return m;
}

}  // namespace kanimator
