#include "effects.h"

#include <algorithm>
#include <cmath>

#include "emtweaks/src/shockwave.hpp"   // EMTweaks' ring: its age, fade and cells, as the game draws them
#include "particles.h"                                      // marker_times

namespace kanimator {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kSpotTries = 16;   // a pillar's foot drawn this many times in `clear` is left out

// A pillar's random numbers: splitmix64 from the set's seed, its place in the rig, its number (start * kMaxPillars + pillar) and a
// stream (its spot, which may be drawn again, and the rest, which is not).
struct Rng {
    uint64_t s;
    Rng(uint32_t seed, size_t effect, uint32_t number, uint32_t stream)
        : s((static_cast<uint64_t>(seed) << 32 ^ static_cast<uint64_t>(effect) << 24 ^ static_cast<uint64_t>(stream) << 20 ^ number) *
                0x9E3779B97F4A7C15ull + 1) {}
    float next() {   // 0 to 1, 1 left out
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<float>(z >> 40) / static_cast<float>(1ull << 24);
    }
};

float lerp(float a, float b, float u) { return a + (b - a) * u; }
float lerp(const Range& r, float u) { return lerp(r.lo, r.hi, u); }

// The frame's turn of `local` (its scale taken out), as a unit vector; `local` itself when the frame flattens it.
void turn_unit(const Affine& f, const float local[3], float out[3]) {
    float len = 0;
    for (int i = 0; i < 3; ++i) {
        out[i] = f.r[i][0] * local[0] + f.r[i][1] * local[1] + f.r[i][2] * local[2];
        len += out[i] * out[i];
    }
    len = std::sqrt(len);
    for (int i = 0; i < 3; ++i) out[i] = len > 0 ? out[i] / len : local[i];
}

// v turned about the unit axis k by `rad` (Rodrigues).
void about(const float k[3], float rad, const float v[3], float out[3]) {
    const float c = std::cos(rad), s = std::sin(rad), d = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    const float kx[3] = {k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]};
    for (int i = 0; i < 3; ++i) out[i] = v[i] * c + kx[i] * s + k[i] * d * (1 - c);
}

// How long ago a start was at `time_ms`; below 0: not yet. A wrapping clip's start later in the lap is the lap before's.
float since(const EffectStart& st, float time_ms, float length_ms, bool wrap) {
    if (time_ms >= st.t_ms) return time_ms - st.t_ms;
    if (wrap && length_ms > 0) return time_ms + length_ms - st.t_ms;
    return -1;
}

// EMTweaks' ring for a shockwave of the rig, facing `facing` (a unit vector).
shockwave::Params ring_params(const Shockwave& w, const float facing[3]) {
    shockwave::Params p;
    for (int i = 0; i < 3; ++i) p.facing[i] = facing[i];
    p.inner_radius = std::max(w.inner_radius, 0.0f);
    p.thickness = std::max(w.thickness, shockwave::kMinThickness);
    p.expansion = w.expansion;
    p.life_ticks = std::min(std::max(w.life_ticks, 1u), shockwave::kMaxLifeTicks);
    p.fade_on_tick = w.fade_on_tick;
    p.fade_speed = std::max(w.fade_speed, 0.0f);
    for (int i = 0; i < 4; ++i) p.color[i] = std::clamp(w.color[i], 0.0f, 1.0f);
    const float px = w.pixel_size == 0 ? kDefaultRingPixel : w.pixel_size;
    p.pixel_size = px > 0 ? std::max(px, shockwave::kMinPixel) : 0.0f;
    return p;
}

// 0xFFRRGGBB of a colour times a face's shade: the share of each axis in its normal times that axis's shade (cubmesh.h), so a
// turned face is lit between the faces it lies between.
uint32_t shaded(const float rgb[3], const float n[3]) {
    float shade = 0;
    for (int a = 0; a < 3; ++a) shade += n[a] * n[a] * face_shade(a, n[a] > 0);
    uint32_t out = 0xFF000000u;
    for (int i = 0; i < 3; ++i)
        out |= static_cast<uint32_t>(std::clamp(rgb[i] * shade * 255.0f + 0.5f, 0.0f, 255.0f)) << (16 - 8 * i);
    return out;
}

