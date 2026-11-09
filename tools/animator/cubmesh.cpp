#include "cubmesh.h"

#include <algorithm>

namespace kanimator {

namespace {

// [axis][positive]
constexpr float kShade[3][2] = {{0.65f, 0.85f}, {0.55f, 0.75f}, {0.5f, 1.0f}};

uint32_t shaded(uint32_t r, uint32_t g, uint32_t b, float shade) {
    auto ch = [shade](uint32_t c) { return static_cast<uint32_t>(std::min(255.0f, static_cast<float>(c) * shade + 0.5f)); };
    return 0xFF000000u | ch(r) << 16 | ch(g) << 8 | ch(b);
}

// One quad of the face looking along +axis or -axis. `lo` is the corner of the face with the smallest coordinates; the quad spans
// `du` along axis (a + 1) % 3 and `dv` along axis (a + 2) % 3, so u x v is +axis and the +axis face is wound u then v.
void emit_face(Mesh& m, const float lo[3], int axis, bool positive, float du, float dv, uint32_t color) {
    int u = (axis + 1) % 3, v = (axis + 2) % 3;
    float c[4][3];
    for (auto& p : c) std::copy(lo, lo + 3, p);
    c[1][u] += du;
    c[2][u] += du;
    c[2][v] += dv;
    c[3][v] += dv;
    // The corners go 0 (lo), 1 (+u), 2 (+u +v), 3 (+v): counter-clockwise seen from +axis. A face looking along -axis is wound back.
    static const int kForward[6] = {0, 1, 2, 0, 2, 3};
    static const int kBackward[6] = {0, 2, 1, 0, 3, 2};
    const int* order = positive ? kForward : kBackward;
    for (int i = 0; i < 6; ++i) {
        MeshVertex vtx;
        vtx.x = c[order[i]][0];
        vtx.y = c[order[i]][1];
        vtx.z = c[order[i]][2];
        vtx.color = color;
        m.tris.push_back(vtx);
    }
}

}  // namespace

float face_shade(int axis, bool positive) { return kShade[axis][positive ? 1 : 0]; }

const uint8_t* CubModel::voxel(int x, int y, int z) const {
    return rgb.data() + 3 * (static_cast<size_t>(x) + static_cast<size_t>(size[0]) * (static_cast<size_t>(y) + static_cast<size_t>(size[1]) * z));
}

bool CubModel::solid(int x, int y, int z) const {
    if (x < 0 || y < 0 || z < 0 || x >= size[0] || y >= size[1] || z >= size[2]) return false;
    const uint8_t* p = voxel(x, y, z);
    return p[0] != 0 || p[1] != 0 || p[2] != 0;
}

bool parse_cub(const uint8_t* data, size_t size, CubModel& out, std::string& err) {
    if (size < 12) {
        err = "the model is shorter than its 12-byte header";
        return false;
    }
    uint32_t s[3];
    for (int i = 0; i < 3; ++i)
        s[i] = static_cast<uint32_t>(data[4 * i]) | static_cast<uint32_t>(data[4 * i + 1]) << 8 |
               static_cast<uint32_t>(data[4 * i + 2]) << 16 | static_cast<uint32_t>(data[4 * i + 3]) << 24;
    for (int i = 0; i < 3; ++i) {
        if (s[i] == 0 || s[i] > static_cast<uint32_t>(kMaxCubSize)) {
            err = "a size of the model is 0 or above 256";
            return false;
        }
    }
    size_t voxels = static_cast<size_t>(s[0]) * s[1] * s[2];
    if (size != 12 + 3 * voxels) {
        err = "the length of the model does not match its sizes";
        return false;
    }
    CubModel m;
    for (int i = 0; i < 3; ++i) m.size[i] = static_cast<int>(s[i]);
    m.rgb.assign(data + 12, data + size);
    bool any = false;
    for (size_t i = 0; i < voxels && !any; ++i) any = m.rgb[3 * i] != 0 || m.rgb[3 * i + 1] != 0 || m.rgb[3 * i + 2] != 0;
    if (!any) {
        err = "the model has no voxel";
        return false;
    }
    out = std::move(m);
    return true;
}

Mesh mesh_cub(const CubModel& model) {
    Mesh mesh;
    if (model.rgb.empty()) return mesh;
    const float half[3] = {model.size[0] * 0.5f, model.size[1] * 0.5f, model.size[2] * 0.5f};
    for (int z = 0; z < model.size[2]; ++z)
        for (int y = 0; y < model.size[1]; ++y)
            for (int x = 0; x < model.size[0]; ++x) {
                if (!model.solid(x, y, z)) continue;
                const uint8_t* rgb = model.voxel(x, y, z);
                const int at[3] = {x, y, z};
                for (int axis = 0; axis < 3; ++axis)
                    for (int side = 0; side < 2; ++side) {
                        int n[3] = {x, y, z};
                        n[axis] += side ? 1 : -1;
                        if (model.solid(n[0], n[1], n[2])) continue;
                        float lo[3] = {at[0] - half[0], at[1] - half[1], at[2] - half[2]};
                        if (side) lo[axis] += 1;
                        emit_face(mesh, lo, axis, side == 1, 1, 1, shaded(rgb[0], rgb[1], rgb[2], kShade[axis][side]));
                    }
            }
    return mesh;
}

Mesh mesh_box(float w, float d, float h, uint32_t rgb) {
    Mesh mesh;
    if (!(w > 0 && d > 0 && h > 0)) return mesh;
    const float size[3] = {w, d, h};
    for (int axis = 0; axis < 3; ++axis)
        for (int side = 0; side < 2; ++side) {
            float lo[3] = {-w * 0.5f, -d * 0.5f, -h * 0.5f};
            if (side) lo[axis] += size[axis];
            emit_face(mesh, lo, axis, side == 1, size[(axis + 1) % 3], size[(axis + 2) % 3],
                      shaded((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, kShade[axis][side]));
        }
    return mesh;
}

bool mesh_bounds(const Mesh& m, Vec3& lo, Vec3& hi) {
    if (m.tris.empty()) return false;
    lo = hi = {m.tris[0].x, m.tris[0].y, m.tris[0].z};
    for (const MeshVertex& v : m.tris) {
        lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
    }
    return true;
}

}  // namespace kanimator
