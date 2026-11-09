#include "voxel.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace kvox {

Rgb solid_colour(Rgb c) { return c.empty() ? Rgb{1, 1, 1} : c; }

void Grid::resize(int sx, int sy, int sz) {
    size[0] = sx;
    size[1] = sy;
    size[2] = sz;
    rgb.assign(cells() * 3, 0);
}

Cell Grid::cell_of(size_t i) const {
    Cell c;
    c.x = int(i % size_t(size[0]));
    i /= size_t(size[0]);
    c.y = int(i % size_t(size[1]));
    c.z = int(i / size_t(size[1]));
    return c;
}

Rgb Grid::get(int x, int y, int z) const {
    if (!inside(x, y, z)) return {};
    return at(index(x, y, z));
}

void Grid::put(size_t i, Rgb c) {
    rgb[3 * i] = c.r;
    rgb[3 * i + 1] = c.g;
    rgb[3 * i + 2] = c.b;
}

size_t Grid::count() const {
    size_t n = 0;
    for (size_t i = 0; i < cells(); ++i) n += !at(i).empty();
    return n;
}

Vec3 Grid::centre_of(Cell c) const {
    return {c.x + 0.5f - size[0] * 0.5f, c.y + 0.5f - size[1] * 0.5f, c.z + 0.5f - size[2] * 0.5f};
}

Cell Grid::cell_at(Vec3 p) const {
    return {int(std::floor(p.x + size[0] * 0.5f)), int(std::floor(p.y + size[1] * 0.5f)), int(std::floor(p.z + size[2] * 0.5f))};
}

bool parse_grid(const uint8_t* data, size_t n, Grid& out, std::string& err) {
    if (n < 12) {
        err = "the file is shorter than a .cub header (12 bytes)";
        return false;
    }
    uint32_t s[3];
    for (int i = 0; i < 3; ++i)
        s[i] = uint32_t(data[4 * i]) | uint32_t(data[4 * i + 1]) << 8 | uint32_t(data[4 * i + 2]) << 16 | uint32_t(data[4 * i + 3]) << 24;
    for (uint32_t v : s)
        if (v == 0 || v > uint32_t(kMaxSize)) {
            err = "a size of the model is 0 or above 256";
            return false;
        }
    const size_t cells = size_t(s[0]) * s[1] * s[2];
    if (n != 12 + 3 * cells) {
        err = "the length of the file does not match the sizes in its header";
        return false;
    }
    Grid g;
    for (int i = 0; i < 3; ++i) g.size[i] = int(s[i]);
    g.rgb.assign(data + 12, data + n);
    out = std::move(g);
    return true;
}

std::string cub_bytes(const Grid& g) {
    std::string b(12, '\0');
    for (int i = 0; i < 3; ++i) {
        const uint32_t v = uint32_t(g.size[i]);
        for (int k = 0; k < 4; ++k) b[4 * i + k] = char((v >> (8 * k)) & 0xFF);
    }
    b.append(reinterpret_cast<const char*>(g.rgb.data()), g.rgb.size());
    return b;
}

bool Slice::shows(int x, int y, int z) const {
    if (!on) return true;
    return x >= lo[0] && x <= hi[0] && y >= lo[1] && y <= hi[1] && z >= lo[2] && z <= hi[2];
}

void Slice::clamp_to(const Grid& g) {
    for (int a = 0; a < 3; ++a) {
        lo[a] = std::clamp(lo[a], 0, std::max(g.size[a] - 1, 0));
        hi[a] = std::clamp(hi[a], lo[a], std::max(g.size[a] - 1, 0));
    }
}

// ----- picking

bool Hit::add_cell(Cell& out) const {
    if (floor) {
        out = cell;
        return true;
    }
    if (!solid || (normal[0] == 0 && normal[1] == 0 && normal[2] == 0)) return false;
    out = {cell.x + normal[0], cell.y + normal[1], cell.z + normal[2]};
    return true;
}

