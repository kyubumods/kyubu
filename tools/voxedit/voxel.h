// kyubu-voxedit's voxel core: a model as a grid of colours, the cells a tool touches, picking with a ray, a mesh per chunk and the
// undo steps. Pure: no windows.h, no ImGui, no Direct3D (tests/unit/test_voxedit.cpp runs it).
//
// A grid is a `.cub` model (docs/assets.md): x fastest, then y, then z; (0,0,0) is an empty voxel. As in the Animator, a model is
// centred on its node: a model of size s spans -s/2 to +s/2, so cell (x, y, z) is the unit box from (x, y, z) - s/2. Growing or
// trimming a grid keeps that centre (the same number of cells is added or removed on both sides of an axis), so a piece of a rig
// stays where its rig puts it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera.h"
#include "cubmesh.h"

namespace kvox {

using kanimator::Mesh;
using kanimator::Vec3;

constexpr int kMaxSize = kanimator::kMaxCubSize;   // per axis
constexpr int kChunk = 16;                          // cells per chunk side; a chunk is meshed on its own

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    bool empty() const { return r == 0 && g == 0 && b == 0; }
    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Rgb& o) const { return !(*this == o); }
    uint32_t packed() const { return uint32_t(r) << 16 | uint32_t(g) << 8 | b; }
};
// A colour a voxel can hold: pure black means empty in a .cub, so it becomes (1, 1, 1).
Rgb solid_colour(Rgb c);