// ---- rock pillars: their voxels ----

constexpr int kMaxAcross = 40;   // voxels across a pillar's foot at most: a wider one gets bigger voxels
constexpr int kMaxUp = 80;       // and up it
constexpr int kMargin = 4;       // voxels round the widest stone in its grid: room for the shifts, leans and bulges
constexpr int kRubbleReach = 6;  // voxels past that for the rubble's grid
constexpr int kRubbleUp = 4;     // voxels: the rubble's grid is this tall
constexpr int kMaxShear = 2;     // voxels a stone's top is shifted off its foot, at most (its own lean)

// A grid of voxels: 0 is empty, else 0xFFRRGGBB (before the faces' shade).
struct Voxels {
    int n[3] = {0, 0, 0};
    std::vector<uint32_t> c;
    void reset(int x, int y, int z) {
        n[0] = x;
        n[1] = y;
        n[2] = z;
        c.assign(static_cast<size_t>(x) * static_cast<size_t>(y) * static_cast<size_t>(z), 0);
    }
    bool in(int x, int y, int z) const { return x >= 0 && y >= 0 && z >= 0 && x < n[0] && y < n[1] && z < n[2]; }
    size_t index(int x, int y, int z) const {
        return (static_cast<size_t>(y) * static_cast<size_t>(n[2]) + static_cast<size_t>(z)) * static_cast<size_t>(n[0]) + static_cast<size_t>(x);
    }
    uint32_t get(int x, int y, int z) const { return in(x, y, z) ? c[index(x, y, z)] : 0; }
    void set(int x, int y, int z, uint32_t v) {
        if (in(x, y, z)) c[index(x, y, z)] = v;
    }
};

int pick(Rng& r, int lo, int hi) {   // lo to hi, both in
    if (hi <= lo) return lo;
    return std::min(hi, lo + static_cast<int>(r.next() * static_cast<float>(hi - lo + 1)));
}

int rounded(float v) { return static_cast<int>(std::lround(v)); }

float c_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

uint32_t pack(const float rgb[3]) {
    uint32_t out = 0xFF000000u;
    for (int i = 0; i < 3; ++i) out |= static_cast<uint32_t>(std::clamp(rgb[i], 0.0f, 1.0f) * 255.0f + 0.5f) << (16 - 8 * i);
    return out;
}

// A stone's colours: its own (the pillar's tinted), its seam (darker), its rim (lighter, or color_top) and its cracks (paler).
struct StoneColors {
    uint32_t body, seam, rim, crack;
};

StoneColors stone_colors(const Pillars& p, const float rgb[3], Rng& r) {
    const float tint = 0.82f + 0.28f * r.next();
    float body[3], seam[3], rim[3], crack[3];
    static const float kPale[3] = {0.9f, 0.84f, 0.8f};
    for (int i = 0; i < 3; ++i) {
        body[i] = rgb[i] * tint * (0.96f + 0.08f * r.next());
        seam[i] = body[i] * 0.65f;
        rim[i] = p.has_color_top ? p.color_top[i] : std::min(1.0f, body[i] * 1.15f + 0.05f);
        crack[i] = lerp(body[i], kPale[i], 0.55f);
    }
    return {pack(body), pack(seam), pack(rim), pack(crack)};
}

// Colours the rim of a stone's top in rows y0 to y1 - 1: a voxel with none above it in those rows and an empty side.
void paint_rim(Voxels& v, int y0, int y1, uint32_t body, uint32_t rim) {
    for (int y = y0; y < y1; ++y)
        for (int z = 0; z < v.n[2]; ++z)
            for (int x = 0; x < v.n[0]; ++x) {
                if (v.get(x, y, z) != body) continue;
                if (y + 1 < y1 && v.get(x, y + 1, z) != 0) continue;
                if (!v.get(x - 1, y, z) || !v.get(x + 1, y, z) || !v.get(x, y, z - 1) || !v.get(x, y, z + 1)) v.set(x, y, z, rim);
            }
}

