#include "camera.h"

#include <algorithm>
#include <cmath>

namespace kanimator {

namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

}  // namespace

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(Vec3 a) {
    float l = length(a);
    return l > 0 ? a * (1.0f / l) : Vec3{};
}

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

Mat4 transposed(const Mat4& a) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[j][i];
    return r;
}

Mat4 mat4_from_rt(const float r[3][3], const float t[3]) {
    Mat4 m;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m.m[i][j] = r[i][j];
        m.m[i][3] = t[i];
    }
    return m;
}

Vec3 Camera::eye() const {
    float yaw = yaw_deg * kDegToRad, pitch = pitch_deg * kDegToRad;
    Vec3 dir = {std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
    return target + dir * distance;
}

Vec3 Camera::forward() const { return normalized(target - eye()); }

Vec3 Camera::right() const { return normalized(cross(forward(), Vec3{0, 0, 1})); }

Vec3 Camera::up() const { return cross(right(), forward()); }

void Camera::set_preset(ViewPreset p) {
    switch (p) {
    case ViewPreset::Front: yaw_deg = 0; pitch_deg = 0; break;
    case ViewPreset::Left: yaw_deg = -90; pitch_deg = 0; break;
    case ViewPreset::Free: break;
    }
}

void Camera::orbit(float dx_px, float dy_px) {
    yaw_deg += dx_px * kOrbitDegPerPixel;     // a turntable: a drag to the right swings the figure's front to the right
    yaw_deg = std::fmod(yaw_deg, 360.0f);
    pitch_deg = std::clamp(pitch_deg + dy_px * kOrbitDegPerPixel, -kMaxPitchDeg, kMaxPitchDeg);
}

void Camera::pan(float dx_px, float dy_px, float viewport_h_px) {
    if (viewport_h_px <= 0) return;
    float per_pixel = 2.0f * distance * std::tan(fov_y_deg * 0.5f * kDegToRad) / viewport_h_px;   // voxels at the target's depth
    target = target - right() * (dx_px * per_pixel) + up() * (dy_px * per_pixel);
}

void Camera::zoom(float steps) {
    distance = std::clamp(distance * std::pow(kZoomPerStep, steps), kMinDistance, kMaxDistance);
}

void Camera::frame(Vec3 centre, float radius) {
    target = centre;
    float half = std::tan(fov_y_deg * 0.5f * kDegToRad);
    distance = std::clamp(radius / half * 1.15f, kMinDistance, kMaxDistance);
}

Mat4 view_matrix(const Camera& c) {
    Vec3 f = c.forward(), r = c.right(), u = c.up(), e = c.eye();
    Mat4 v;
    v.m[0][0] = r.x; v.m[0][1] = r.y; v.m[0][2] = r.z; v.m[0][3] = -dot(r, e);
    v.m[1][0] = u.x; v.m[1][1] = u.y; v.m[1][2] = u.z; v.m[1][3] = -dot(u, e);
    v.m[2][0] = -f.x; v.m[2][1] = -f.y; v.m[2][2] = -f.z; v.m[2][3] = dot(f, e);
    return v;
}

Mat4 projection_matrix(const Camera& c, float aspect) {
    float fy = 1.0f / std::tan(c.fov_y_deg * 0.5f * kDegToRad);
    float n = c.near_z, f = c.far_z;
    Mat4 p;
    for (auto& row : p.m)
        for (float& v : row) v = 0;
    p.m[0][0] = fy / (aspect > 0 ? aspect : 1.0f);
    p.m[1][1] = fy;
    p.m[2][2] = -f / (f - n);
    p.m[2][3] = -n * f / (f - n);
    p.m[3][2] = -1;
    return p;
}

bool project(const Camera& c, const Viewport& vp, Vec3 world, float& px, float& py, float* depth) {
    Mat4 m = projection_matrix(c, vp.h > 0 ? vp.w / vp.h : 1.0f) * view_matrix(c);
    float in[4] = {world.x, world.y, world.z, 1}, out[4];
    for (int i = 0; i < 4; ++i) out[i] = m.m[i][0] * in[0] + m.m[i][1] * in[1] + m.m[i][2] * in[2] + m.m[i][3] * in[3];
    if (out[3] <= 0) return false;
    float nx = out[0] / out[3], ny = out[1] / out[3];
    px = vp.x + (nx * 0.5f + 0.5f) * vp.w;
    py = vp.y + (0.5f - ny * 0.5f) * vp.h;
    if (depth) *depth = out[2] / out[3];
    return true;
}

Ray ray_through_pixel(const Camera& c, const Viewport& vp, float px, float py) {
    float nx = vp.w > 0 ? (px - vp.x) / vp.w * 2.0f - 1.0f : 0;
    float ny = vp.h > 0 ? 1.0f - (py - vp.y) / vp.h * 2.0f : 0;
    float t = std::tan(c.fov_y_deg * 0.5f * kDegToRad);
    float aspect = vp.h > 0 ? vp.w / vp.h : 1.0f;
    Ray r;
    r.origin = c.eye();
    r.dir = normalized(c.right() * (nx * t * aspect) + c.up() * (ny * t) + c.forward());
    return r;
}

Sphere bounding_sphere(const Vec3* pts, size_t n) {
    Sphere s;
    s.radius = 1;
    if (n == 0) return s;
    Vec3 lo = pts[0], hi = pts[0];
    for (size_t i = 1; i < n; ++i) {
        lo = {std::min(lo.x, pts[i].x), std::min(lo.y, pts[i].y), std::min(lo.z, pts[i].z)};
        hi = {std::max(hi.x, pts[i].x), std::max(hi.y, pts[i].y), std::max(hi.z, pts[i].z)};
    }
    s.centre = (lo + hi) * 0.5f;
    s.radius = std::max(length(hi - lo) * 0.5f, 1.0f);
    return s;
}

}  // namespace kanimator