Hit raycast(const Grid& g, const Slice& slice, Vec3 origin, Vec3 dir) {
    Hit hit;
    // In cell units: q = p + size/2, so cell = floor(q).
    float o[3] = {origin.x + g.size[0] * 0.5f, origin.y + g.size[1] * 0.5f, origin.z + g.size[2] * 0.5f};
    float d[3] = {dir.x, dir.y, dir.z};
    // The shown box: the grid, or the slice.
    float lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        lo[a] = slice.on ? float(slice.lo[a]) : 0.0f;
        hi[a] = slice.on ? float(slice.hi[a] + 1) : float(g.size[a]);
    }
    float t0 = 0, t1 = 1e30f;
    int entry_axis = -1;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(d[a]) < 1e-12f) {
            if (o[a] < lo[a] || o[a] > hi[a]) return hit;
            continue;
        }
        float ta = (lo[a] - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a];
        if (ta > tb) std::swap(ta, tb);
        if (ta > t0) {
            t0 = ta;
            entry_axis = a;
        }
        t1 = std::min(t1, tb);
        if (t0 > t1) return hit;
    }
    // Walk the cells from the entry point (3D DDA).
    float p[3];
    for (int a = 0; a < 3; ++a) p[a] = o[a] + d[a] * t0;
    int c[3], step[3];
    float tmax[3], tdelta[3];
    for (int a = 0; a < 3; ++a) {
        c[a] = int(std::floor(p[a]));
        if (a == entry_axis) c[a] = d[a] > 0 ? int(lo[a]) : int(hi[a]) - 1;   // exactly on the face: the cell inside
        c[a] = std::clamp(c[a], int(lo[a]), int(hi[a]) - 1);
        step[a] = d[a] > 0 ? 1 : d[a] < 0 ? -1 : 0;
        if (step[a] == 0) {
            tmax[a] = tdelta[a] = 1e30f;
        } else {
            const float next = step[a] > 0 ? float(c[a] + 1) : float(c[a]);
            tmax[a] = t0 + (next - p[a]) / d[a];
            tdelta[a] = std::fabs(1.0f / d[a]);
        }
    }
    int last_axis = entry_axis;
    float t = t0;
    for (int guard = 0; guard < 4 * kMaxSize; ++guard) {
        if (c[0] < lo[0] || c[1] < lo[1] || c[2] < lo[2] || c[0] >= hi[0] || c[1] >= hi[1] || c[2] >= hi[2]) break;
        if (g.solid(c[0], c[1], c[2]) && slice.shows(c[0], c[1], c[2])) {
            hit.solid = true;
            hit.cell = {c[0], c[1], c[2]};
            hit.t = t;
            if (last_axis >= 0) hit.normal[last_axis] = -step[last_axis];
            return hit;
        }
        int a = tmax[0] < tmax[1] ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
        t = tmax[a];
        if (t > t1 + 1e-4f) break;
        c[a] += step[a];
        tmax[a] += tdelta[a];
        last_axis = a;
    }
    // No voxel: the floor (the lowest shown z plane), seen from above.
    if (d[2] < 0 && o[2] >= lo[2]) {
        const float tf = (lo[2] - o[2]) / d[2];
        const float fx = o[0] + d[0] * tf, fy = o[1] + d[1] * tf;
        if (tf >= 0 && fx >= lo[0] && fx < hi[0] && fy >= lo[1] && fy < hi[1]) {
            hit.floor = true;
            hit.cell = {int(std::floor(fx)), int(std::floor(fy)), int(lo[2])};
            hit.t = tf;
            hit.normal[2] = 1;
        }
    }
    return hit;
}

// ----- what a tool touches

std::vector<Cell> brush_cells(Cell centre, int size, Shape shape) {
    size = std::clamp(size, 1, 64);
    const int lo = -(size - 1) / 2, hi = lo + size - 1;
    const float mid = (lo + hi) * 0.5f, r = size * 0.5f;
    std::vector<Cell> out;
    for (int z = lo; z <= hi; ++z)
        for (int y = lo; y <= hi; ++y)
            for (int x = lo; x <= hi; ++x) {
                if (shape == Shape::Sphere && size > 2) {
                    const float dx = x - mid, dy = y - mid, dz = z - mid;
                    if (dx * dx + dy * dy + dz * dz > r * r) continue;
                }
                out.push_back({centre.x + x, centre.y + y, centre.z + z});
            }
    return out;
}

std::vector<Cell> box_cells(Cell a, Cell b) {
    std::vector<Cell> out;
    const int x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    const int z0 = std::min(a.z, b.z), z1 = std::max(a.z, b.z);
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) out.push_back({x, y, z});
    return out;
}