// One stone, wx by h by wz voxels, in its own grid `v` with a voxel of room on each side: its box cut by a few random planes
// across its corners and edges (its facets: the top's edges more often, the cap's most), shaded darker towards its foot and, when
// it stands on another (`seam`), darkest in its lowest row, with up to two pale cracks down its sides. Its rim is painted once it
// is in the stack.
void make_stone(Voxels& v, int wx, int h, int wz, bool cap, bool seam, const StoneColors& col, Rng& r) {
    v.reset(wx + 2, h, wz + 2);
    const int w[3] = {wx, h, wz};
    // the facets: in the box's own measure (-1 to 1 across it on each axis), each cuts off what lies past `d` along `n`; `d` lies
    // between the face centres (kept) and the corner (cut)
    struct Cut {
        float n[3];
        float d;
    };
    Cut cuts[8];
    const int count = pick(r, 4, cap ? 7 : 6);
    for (int k = 0; k < count; ++k) {
        Cut& c = cuts[k];
        const float sy = r.next() < (cap ? 0.8f : 0.6f) ? 1.0f : -1.0f;
        c.n[0] = (r.next() < 0.5f ? -1.0f : 1.0f) * (0.3f + r.next());
        c.n[1] = sy * (0.3f + r.next());
        c.n[2] = (r.next() < 0.5f ? -1.0f : 1.0f) * (0.3f + r.next());
        const float edge = r.next();   // now and then along an edge, not across a corner
        if (edge < 0.2f) c.n[0] = 0;
        else if (edge < 0.4f) c.n[2] = 0;
        else if (edge < 0.65f) c.n[1] = 0;   // an upright edge: the stone less square seen from above
        float most = 0, sum = 0;
        for (float a : c.n) {
            most = std::max(most, std::fabs(a));
            sum += std::fabs(a);
        }
        c.d = lerp(most, sum, 0.15f + 0.4f * r.next());
    }
    float body[3];
    for (int i = 0; i < 3; ++i) body[i] = static_cast<float>((col.body >> (16 - 8 * i)) & 0xFF) / 255.0f;
    for (int y = 0; y < h; ++y)
        for (int z = 0; z < wz; ++z)
            for (int x = 0; x < wx; ++x) {
                float q[3] = {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f};
                for (int i = 0; i < 3; ++i) q[i] = (q[i] - 0.5f * static_cast<float>(w[i])) / (0.5f * static_cast<float>(w[i]));
                bool kept = true;
                for (int k = 0; k < count && kept; ++k) kept = c_dot(cuts[k].n, q) <= cuts[k].d;
                if (!kept) continue;
                // the seam: its lowest row in by a voxel all round, darker, so a groove runs between it and the stone below
                if (seam && y == 0 && (x == 0 || z == 0 || x == wx - 1 || z == wz - 1) && wx > 2 && wz > 2) continue;
                uint32_t c = col.body;
                if (seam && y == 0) {
                    c = col.seam;
                } else if (q[1] < -0.3f) {   // darker towards its foot
                    const float dark = 1 - 0.25f * std::min(1.0f, (-q[1] - 0.3f) / 0.7f);
                    float rgb[3];
                    for (int i = 0; i < 3; ++i) rgb[i] = body[i] * dark;
                    c = pack(rgb);
                }
                v.set(x + 1, y, z + 1, c);
            }
    // cracks: a short walk down one side, on its outermost voxels
    static const int kSides[4][2] = {{0, -1}, {0, 1}, {2, -1}, {2, 1}};
    const int cracks = pick(r, 0, 2);
    for (int k = 0; k < cracks && h >= 4; ++k) {
        const auto& s = kSides[pick(r, 0, 3)];
        const int axis = s[0], other = 2 - axis;
        int a = 1 + pick(r, 1, std::max(1, w[other] - 2));
        int y = h - 2 - pick(r, 0, h / 3);
        for (int step = pick(r, 3, 6); step > 0 && y > 0; --step, --y) {
            for (int d = 0; d <= w[axis] + 1; ++d) {   // in from the outside to the first voxel
                const int across = s[1] < 0 ? d : w[axis] + 1 - d;
                const int x = axis == 0 ? across : a, z = axis == 0 ? a : across;
                const uint32_t here = v.get(x, y, z);
                if (here == 0) continue;
                if (here == col.body) v.set(x, y, z, col.crack);
                break;
            }
            a = std::clamp(a + pick(r, -1, 1), 1, w[other]);
        }
    }
}

