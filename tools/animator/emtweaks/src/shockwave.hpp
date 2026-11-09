// The shockwave effect without the game: validating a definition, ageing and fading it, and building the ring's triangles.
// Nothing here touches Direct3D or the game, so tests/ builds it on its own (mods-gameplay/tests/emtweaks).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../api/emtweaks.h"

namespace shockwave {

constexpr double kTickMs = 20.0;            // a tick is one World step of the game
constexpr double kUnitsPerBlock = 65536.0;  // the game's world units
constexpr uint32_t kMaxEffects = 256;
constexpr uint32_t kMaxLifeTicks = 6000;    // 2 minutes
constexpr unsigned kSegments = 64;          // the ring is a polygon of this many sides
constexpr unsigned kRingVertices = kSegments * 6;   // two triangles a side
constexpr float kMinThickness = 0.01f;
constexpr float kMinPixel = 0.02f;           // a cell is never smaller
constexpr int kMaxGrid = 96;                 // cells across the outer diameter: a larger ring gets larger cells

// A definition checked and converted: the centre in blocks, the facing a unit vector, the colour clamped.
struct Params {
    int64_t units[3] = {0, 0, 0};   // the centre in world units, as given
    double pos[3] = {0, 0, 0};      // the same in blocks
    float facing[3] = {0, 0, 1};
    float inner_radius = 0, thickness = kMinThickness, expansion = 0;
    uint32_t life_ticks = 1, fade_on_tick = 0;
    float fade_speed = 0;
    float color[4] = {1, 1, 1, 1};
    float pixel_size = 0;           // blocks: the size of a square cell; 0 draws a smooth ring
};

struct Effect {
    uint32_t id = 0;
    Params p;
    double age = 0;   // ticks
};

// Position in blocks relative to the centre; `argb` is a D3DCOLOR (0xAARRGGBB).
struct Vertex {
    float x, y, z;
    uint32_t argb;
};

inline bool finite(float v) { return std::isfinite(v); }

// Fills `out` from a definition. False, with the reason, when it is refused.
inline bool make_params(const EmShockwaveDef* d, Params* out, std::string* why, float default_pixel = 0.0f) {
    // The fields up to and including `color` are the whole of version 1.
    constexpr uint32_t kMinSize = offsetof(EmShockwaveDef, color) + sizeof(float) * 4;
    if (!d) {
        *why = "the definition is NULL";
        return false;
    }
    if (d->size < kMinSize) {
        *why = "def->size is " + std::to_string(d->size) + ", below " + std::to_string(kMinSize);
        return false;
    }
    float f[11] = {d->facing[0], d->facing[1], d->facing[2], d->inner_radius, d->thickness, d->expansion,
                                      d->fade_speed, d->color[0], d->color[1], d->color[2], d->color[3]};
    for (float v : f)
        if (!finite(v)) {
            *why = "a field is not a number";
            return false;
        }
    Params p;
    for (int i = 0; i < 3; ++i) {
        p.units[i] = d->pos[i];
        p.pos[i] = double(d->pos[i]) / kUnitsPerBlock;
    }
    double len = std::sqrt(double(d->facing[0]) * d->facing[0] + double(d->facing[1]) * d->facing[1] +
                           double(d->facing[2]) * d->facing[2]);
    if (len < 1e-6) {
        p.facing[0] = 0;
        p.facing[1] = 0;
        p.facing[2] = 1;
    } else {
        for (int i = 0; i < 3; ++i) p.facing[i] = float(d->facing[i] / len);
    }
    p.inner_radius = std::max(d->inner_radius, 0.0f);
    p.thickness = std::max(d->thickness, kMinThickness);
    p.expansion = d->expansion;
    p.life_ticks = std::min(std::max(d->life_ticks, 1u), kMaxLifeTicks);
    p.fade_on_tick = d->fade_on_tick;
    p.fade_speed = std::max(d->fade_speed, 0.0f);
    for (int i = 0; i < 4; ++i) p.color[i] = std::min(std::max(d->color[i], 0.0f), 1.0f);
    // pixel_size: not in the caller's table, or 0, means the default; below 0 is smooth
    float px = default_pixel;
    if (d->size >= offsetof(EmShockwaveDef, pixel_size) + sizeof(float) && finite(d->pixel_size) && d->pixel_size != 0.0f) px = d->pixel_size;
    p.pixel_size = px > 0.0f ? std::max(px, kMinPixel) : 0.0f;
    *out = p;
    return true;
}

// The inner and outer radius after `age` ticks (never below 0).
inline float inner_at(const Params& p, double age) { return std::max(0.0f, p.inner_radius + p.expansion * float(age)); }
inline float outer_at(const Params& p, double age) {
    return std::max(0.0f, p.inner_radius + p.thickness + p.expansion * float(age));
}

// The opacity after `age` ticks: the colour's alpha times the fade. 0 once it has faded out.
inline float alpha_at(const Params& p, double age) {
    float k = 1.0f;
    if (p.fade_speed > 0 && age >= double(p.fade_on_tick)) k = std::max(0.0f, 1.0f - p.fade_speed * float(age - double(p.fade_on_tick)));
    return p.color[3] * k;
}

inline bool alive_at(const Params& p, double age) {
    if (age >= double(p.life_ticks)) return false;
    if (alpha_at(p, age) <= 0.0f) return false;
    return outer_at(p, age) > 0.0f;
}

// Builds the ring's kRingVertices vertices (a triangle list) for the effect at its age: a flat ring in the plane whose normal is
// the facing, around the centre (the origin of the vertices). Returns false when there is nothing to draw (invisible).
inline bool build_ring(const Params& p, double age, Vertex* out) {
    const float a = alpha_at(p, age);
    const float ro = outer_at(p, age), ri = std::min(inner_at(p, age), ro);
    if (a <= 0.0f || ro <= 0.0f) return false;
    // A basis (u, v) of the ring's plane.
    const double nx = p.facing[0], ny = p.facing[1], nz = p.facing[2];
    double ax = 0, ay = 0, az = 1;
    if (std::fabs(nz) > 0.9) {
        ax = 1;
        az = 0;
    }
    double ux = ay * nz - az * ny, uy = az * nx - ax * nz, uz = ax * ny - ay * nx;   // a x n
    const double ul = std::sqrt(ux * ux + uy * uy + uz * uz);
    ux /= ul;
    uy /= ul;
    uz /= ul;
    const double vx = ny * uz - nz * uy, vy = nz * ux - nx * uz, vz = nx * uy - ny * ux;   // n x u
    const auto byte = [](float c) { return uint32_t(std::min(std::max(c, 0.0f), 1.0f) * 255.0f + 0.5f); };
    const uint32_t argb = (byte(a) << 24) | (byte(p.color[0]) << 16) | (byte(p.color[1]) << 8) | byte(p.color[2]);
    const double step = 6.283185307179586 / kSegments;
    Vertex* o = out;
    for (unsigned k = 0; k < kSegments; ++k) {
        const double t0 = step * k, t1 = step * (k + 1);
        const double c0 = std::cos(t0), s0 = std::sin(t0), c1 = std::cos(t1), s1 = std::sin(t1);
        auto at = [&](double r, double c, double s) {
            return Vertex{float(r * (c * ux + s * vx)), float(r * (c * uy + s * vy)), float(r * (c * uz + s * vz)), argb};
        };
        const Vertex i0 = at(ri, c0, s0), o0 = at(ro, c0, s0), i1 = at(ri, c1, s1), o1 = at(ro, c1, s1);
        *o++ = i0; *o++ = o0; *o++ = o1;
        *o++ = i0; *o++ = o1; *o++ = i1;
    }
    return true;
}

// The ring as square cells (a triangle list, two triangles a cell) on a grid in the ring's plane, centred on the ring's centre.
// A cell is in when its middle lies in the ring (a ring thinner than a cell is one cell wide). False when nothing is drawn.
inline bool build_pixel_ring(const Params& p, double age, std::vector<Vertex>& out) {
    out.clear();
    const float a = alpha_at(p, age);
    const float ro = outer_at(p, age), ri = std::min(inner_at(p, age), ro);
    if (a <= 0.0f || ro <= 0.0f || p.pixel_size <= 0.0f) return false;
    const double nx = p.facing[0], ny = p.facing[1], nz = p.facing[2];
    double ax = 0, ay = 0, az = 1;
    if (std::fabs(nz) > 0.9) {
        ax = 1;
        az = 0;
    }
    double ux = ay * nz - az * ny, uy = az * nx - ax * nz, uz = ax * ny - ay * nx;
    const double ul = std::sqrt(ux * ux + uy * uy + uz * uz);
    ux /= ul;
    uy /= ul;
    uz /= ul;
    const double vx = ny * uz - nz * uy, vy = nz * ux - nx * uz, vz = nx * uy - ny * ux;
    const auto byte = [](float c) { return uint32_t(std::min(std::max(c, 0.0f), 1.0f) * 255.0f + 0.5f); };
    const uint32_t argb = (byte(a) << 24) | (byte(p.color[0]) << 16) | (byte(p.color[1]) << 8) | byte(p.color[2]);
    const double s = std::max(double(p.pixel_size), 2.0 * ro / kMaxGrid);
    const double mid = 0.5 * (ri + ro), half = std::max(0.5 * (ro - ri), 0.5 * s);
    const int n = int(std::ceil(ro / s)) + 1;
    const auto pt = [&](double cu, double cv) {
        return Vertex{float(cu * ux + cv * vx), float(cu * uy + cv * vy), float(cu * uz + cv * vz), argb};
    };
    for (int j = -n; j < n; ++j)
        for (int i = -n; i < n; ++i) {
            const double cu = (i + 0.5) * s, cv = (j + 0.5) * s;
            if (std::fabs(std::sqrt(cu * cu + cv * cv) - mid) > half) continue;
            const Vertex q00 = pt(i * s, j * s), q10 = pt((i + 1) * s, j * s), q01 = pt(i * s, (j + 1) * s), q11 = pt((i + 1) * s, (j + 1) * s);
            out.insert(out.end(), {q00, q10, q11, q00, q11, q01});
        }
    return !out.empty();
}

// The shockwaves alive.
class Manager {
public:
    // The new id (1 or more), or 0 when kMaxEffects are alive.
    uint32_t spawn(const Params& p) {
        if (effects_.size() >= kMaxEffects) return 0;
        Effect e;
        e.id = next_id_++;
        if (next_id_ == 0) next_id_ = 1;
        e.p = p;
        effects_.push_back(e);
        return e.id;
    }
    bool cancel(uint32_t id) {
        for (size_t i = 0; i < effects_.size(); ++i)
            if (effects_[i].id == id) {
                effects_.erase(effects_.begin() + i);
                return true;
            }
        return false;
    }
    // Ages every effect by `ms` of game time and drops the ones that ended.
    void advance(double ms) {
        const double ticks = ms / kTickMs;
        for (auto& e : effects_) e.age += ticks;
        effects_.erase(std::remove_if(effects_.begin(), effects_.end(), [](const Effect& e) { return !alive_at(e.p, e.age); }),
                       effects_.end());
    }
    void clear() { effects_.clear(); }
    size_t size() const { return effects_.size(); }
    bool empty() const { return effects_.empty(); }
    const std::vector<Effect>& effects() const { return effects_; }

private:
    std::vector<Effect> effects_;
    uint32_t next_id_ = 1;
};

}  // namespace shockwave