struct Cell {
    int x = 0, y = 0, z = 0;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct Grid {
    int size[3] = {0, 0, 0};
    std::vector<uint8_t> rgb;   // 3 bytes per cell

    void resize(int sx, int sy, int sz);   // all empty
    bool inside(int x, int y, int z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < size[0] && y < size[1] && z < size[2];
    }
    bool inside(Cell c) const { return inside(c.x, c.y, c.z); }
    size_t index(int x, int y, int z) const { return size_t(x) + size_t(size[0]) * (size_t(y) + size_t(size[1]) * size_t(z)); }
    Cell cell_of(size_t index) const;
    Rgb get(int x, int y, int z) const;   // empty outside
    Rgb get(Cell c) const { return get(c.x, c.y, c.z); }
    Rgb at(size_t index) const { return {rgb[3 * index], rgb[3 * index + 1], rgb[3 * index + 2]}; }
    void put(size_t index, Rgb c);
    bool solid(int x, int y, int z) const { return !get(x, y, z).empty(); }
    size_t cells() const { return size_t(size[0]) * size[1] * size[2]; }
    size_t count() const;   // solid voxels
    // The centre of a cell in the model's own frame (the model centred on its node).
    Vec3 centre_of(Cell c) const;
    // The cell a point of the model's frame is in (may be outside the grid).
    Cell cell_at(Vec3 p) const;
};

// Reads a .cub. False with `err` set when the bytes are not one (an empty model is accepted: it can be drawn into).
bool parse_grid(const uint8_t* data, size_t size, Grid& out, std::string& err);
// The bytes of a .cub.
std::string cub_bytes(const Grid& g);

// The cells shown and picked: inclusive bounds per axis. Off: all of them.
struct Slice {
    bool on = false;
    int lo[3] = {0, 0, 0};
    int hi[3] = {0, 0, 0};
    bool shows(int x, int y, int z) const;
    // Keeps the bounds inside the grid, lo <= hi.
    void clamp_to(const Grid& g);
};

// ----- picking

struct Hit {
    bool solid = false;      // the ray hit a shown voxel: `cell`, and `normal` the face it entered by (0 when it started inside)
    bool floor = false;      // or it hit the floor of the grid (its lowest shown z plane) from above: `cell` is the cell on it
    Cell cell;
    int normal[3] = {0, 0, 0};
    float t = 0;             // the distance along the ray
    bool any() const { return solid || floor; }
    // The cell a voxel would be added to: the one in front of the face hit, or the floor cell. False when there is none.
    bool add_cell(Cell& out) const;
};
// A ray in the model's own frame (`dir` need not be unit; `t` is in its units).
Hit raycast(const Grid& g, const Slice& slice, Vec3 origin, Vec3 dir);

// ----- what a tool touches

enum class Shape : uint8_t { Cube, Sphere };
// The cells of a brush `size` cells wide (1 to 64) around `centre`. An even size leans to +x +y +z.
std::vector<Cell> brush_cells(Cell centre, int size, Shape shape);
// The cells of the box with corners a and b (inclusive).
std::vector<Cell> box_cells(Cell a, Cell b);
// The shown cells 6-connected to `start` with its colour (a solid start). Empty when `start` is empty.
std::vector<Cell> connected_cells(const Grid& g, const Slice& slice, Cell start);
// Every shown cell of colour `c`.
std::vector<Cell> cells_of_colour(const Grid& g, const Slice& slice, Rgb c);

// The cell mirrored across the grid's middle on x (the plane x = 0 of the model's frame).
Cell mirror_x(const Grid& g, Cell c);

// ----- edits and undo

enum class Op : uint8_t { Add, Remove, Paint };

struct Change {
    uint32_t index = 0;
    Rgb before, after;
};

// One piece's part of an undo step: either the cells changed, or (after the grid's size changed) the whole other grid.
struct PieceStep {
    int piece = 0;
    bool snapshot = false;
    Grid other;                    // snapshot: the grid on the other side of the step
    std::vector<Change> changes;   // otherwise
    uint64_t rev_before = 0, rev_after = 0;   // the piece's revision on each side (doc.h: unsaved = revision != saved one)
};
struct Step {
    std::string what;
    std::vector<PieceStep> pieces;
};

// Collects one stroke's edits of a grid (a press to a release): each cell keeps its first `before` and its last `after`.
class Stroke {
public:
    void begin(int piece, const Grid& g);
    // Sets a cell, as `op` with colour `c` would (Add only fills an empty cell, Paint only a solid one, Remove empties). Outside the
    // grid does nothing. True when the cell changed.
    bool apply(Grid& g, Cell cell, Op op, Rgb c);
    // Before the grid's size changes: from then on the step keeps the grid as it was at begin().
    void before_resize(const Grid& g);
    bool touched() const { return resized_ || !order_.empty(); }
    // The step, or false when nothing changed. Leaves the stroke empty.
    bool finish(PieceStep& out);
    const std::vector<uint32_t>& changed() const { return order_; }   // indexes changed since begin, in order (unless resized)
    bool resized() const { return resized_; }

private:
    int piece_ = 0;
    bool resized_ = false;
    Grid start_;                    // a copy at begin(), kept only once the size changes
    const Grid* live_ = nullptr;
    std::vector<uint32_t> order_;
    std::vector<Rgb> before_;       // parallel to order_
    std::unordered_map<uint32_t, uint32_t> slot_;   // cell index -> its place in order_
};

// Steps one piece back (undo) or forward (redo). A snapshot step swaps the grids.
void undo_piece(Grid& g, PieceStep& s);
void redo_piece(Grid& g, PieceStep& s);

// Grows the grid so the box lo..hi fits, by the same number of cells on both sides of each axis (the size stays under kMaxSize:
// what does not fit stays out). `shift` is how far each cell moved. False when nothing changed.
bool grow_to_fit(Grid& g, Cell lo, Cell hi, int shift[3]);
// Removes empty layers, as many on both sides of each axis, keeping at least one cell per axis. False when nothing changed.
bool trim_even(Grid& g, int shift[3]);

// ----- meshing

int chunk_count(const Grid& g, int axis);   // chunks along an axis
// The faces of one chunk's shown voxels that border a hidden or empty cell, in the model's own frame, coloured like the Animator's
// meshes (a fixed shade per face direction).
Mesh mesh_chunk(const Grid& g, const Slice& slice, int cx, int cy, int cz);

// The colours of the grid, most used first, with their counts.
struct ColourCount {
    Rgb c;
    size_t n = 0;
};
std::vector<ColourCount> colour_counts(const Grid& g, size_t max);

}  // namespace kvox