// Copies stone `s` into `v` with its grid's corner at (x0, y0, z0), each row shifted by `shear` voxels a row (its own lean, at most
// kMaxShear), then paints the rim round its top (its top quarter) where it now lies.
void place_stone(Voxels& v, const Voxels& s, int x0, int y0, int z0, const float shear[2], const StoneColors& col) {
    for (int y = 0; y < s.n[1]; ++y) {
        const int dx = std::clamp(rounded(shear[0] * static_cast<float>(y)), -kMaxShear, kMaxShear);
        const int dz = std::clamp(rounded(shear[1] * static_cast<float>(y)), -kMaxShear, kMaxShear);
        for (int z = 0; z < s.n[2]; ++z)
            for (int x = 0; x < s.n[0]; ++x)
                if (const uint32_t c = s.get(x, y, z)) v.set(x0 + x + dx, y0 + y, z0 + z + dz, c);
    }
    paint_rim(v, y0 + s.n[1] - std::max(1, s.n[1] / 4), y0 + s.n[1], col.body, col.rim);   // round its top only
}

// The pillar's stones stacked in `v`: `w` voxels across its foot, `up` voxels from its lowest point (`under` of them under the
// ground at its axis) to its top. Each stone is about as tall as it is wide (fewer stones when the pillar is too short for them),
// sits on the top of the one below a voxel or so off, and leans its own way.
void build_stack(const Pillars& p, const float rgb[3], Rng& r, int w, int up, int under, Voxels& v) {
    v.reset(w + 2 * kMargin, up, w + 2 * kMargin);
    int stones = std::clamp(rounded(lerp(p.stones, r.next())), 1, 8);
    int wide[8][2], tall[8];
    for (;;) {
        float want[8], sum = 0;
        for (int k = 0; k < stones; ++k) {
            const float t = stones > 1 ? lerp(1.0f, p.taper, static_cast<float>(k) / static_cast<float>(stones - 1)) : 1.0f;
            for (int i = 0; i < 2; ++i) wide[k][i] = std::clamp(rounded(static_cast<float>(w) * t * (0.85f + 0.25f * r.next())), 2, w + 1);
            const float side = static_cast<float>(std::max(wide[k][0], wide[k][1]));
            want[k] = side * (k == stones - 1 && stones > 1 ? 0.55f + 0.35f * r.next() : 0.7f + 0.5f * r.next());
            sum += want[k];
        }
        const float fit = static_cast<float>(up - under) / sum;
        if (stones > 1 && (fit < 0.8f || up - under < 2 * stones)) {
            --stones;
            continue;
        }
        int y = under;
        for (int k = 0; k < stones; ++k) {
            const int rest = stones - 1 - k;
            tall[k] = k == stones - 1 ? up - y : std::clamp(rounded(want[k] * fit), 2, up - y - 2 * rest);
            y += tall[k];
        }
        tall[0] += under;
        break;
    }
    Voxels s;
    float cx = 0.5f * static_cast<float>(v.n[0]), cz = 0.5f * static_cast<float>(v.n[2]);
    int y = 0;
    for (int k = 0; k < stones; ++k) {
        const int wx = wide[k][0], wz = wide[k][1], h = tall[k];
        if (k > 0) {   // off the top of the one below by a voxel or so
            cx += static_cast<float>(pick(r, -1, 1));
            cz += static_cast<float>(pick(r, -1, 1));
        }
        const float shear[2] = {(2 * r.next() - 1) * 0.25f, (2 * r.next() - 1) * 0.25f};
        const StoneColors col = stone_colors(p, rgb, r);
        make_stone(s, wx, h, wz, k == stones - 1, k > 0, col, r);
        // its grid's corner: a voxel of room round it, kept in the pillar's grid with its lean
        const int x0 = std::clamp(rounded(cx - 0.5f * static_cast<float>(wx)) - 1, kMaxShear, v.n[0] - s.n[0] - kMaxShear);
        const int z0 = std::clamp(rounded(cz - 0.5f * static_cast<float>(wz)) - 1, kMaxShear, v.n[2] - s.n[2] - kMaxShear);
        place_stone(v, s, x0, y, z0, shear, col);
        // the next one stands on this one's top
        cx = static_cast<float>(x0 + 1) + 0.5f * static_cast<float>(wx) +
             static_cast<float>(std::clamp(rounded(shear[0] * static_cast<float>(h - 1)), -kMaxShear, kMaxShear));
        cz = static_cast<float>(z0 + 1) + 0.5f * static_cast<float>(wz) +
             static_cast<float>(std::clamp(rounded(shear[1] * static_cast<float>(h - 1)), -kMaxShear, kMaxShear));
        y += h;
    }
}