std::vector<Cell> connected_cells(const Grid& g, const Slice& slice, Cell start) {
    std::vector<Cell> out;
    if (!g.inside(start) || g.get(start).empty() || !slice.shows(start.x, start.y, start.z)) return out;
    const Rgb want = g.get(start);
    std::vector<uint8_t> seen(g.cells(), 0);
    std::vector<Cell> todo{start};
    seen[g.index(start.x, start.y, start.z)] = 1;
    static const int kN[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    while (!todo.empty()) {
        Cell c = todo.back();
        todo.pop_back();
        out.push_back(c);
        for (const auto& n : kN) {
            Cell m{c.x + n[0], c.y + n[1], c.z + n[2]};
            if (!g.inside(m) || !slice.shows(m.x, m.y, m.z)) continue;
            const size_t i = g.index(m.x, m.y, m.z);
            if (seen[i] || g.at(i) != want) continue;
            seen[i] = 1;
            todo.push_back(m);
        }
    }
    return out;
}

std::vector<Cell> cells_of_colour(const Grid& g, const Slice& slice, Rgb c) {
    std::vector<Cell> out;
    if (c.empty()) return out;
    for (size_t i = 0; i < g.cells(); ++i) {
        if (g.at(i) != c) continue;
        Cell k = g.cell_of(i);
        if (slice.shows(k.x, k.y, k.z)) out.push_back(k);
    }
    return out;
}

Cell mirror_x(const Grid& g, Cell c) { return {g.size[0] - 1 - c.x, c.y, c.z}; }

// ----- edits and undo

void Stroke::begin(int piece, const Grid& g) {
    piece_ = piece;
    resized_ = false;
    start_ = Grid();
    live_ = &g;
    order_.clear();
    before_.clear();
    slot_.clear();
}

bool Stroke::apply(Grid& g, Cell cell, Op op, Rgb c) {
    if (!g.inside(cell)) return false;
    const size_t i = g.index(cell.x, cell.y, cell.z);
    const Rgb old = g.at(i);
    Rgb now = old;
    switch (op) {
    case Op::Add: if (old.empty()) now = solid_colour(c); break;
    case Op::Paint: if (!old.empty()) now = solid_colour(c); break;
    case Op::Remove: now = Rgb{}; break;
    }
    if (now == old) return false;
    if (!resized_) {
        if (slot_.find(uint32_t(i)) == slot_.end()) {
            slot_[uint32_t(i)] = uint32_t(order_.size());
            order_.push_back(uint32_t(i));
            before_.push_back(old);
        }
    }
    g.put(i, now);
    return true;
}

void Stroke::before_resize(const Grid& g) {
    if (resized_) return;
    // The grid as it was at begin(): the current one with the recorded cells put back.
    start_ = g;
    for (size_t k = 0; k < order_.size(); ++k) start_.put(order_[k], before_[k]);
    resized_ = true;
    order_.clear();
    before_.clear();
    slot_.clear();
}

bool Stroke::finish(PieceStep& out) {
    out = PieceStep();
    out.piece = piece_;
    bool any = false;
    if (resized_) {
        out.snapshot = true;
        out.other = std::move(start_);
        any = true;
    } else if (live_) {
        for (size_t k = 0; k < order_.size(); ++k) {
            const Rgb after = live_->at(order_[k]);
            if (after == before_[k]) continue;
            out.changes.push_back({order_[k], before_[k], after});
        }
        any = !out.changes.empty();
    }
    order_.clear();
    before_.clear();
    slot_.clear();
    resized_ = false;
    start_ = Grid();
    return any;
}

void undo_piece(Grid& g, PieceStep& s) {
    if (s.snapshot) {
        std::swap(g, s.other);
        return;
    }
    for (auto it = s.changes.rbegin(); it != s.changes.rend(); ++it) g.put(it->index, it->before);
}

void redo_piece(Grid& g, PieceStep& s) {
    if (s.snapshot) {
        std::swap(g, s.other);
        return;
    }
    for (const Change& c : s.changes) g.put(c.index, c.after);
}

namespace {
// A copy of `g` with `k` cells added (k > 0) or removed (k < 0) on both sides of each axis.
Grid regrid(const Grid& g, const int k[3]) {
    Grid n;
    n.resize(g.size[0] + 2 * k[0], g.size[1] + 2 * k[1], g.size[2] + 2 * k[2]);
    for (int z = 0; z < n.size[2]; ++z)
        for (int y = 0; y < n.size[1]; ++y)
            for (int x = 0; x < n.size[0]; ++x) {
                const int ox = x - k[0], oy = y - k[1], oz = z - k[2];
                if (g.inside(ox, oy, oz)) n.put(n.index(x, y, z), g.at(g.index(ox, oy, oz)));
            }
    return n;
}
}  // namespace

bool grow_to_fit(Grid& g, Cell lo, Cell hi, int shift[3]) {
    const int l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    int k[3];
    bool any = false;
    for (int a = 0; a < 3; ++a) {
        k[a] = std::max({0, -l[a], h[a] - (g.size[a] - 1)});
        k[a] = std::min(k[a], (kMaxSize - g.size[a]) / 2);
        shift[a] = k[a];
        any = any || k[a] > 0;
    }
    if (!any) return false;
    g = regrid(g, k);
    return true;
}

bool trim_even(Grid& g, int shift[3]) {
    int lo[3] = {g.size[0], g.size[1], g.size[2]}, hi[3] = {-1, -1, -1};
    for (size_t i = 0; i < g.cells(); ++i) {
        if (g.at(i).empty()) continue;
        Cell c = g.cell_of(i);
        const int v[3] = {c.x, c.y, c.z};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], v[a]);
            hi[a] = std::max(hi[a], v[a]);
        }
    }
    if (hi[0] < 0) return false;   // empty: nothing to keep
    int k[3];
    bool any = false;
    for (int a = 0; a < 3; ++a) {
        int cut = std::min(lo[a], g.size[a] - 1 - hi[a]);
        cut = std::min(cut, (g.size[a] - 1) / 2);
        k[a] = -cut;
        shift[a] = -cut;
        any = any || cut > 0;
    }
    if (!any) return false;
    g = regrid(g, k);
    return true;
}

