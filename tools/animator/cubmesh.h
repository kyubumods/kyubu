// The Animator's voxel mesher (openspec add-ability-animation, task 6.3): a `.cub` model, or a box, as a list of coloured triangles
// for the view. Pure: no Direct3D, no ImGui. The `.cub` layout is docs/assets.md's: three little-endian uint32 sizes (x, y, z), then
// one RGB triple per voxel, x fastest, then y, then z; (0,0,0) is an empty voxel.
//
// A mesh is centred on its node: a model of size (sx, sy, sz) spans -s/2 to +s/2 on each axis, as a `box W D H` of a rig does, so the
// rig's own steps place it. Voxels are one unit; x right, y forward, z up, as everywhere in the rig reference. Only faces that border
// an empty voxel (or the outside) are made. Each face is two triangles, wound so that the right-handed normal points out of the
// solid. The vertex colour is the voxel's colour times a fixed shade per face direction (the top is the brightest), so the figure
// reads as solid without a light.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "camera.h"

namespace kanimator {

constexpr int kMaxCubSize = 256;   // per axis, the format's own bound

struct CubModel {
    int size[3] = {0, 0, 0};       // x, y, z
    std::vector<uint8_t> rgb;      // 3 bytes per voxel, x fastest
    bool solid(int x, int y, int z) const;     // false outside the model and for (0,0,0)
    const uint8_t* voxel(int x, int y, int z) const;
};

// Reads a model from its bytes. False with `err` set (one sentence) when the length does not match the sizes, a size is 0 or above
// 256, or the model has no voxel.
bool parse_cub(const uint8_t* data, size_t size, CubModel& out, std::string& err);

// One vertex of a mesh. The layout is Direct3D's D3DFVF_XYZ | D3DFVF_DIFFUSE (16 bytes): the view draws a mesh as it is.
struct MeshVertex {
    float x = 0, y = 0, z = 0;
    uint32_t color = 0xFFFFFFFFu;  // 0xAARRGGBB
};

struct Mesh {
    std::vector<MeshVertex> tris;  // three vertices per triangle
    size_t triangle_count() const { return tris.size() / 3; }
    bool empty() const { return tris.empty(); }
};

// The faces of a model, hidden ones left out. An empty mesh for an empty model.
Mesh mesh_cub(const CubModel& model);
// A box of w x d x h (x, y, z) in voxels, six faces, one colour (0xRRGGBB; the alpha is made opaque).
Mesh mesh_box(float w, float d, float h, uint32_t rgb);

// The corners of the box around the mesh. False for an empty mesh.
bool mesh_bounds(const Mesh& m, Vec3& lo, Vec3& hi);

// The shade of a face looking along +axis (positive) or -axis, 0..1. Axis 0 is x, 1 is y, 2 is z.
float face_shade(int axis, bool positive);

}  // namespace kanimator