// The pillar's rubble in `v` (flat, on the ground): chipped stones lying round a foot `w` voxels across, some against it.
void build_rubble(const Pillars& p, const float rgb[3], Rng& r, int w, Voxels& v) {
    const int side = w + 2 * (kMargin + kRubbleReach);
    v.reset(side, kRubbleUp, side);
    const int pieces = std::clamp(rounded(lerp(p.rubble, r.next())), 0, 8);
    Voxels s;
    for (int i = 0; i < pieces; ++i) {
        const int big = std::max(3, w / 2);
        const int sx = pick(r, 2, big), sz = pick(r, 2, big);
        const int sy = pick(r, 2, std::min(kRubbleUp, std::max(2, std::min(sx, sz))));
        const float a = 2 * kPi * r.next();
        const float d = 0.5f * static_cast<float>(w + std::max(sx, sz)) + 2.5f * r.next();
        const StoneColors col = stone_colors(p, rgb, r);
        make_stone(s, sx, sy, sz, true, false, col, r);
        const int x0 = std::clamp(rounded(0.5f * static_cast<float>(side) + d * std::cos(a) - 0.5f * static_cast<float>(s.n[0])), 0, side - s.n[0]);
        const int z0 = std::clamp(rounded(0.5f * static_cast<float>(side) + d * std::sin(a) - 0.5f * static_cast<float>(s.n[2])), 0, side - s.n[2]);
        const float flat[2] = {0, 0};
        place_stone(v, s, x0, 0, z0, flat, col);
    }
}

