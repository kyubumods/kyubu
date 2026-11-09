// The Animator's camera maths (openspec add-ability-animation, task 6.3): an orbit camera over the rig reference's frame, the two fixed
// views of a capture, a projection, and a ray through a pixel (6.4 picks parts with it). Pure: no windows.h, no ImGui, no Direct3D.
//
// The frame is the rig reference's (docs/animations.md, "Frame and order"): x to the figure's right, y forward, z up; right-handed.
// The figure faces +y. A capture's camera looks along +y whatever the player's yaw, so the figure's face is seen at body_yaw 180 and
// its left profile at 90 (design D11). In this camera:
//   Front  the camera is at +y of the target and looks along -y: the figure faces the camera and its right (+x) is on the screen's left.
//   Left   the camera is at -x of the target and looks along +x: the figure's left side faces the camera and it faces the screen's left.
// Matrices are column-vector, `m[row][col]`, p' = M p. View space is x right, y up, the camera looks down -z. Depth runs 0 at the near
// plane to 1 at the far plane, as Direct3D clips it; `transposed()` gives the row-vector form Direct3D takes.
#pragma once
#include <cstddef>

namespace kanimator {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
Vec3 operator+(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 a, Vec3 b);
Vec3 operator*(Vec3 a, float s);
float dot(Vec3 a, Vec3 b);
Vec3 cross(Vec3 a, Vec3 b);
float length(Vec3 a);
Vec3 normalized(Vec3 a);   // a zero vector stays zero

struct Mat4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};
Mat4 operator*(const Mat4& a, const Mat4& b);   // a * b: b acts first
Mat4 transposed(const Mat4& a);
// The 4x4 of a rotation `r` (row-major 3x3) and a translation `t`: the point p goes to r p + t. rig.h's Affine holds exactly these.
Mat4 mat4_from_rt(const float r[3][3], const float t[3]);

enum class ViewPreset { Free, Front, Left };

constexpr float kMinDistance = 10.0f;     // voxels from the target
constexpr float kMaxDistance = 1500.0f;
constexpr float kMaxPitchDeg = 89.0f;     // the up vector is the world's z, so straight up and down are not reachable
constexpr float kOrbitDegPerPixel = 0.4f;
constexpr float kZoomPerStep = 0.9f;      // one wheel step multiplies the distance by this (a step toward the target)

struct Camera {
    Vec3 target = {0, 0, 20};
    float yaw_deg = 0;       // 0: the camera is at +y of the target. Positive turns it toward +x.
    float pitch_deg = 0;     // positive lifts the camera above the target
    float distance = 120;
    float fov_y_deg = 35;
    float near_z = 1;
    float far_z = 3000;

    Vec3 eye() const;
    Vec3 forward() const;    // unit; from the eye to the target
    Vec3 right() const;      // unit; to the screen's right
    Vec3 up() const;         // unit; to the screen's top

    // Sets the angles of a fixed view and keeps the target and the distance. Free changes nothing.
    void set_preset(ViewPreset p);
    // A drag of (dx, dy) pixels, as a turntable: right moves the camera toward +x (the figure's front swings to the right), down
    // lifts it. Clamps the pitch.
    void orbit(float dx_px, float dy_px);
    // A drag of (dx, dy) pixels in a viewport `viewport_h_px` high: the scene follows the pointer.
    void pan(float dx_px, float dy_px, float viewport_h_px);
    // `steps` wheel steps, positive toward the target. Clamps the distance.
    void zoom(float steps);
    // Looks at a ball: the target is its centre and the distance shows all of it.
    void frame(Vec3 centre, float radius);
};

struct Viewport {
    float x = 0, y = 0, w = 1, h = 1;   // pixels; y grows downward
};

Mat4 view_matrix(const Camera& c);
Mat4 projection_matrix(const Camera& c, float aspect);

// The pixel a world point is drawn at, and its depth in 0..1. False when the point is at or behind the camera's plane.
bool project(const Camera& c, const Viewport& vp, Vec3 world, float& px, float& py, float* depth = nullptr);

struct Ray {
    Vec3 origin;
    Vec3 dir;   // unit
};
// The ray from the eye through the centre of the pixel (px, py) of the viewport.
Ray ray_through_pixel(const Camera& c, const Viewport& vp, float px, float py);

struct Sphere {
    Vec3 centre;
    float radius = 0;
};
// The centre of the box around the points and half its diagonal. Zero points give a unit ball at the origin.
Sphere bounding_sphere(const Vec3* pts, size_t n);

}  // namespace kanimator