// ----- meshing

int chunk_count(const Grid& g, int axis) { return (g.size[axis] + kChunk - 1) / kChunk; }

namespace {
uint32_t shaded(Rgb c, float shade) {
    auto ch = [shade](uint32_t v) { return uint32_t(std::min(255.0f, float(v) * shade + 0.5f)); };
    return 0xFF000000u | ch(c.r) << 16 | ch(c.g) << 8 | ch(c.b);
}
}  // namespace

Mesh mesh_chunk(const Grid& g, const Slice& slice, int cx, int cy, int cz) {
    Mesh m;
    const float half[3] = {g.size[0] * 0.5f, g.size[1] * 0.5f, g.size[2] * 0.5f};
    auto shown = [&](int x, int y, int z) { return g.solid(x, y, z) && slice.shows(x, y, z); };
    const int x0 = cx * kChunk, y0 = cy * kChunk, z0 = cz * kChunk;
    const int x1 = std::min(x0 + kChunk, g.size[0]), y1 = std::min(y0 + kChunk, g.size[1]), z1 = std::min(z0 + kChunk, g.size[2]);
    static const int kForward[6] = {0, 1, 2, 0, 2, 3};
    static const int kBackward[6] = {0, 2, 1, 0, 3, 2};
    for (int z = z0; z < z1; ++z)
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                if (!shown(x, y, z)) continue;
                const Rgb rgb = g.at(g.index(x, y, z));
                const int at[3] = {x, y, z};
                for (int axis = 0; axis < 3; ++axis)
                    for (int side = 0; side < 2; ++side) {
                        int n[3] = {x, y, z};
                        n[axis] += side ? 1 : -1;
                        if (shown(n[0], n[1], n[2])) continue;
                        float lo[3] = {at[0] - half[0], at[1] - half[1], at[2] - half[2]};
                        if (side) lo[axis] += 1;
                        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                        float c[4][3];
                        for (auto& p : c) std::copy(lo, lo + 3, p);
                        c[1][u] += 1;
                        c[2][u] += 1;
                        c[2][v] += 1;
                        c[3][v] += 1;
                        const uint32_t col = shaded(rgb, kanimator::face_shade(axis, side == 1));
                        const int* order = side ? kForward : kBackward;
                        for (int i = 0; i < 6; ++i) m.tris.push_back({c[order[i]][0], c[order[i]][1], c[order[i]][2], col});
                    }
            }
    return m;
}

std::vector<ColourCount> colour_counts(const Grid& g, size_t max) {
    std::unordered_map<uint32_t, size_t> n;
    for (size_t i = 0; i < g.cells(); ++i) {
        const Rgb c = g.at(i);
        if (!c.empty()) ++n[c.packed()];
    }
    std::vector<ColourCount> out;
    out.reserve(n.size());
    for (const auto& [k, v] : n) out.push_back({Rgb{uint8_t(k >> 16), uint8_t(k >> 8), uint8_t(k)}, v});
    std::sort(out.begin(), out.end(), [](const ColourCount& a, const ColourCount& b) {
        return a.n != b.n ? a.n > b.n : a.c.packed() < b.c.packed();
    });
    if (out.size() > max) out.resize(max);
    return out;
}

}  // namespace kvox