// The faces of `v`'s voxels with no neighbour, each voxel's colour times a small random shade of its own and the face's. Voxel
// (x, y, z) spans x to x + 1 and z to z + 1 voxels of `cell` blocks from `corner` along axes[0] and axes[2], and base + y * cell
// to a cell more along axes[1] (blocks from `corner`); what is under `ground` there is cut off, and the lowest row reaches down
// to it (so a pillar pushed a little past its height still stands on the ground). Under-ground faces are left out.
void add_voxels(Mesh& m, const Voxels& v, const float corner[3], const float axes[3][3], float cell, float base, float ground,
                float block, uint32_t salt) {
    static const int kDir[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    static const float kQuad[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    static const int kTris[6] = {0, 1, 2, 0, 2, 3};
    for (int y = 0; y < v.n[1]; ++y) {
        const float u1 = base + static_cast<float>(y + 1) * cell;
        if (u1 <= ground) continue;
        const float u0 = y == 0 ? ground : std::max(ground, base + static_cast<float>(y) * cell);
        for (int z = 0; z < v.n[2]; ++z)
            for (int x = 0; x < v.n[0]; ++x) {
                const uint32_t c = v.get(x, y, z);
                if (c == 0) continue;
                // its own shade: 0.94 to 1.06, from where it is
                uint32_t h = (static_cast<uint32_t>(x) * 73856093u) ^ (static_cast<uint32_t>(y) * 19349663u) ^
                             (static_cast<uint32_t>(z) * 83492791u) ^ (salt * 2654435761u);
                h ^= h >> 13;
                h *= 0x5BD1E995u;
                h ^= h >> 15;
                const float own = 0.94f + 0.12f * static_cast<float>(h & 0xFFFF) / 65535.0f;
                float rgb[3];
                for (int i = 0; i < 3; ++i) rgb[i] = std::min(1.0f, static_cast<float>((c >> (16 - 8 * i)) & 0xFF) / 255.0f * own);
                const float lo[3] = {static_cast<float>(x) * cell, u0, static_cast<float>(z) * cell};
                const float hi[3] = {static_cast<float>(x + 1) * cell, u1, static_cast<float>(z + 1) * cell};
                for (int f = 0; f < 6; ++f) {
                    if (v.get(x + kDir[f][0], y + kDir[f][1], z + kDir[f][2]) != 0) continue;
                    const int a = f / 2;
                    const bool positive = (f & 1) != 0;
                    if (a == 1 && !positive && u0 <= ground) continue;   // under the ground
                    float n[3];
                    for (int i = 0; i < 3; ++i) n[i] = axes[a][i] * (positive ? 1.0f : -1.0f);
                    const uint32_t color = shaded(rgb, n);
                    const int ua = (a + 1) % 3, va = (a + 2) % 3;
                    MeshVertex q[4];
                    for (int k = 0; k < 4; ++k) {
                        float l[3];
                        l[a] = positive ? hi[a] : lo[a];
                        l[ua] = kQuad[k][0] > 0 ? hi[ua] : lo[ua];
                        l[va] = kQuad[k][1] > 0 ? hi[va] : lo[va];
                        float p[3];
                        for (int i = 0; i < 3; ++i) p[i] = corner[i] + block * (l[0] * axes[0][i] + l[1] * axes[1][i] + l[2] * axes[2][i]);
                        q[k] = {p[0], p[1], p[2], color};
                    }
                    for (int k : kTris) m.tris.push_back(q[k]);
                }
            }
    }
}

}  // namespace

EffectPlan plan_effects(const Rig& rig, const Clip& clip, float length_ms, const std::function<Pose(float)>& base_at) {
    EffectPlan plan;
    plan.length_ms = length_ms;
    if (!rig.has_effects() || !(length_ms > 0)) return plan;
    auto starts = [&](int parent, const std::vector<Step>& steps, const std::string& at) {
        std::vector<EffectStart> out;
        uint32_t index = 0;
        for (float t : marker_times(clip, at, length_ms)) {
            Timing tm;
            tm.elapsed_ms = t;
            tm.duration_ms = length_ms;
            tm.run_ms = length_ms;
            const Solved s = solve(rig, base_at ? base_at(t) : Pose{}, clip, tm);
            const Affine pw = parent >= 0 && static_cast<size_t>(parent) < s.world.size() ? s.world[parent] : Affine{};
            out.push_back({t, index++, pw * steps_matrix(steps, s.pose)});
        }
        return out;
    };
    for (const Shockwave& w : rig.shockwaves) plan.rings.push_back(starts(w.parent, w.steps, w.at));
    for (const Pillars& p : rig.pillars) plan.pillars.push_back(starts(p.parent, p.steps, p.at));
    return plan;
}

float pillar_out(float rise_ms, float hold_ms, float sink_ms, float age_ms) {
    if (!(age_ms >= 0)) return 0;
    if (age_ms < rise_ms) {
        // ease out with a little overshoot ("back", s = 1.2): 0 at the start, about 1.05 at 64 %, 1 at the end
        const float u = age_ms / rise_ms - 1, s = 1.2f;
        return 1 + (s + 1) * u * u * u + s * u * u;
    }
    age_ms -= rise_ms;
    if (age_ms < hold_ms) return 1;
    age_ms -= hold_ms;
    if (age_ms < sink_ms) {
        const float u = age_ms / sink_ms;
        return 1 - u * u * (3 - 2 * u);
    }
    return 0;
}

LiveEffects live_effects(const Rig& rig, const EffectPlan& plan, float time_ms, bool wrap) {
    LiveEffects live;
    const float block = rig.particles.block;
    for (size_t e = 0; e < plan.rings.size() && e < rig.shockwaves.size(); ++e) {
        const Shockwave& w = rig.shockwaves[e];
        for (const EffectStart& st : plan.rings[e]) {
            const float ms = since(st, time_ms, plan.length_ms, wrap);
            if (ms < 0) continue;
            const float up[3] = {0, 1, 0};
            LiveRing r;
            r.effect = e;
            turn_unit(st.frame, up, r.facing);
            const shockwave::Params p = ring_params(w, r.facing);
            r.age_ticks = ms / kEffectTickMs;
            if (!shockwave::alive_at(p, r.age_ticks)) continue;
            for (int i = 0; i < 3; ++i) r.centre[i] = st.frame.t[i];
            r.inner = shockwave::inner_at(p, r.age_ticks);
            r.outer = shockwave::outer_at(p, r.age_ticks);
            r.alpha = shockwave::alpha_at(p, r.age_ticks);
            live.rings.push_back(r);
        }
    }
    for (size_t e = 0; e < plan.pillars.size() && e < rig.pillars.size(); ++e) {
        const Pillars& p = rig.pillars[e];
        for (const EffectStart& st : plan.pillars[e]) {
            const float ms = since(st, time_ms, plan.length_ms, wrap);
            if (ms < 0) continue;
            for (int i = 0; i < p.count; ++i) {
                const uint32_t number = st.index * static_cast<uint32_t>(kMaxPillars) + static_cast<uint32_t>(i);
                // its foot: evenly over the ring of `radius`, drawn again while it falls in `clear`
                Rng where(p.seed, e, number, 1);
                float r = 0, a = 0, foot[3] = {0, 0, 0};
                bool found = false;
                for (int k = 0; k < kSpotTries && !found; ++k) {
                    r = std::sqrt(lerp(p.radius.lo * p.radius.lo, p.radius.hi * p.radius.hi, where.next()));
                    a = 2 * kPi * where.next();
                    const float local[3] = {r * std::cos(a) * block, 0, r * std::sin(a) * block};
                    st.frame.apply(local, foot);
                    const float x = foot[0] / block, y = foot[1] / block;
                    found = !p.has_clear || !(x >= p.clear[0] && x <= p.clear[2] && y >= p.clear[1] && y <= p.clear[3]);
                }
                if (!found) continue;
                Rng own(p.seed, e, number, 2);
                LivePillar q;
                q.effect = e;
                q.number = number;
                q.width = lerp(p.width, own.next());
                q.height = lerp(p.height, own.next());
                const float delay = lerp(p.delay, own.next()), hold = lerp(p.hold_ms, own.next());
                q.lean = p.lean * own.next();
                const float turn = p.turn * (2 * own.next() - 1) * kPi / 180;
                const float mix = own.next();
                const float start = (p.wave > 0 ? r / p.wave * 1000 : 0) + delay;
                q.out = pillar_out(p.rise_ms, hold, p.sink_ms, ms - start);
                if (q.out <= 0) continue;
                // its axes in the frame: turned about +y by its angle round the centre and its own turn, then leaned out from the centre
                const float yaw = a + turn;
                const float x0[3] = {std::cos(yaw), 0, std::sin(yaw)}, y0[3] = {0, 1, 0}, z0[3] = {-std::sin(yaw), 0, std::cos(yaw)};
                const float k[3] = {std::sin(a), 0, -std::cos(a)};   // up x out: turning about it leans +y towards (cos a, 0, sin a)
                const float* own_axes[3] = {x0, y0, z0};
                for (int j = 0; j < 3; ++j) {
                    float leaned[3];
                    about(k, q.lean * kPi / 180, own_axes[j], leaned);
                    turn_unit(st.frame, leaned, q.axes[j]);
                    turn_unit(st.frame, own_axes[j], q.flat[j]);
                }
                for (int j = 0; j < 3; ++j) {
                    q.foot[j] = foot[j];
                    q.rgb[j] = p.has_color_random ? lerp(p.color[j], p.color_random[j], mix) : p.color[j];
                }
                live.pillars.push_back(q);
            }
        }
    }
    return live;
}

Mesh pillar_mesh(const Rig& rig, const std::vector<LivePillar>& pillars) {
    Mesh m;
    const float block = rig.particles.block;
    Voxels v;
    for (const LivePillar& q : pillars) {
        if (q.effect >= rig.pillars.size() || !(q.out > 0)) continue;
        const Pillars& p = rig.pillars[q.effect];
        // Its axis runs from `low` (under the ground plane by half its foot times the lean's tangent, so a leaning foot never
        // floats) to its height; it slides up out of the ground, so its top stone comes out first.
        const float low = -0.5f * q.width * std::tan(q.lean * kPi / 180);
        const float length = q.height - low;
        const float cell = std::max({p.cell, q.width / kMaxAcross, length / kMaxUp});
        const int w = std::max(2, rounded(q.width / cell));
        const int up = std::max(2, rounded(length / cell));
        const int under = std::clamp(rounded(-low / cell), 0, up - 2);
        Rng r(p.seed, q.effect, q.number, 3);
        auto corner_of = [&](const float axes[3][3], float out[3]) {   // the grid's corner: centred on the foot
            for (int i = 0; i < 3; ++i)
                out[i] = q.foot[i] - block * cell * 0.5f * (static_cast<float>(v.n[0]) * axes[0][i] + static_cast<float>(v.n[2]) * axes[2][i]);
        };
        float corner[3];
        build_stack(p, q.rgb, r, w, up, under, v);
        corner_of(q.axes, corner);
        add_voxels(m, v, corner, q.axes, cell, low + (q.out - 1) * static_cast<float>(up) * cell, low, block, 2 * q.number + 1);
        build_rubble(p, q.rgb, r, w, v);
        corner_of(q.flat, corner);
        add_voxels(m, v, corner, q.flat, cell, (std::min(q.out, 1.0f) - 1) * static_cast<float>(kRubbleUp) * cell, 0, block, 2 * q.number + 2);
    }
    return m;
}

Mesh ring_mesh(const Rig& rig, const std::vector<LiveRing>& rings) {
    Mesh m;
    const float block = rig.particles.block;
    std::vector<shockwave::Vertex> cells;
    std::vector<shockwave::Vertex> smooth(shockwave::kRingVertices);
    for (const LiveRing& r : rings) {
        if (r.effect >= rig.shockwaves.size()) continue;
        const shockwave::Params p = ring_params(rig.shockwaves[r.effect], r.facing);
        const std::vector<shockwave::Vertex>* src = &cells;
        if (p.pixel_size > 0) {
            if (!shockwave::build_pixel_ring(p, r.age_ticks, cells)) continue;
        } else {
            if (!shockwave::build_ring(p, r.age_ticks, smooth.data())) continue;
            src = &smooth;
        }
        for (const shockwave::Vertex& v : *src)
            m.tris.push_back({r.centre[0] + v.x * block, r.centre[1] + v.y * block, r.centre[2] + v.z * block, v.argb});
    }
    return m;
}

}  // namespace kanimator
