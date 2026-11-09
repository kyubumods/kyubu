// Unit tests of the Animator's view code (openspec add-ability-animation task 6.3): the camera and its two fixed views, a ray through
// a pixel, the `.cub` mesher, and the play controls. No window, no game.
#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "camera.h"
#include "cubmesh.h"
#include "playback.h"
#include "rig.h"

using namespace kanimator;
namespace anim = kyubu::game::anim;

namespace {

constexpr Viewport kVp = {100, 50, 800, 500};   // an off-origin, non-square viewport
constexpr float kCentreX = 100 + 400, kCentreY = 50 + 250;

void expect_vec(Vec3 v, float x, float y, float z) {
    CHECK(v.x == doctest::Approx(x).epsilon(1e-4).scale(1));
    CHECK(v.y == doctest::Approx(y).epsilon(1e-4).scale(1));
    CHECK(v.z == doctest::Approx(z).epsilon(1e-4).scale(1));
}

std::vector<uint8_t> cub_bytes(int sx, int sy, int sz, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> b;
    for (int s : {sx, sy, sz})
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((static_cast<uint32_t>(s) >> (8 * i)) & 0xFF));
    b.insert(b.end(), rgb.begin(), rgb.end());
    return b;
}

// A model of the given size with the voxels (index x + sx*(y + sy*z)) in `filled` set to (200, 100, 50).
CubModel model_with(int sx, int sy, int sz, const std::vector<int>& filled) {
    std::vector<uint8_t> rgb(3u * sx * sy * sz, 0);
    for (int i : filled) {
        rgb[3 * i] = 200;
        rgb[3 * i + 1] = 100;
        rgb[3 * i + 2] = 50;
    }
    auto bytes = cub_bytes(sx, sy, sz, rgb);
    CubModel m;
    std::string err;
    REQUIRE_MESSAGE(parse_cub(bytes.data(), bytes.size(), m, err), err);
    return m;
}

Vec3 tri_normal(const Mesh& m, size_t tri) {
    const MeshVertex* v = &m.tris[3 * tri];
    Vec3 a = {v[0].x, v[0].y, v[0].z}, b = {v[1].x, v[1].y, v[1].z}, c = {v[2].x, v[2].y, v[2].z};
    return normalized(cross(b - a, c - a));
}

}  // namespace

TEST_CASE("view: Front looks along -y, so the figure faces the camera and its right is on the screen's left") {
    Camera c;
    c.set_preset(ViewPreset::Front);
    expect_vec(c.forward(), 0, -1, 0);
    expect_vec(c.right(), -1, 0, 0);
    expect_vec(c.up(), 0, 0, 1);
    CHECK(dot(c.forward(), Vec3{0, 1, 0}) < 0);   // the figure faces +y: against the viewing direction

    float cx, cy, hx, hy, tx, ty;
    REQUIRE(project(c, kVp, c.target, cx, cy));
    CHECK(cx == doctest::Approx(kCentreX));
    CHECK(cy == doctest::Approx(kCentreY));
    REQUIRE(project(c, kVp, c.target + Vec3{10, 0, 0}, hx, hy));   // the figure's right
    CHECK(hx < cx);
    CHECK(hy == doctest::Approx(cy));
    REQUIRE(project(c, kVp, c.target + Vec3{0, 0, 10}, tx, ty));   // up
    CHECK(ty < cy);                                                // pixel rows grow downward
    CHECK(tx == doctest::Approx(cx));
}

TEST_CASE("view: Left shows the figure's left side and it faces the screen's left") {
    Camera c;
    c.set_preset(ViewPreset::Left);
    expect_vec(c.forward(), 1, 0, 0);
    expect_vec(c.right(), 0, -1, 0);
    CHECK(c.eye().x < c.target.x);   // the camera is on the figure's left (-x)

    float cx, cy, nx, ny, ex, ey;
    REQUIRE(project(c, kVp, c.target, cx, cy));
    REQUIRE(project(c, kVp, c.target + Vec3{0, 10, 0}, nx, ny));   // the nose
    CHECK(nx < cx);
    float near_depth, far_depth;
    REQUIRE(project(c, kVp, c.target + Vec3{-10, 0, 0}, ex, ey, &near_depth));   // the left side
    REQUIRE(project(c, kVp, c.target + Vec3{10, 0, 0}, ex, ey, &far_depth));     // the right side
    CHECK(near_depth < far_depth);
}

TEST_CASE("view: a preset keeps the target and the distance, Free changes nothing") {
    Camera c;
    c.target = {1, 2, 3};
    c.distance = 77;
    c.yaw_deg = 33;
    c.pitch_deg = 12;
    c.set_preset(ViewPreset::Free);
    CHECK(c.yaw_deg == 33);
    c.set_preset(ViewPreset::Left);
    CHECK(c.yaw_deg == -90);
    CHECK(c.pitch_deg == 0);
    CHECK(c.distance == 77);
    expect_vec(c.target, 1, 2, 3);
}

TEST_CASE("view: depth is 0 at the near plane and 1 at the far plane") {
    Camera c;
    c.set_preset(ViewPreset::Front);
    Mat4 m = projection_matrix(c, 1.6f) * view_matrix(c);
    for (float dist : {c.near_z, c.far_z}) {
        Vec3 p = c.eye() + c.forward() * dist;
        float w = m.m[3][0] * p.x + m.m[3][1] * p.y + m.m[3][2] * p.z + m.m[3][3];
        float z = m.m[2][0] * p.x + m.m[2][1] * p.y + m.m[2][2] * p.z + m.m[2][3];
        CHECK(z / w == doctest::Approx(dist == c.near_z ? 0.0f : 1.0f).epsilon(1e-4).scale(1));
    }
}

TEST_CASE("view: a ray through a pixel passes through the point that projects there") {
    Camera c;
    c.yaw_deg = 37;
    c.pitch_deg = 21;
    c.distance = 90;
    c.target = {3, -4, 25};
    Ray centre = ray_through_pixel(c, kVp, kCentreX, kCentreY);
    expect_vec(centre.origin, c.eye().x, c.eye().y, c.eye().z);
    expect_vec(centre.dir, c.forward().x, c.forward().y, c.forward().z);

    const Vec3 points[] = {{0, 0, 0}, {10, 5, 30}, {-12, 8, 2}, {4, -20, 40}};
    for (Vec3 p : points) {
        float px, py;
        REQUIRE(project(c, kVp, p, px, py));
        Ray r = ray_through_pixel(c, kVp, px, py);
        Vec3 to = p - r.origin;
        CHECK(dot(r.dir, to) > 0);
        CHECK(length(cross(r.dir, to)) < 1e-2f);   // the point is on the ray
    }
}

TEST_CASE("view: orbit, pan and zoom") {
    Camera c;
    c.set_preset(ViewPreset::Front);
    float dist = c.distance;
    c.orbit(100, 50);
    CHECK(c.yaw_deg == doctest::Approx(100 * kOrbitDegPerPixel));
    CHECK(c.pitch_deg == doctest::Approx(50 * kOrbitDegPerPixel));
    CHECK(c.distance == dist);
    CHECK(length(c.eye() - c.target) == doctest::Approx(dist));   // it turns about the target
    c.orbit(0, 100000);
    CHECK(c.pitch_deg == kMaxPitchDeg);
    c.orbit(0, -1000000);
    CHECK(c.pitch_deg == -kMaxPitchDeg);

    // A pan moves the target in the screen's plane, by the voxels one pixel covers at the target's depth, and the scene follows the
    // pointer: a drag to the right takes the target to the left.
    c.set_preset(ViewPreset::Front);
    c.pitch_deg = 0;
    Vec3 before = c.target;
    c.pan(10, 0, 500);
    float per_pixel = 2.0f * c.distance * std::tan(c.fov_y_deg * 0.5f * 3.14159265f / 180.0f) / 500.0f;
    Vec3 moved = c.target - before;
    CHECK(length(moved) == doctest::Approx(10 * per_pixel));
    CHECK(dot(moved, c.right()) < 0);
    CHECK(dot(moved, c.forward()) == doctest::Approx(0).scale(1));

    c.distance = 100;
    c.zoom(1);
    CHECK(c.distance == doctest::Approx(100 * kZoomPerStep));
    c.zoom(-1);
    CHECK(c.distance == doctest::Approx(100));
    c.zoom(1000);
    CHECK(c.distance == kMinDistance);
    c.zoom(-1000);
    CHECK(c.distance == kMaxDistance);
}

TEST_CASE("view: frame shows the whole ball, and a bounding sphere of points") {
    const Vec3 pts[] = {{-5, -3, 0}, {5, 3, 40}, {0, 0, 20}};
    Sphere s = bounding_sphere(pts, 3);
    expect_vec(s.centre, 0, 0, 20);
    CHECK(s.radius == doctest::Approx(length(Vec3{10, 6, 40}) * 0.5f));
    Camera c;
    c.set_preset(ViewPreset::Front);
    c.frame(s.centre, s.radius);
    expect_vec(c.target, 0, 0, 20);
    // Every corner of the points' box lands inside the viewport.
    const Viewport vp = {0, 0, 640, 480};
    for (float x : {-5.0f, 5.0f})
        for (float y : {-3.0f, 3.0f})
            for (float z : {0.0f, 40.0f}) {
                float px, py;
                REQUIRE(project(c, vp, {x, y, z}, px, py));
                CHECK(px > 0);
                CHECK(px < 640);
                CHECK(py > 0);
                CHECK(py < 480);
            }
    CHECK(bounding_sphere(nullptr, 0).radius == 1);
}

TEST_CASE("mesher: .cub parse errors") {
    CubModel m;
    std::string err;
    const std::vector<uint8_t> three = {200, 100, 50};
    CHECK_FALSE(parse_cub(nullptr, 0, m, err));
    CHECK(err.find("header") != std::string::npos);

    auto bad_len = cub_bytes(2, 1, 1, three);   // 2 voxels need 6 bytes
    CHECK_FALSE(parse_cub(bad_len.data(), bad_len.size(), m, err));
    CHECK(err.find("does not match") != std::string::npos);

    auto zero = cub_bytes(0, 1, 1, {});
    CHECK_FALSE(parse_cub(zero.data(), zero.size(), m, err));
    CHECK(err.find("0 or above 256") != std::string::npos);

    auto big = cub_bytes(257, 1, 1, std::vector<uint8_t>(3 * 257, 1));
    CHECK_FALSE(parse_cub(big.data(), big.size(), m, err));
    CHECK(err.find("0 or above 256") != std::string::npos);

    auto empty = cub_bytes(1, 1, 1, {0, 0, 0});   // (0,0,0) is an empty voxel
    CHECK_FALSE(parse_cub(empty.data(), empty.size(), m, err));
    CHECK(err.find("no voxel") != std::string::npos);

    auto ok = cub_bytes(1, 1, 1, three);
    CHECK(parse_cub(ok.data(), ok.size(), m, err));
}

TEST_CASE("mesher: one voxel is six faces, two next to each other are ten") {
    Mesh one = mesh_cub(model_with(1, 1, 1, {0}));
    CHECK(one.triangle_count() == 12);
    Vec3 lo, hi;
    REQUIRE(mesh_bounds(one, lo, hi));
    expect_vec(lo, -0.5f, -0.5f, -0.5f);   // centred on the node
    expect_vec(hi, 0.5f, 0.5f, 0.5f);

    Mesh two = mesh_cub(model_with(2, 1, 1, {0, 1}));
    CHECK(two.triangle_count() == 20);   // the two touching faces are not made
}

TEST_CASE("mesher: a 2x2x2 model") {
    Mesh full = mesh_cub(model_with(2, 2, 2, {0, 1, 2, 3, 4, 5, 6, 7}));
    CHECK(full.triangle_count() == 48);   // 6 sides x 4 voxel faces x 2 triangles; no inner face
    Vec3 lo, hi;
    REQUIRE(mesh_bounds(full, lo, hi));
    expect_vec(lo, -1, -1, -1);
    expect_vec(hi, 1, 1, 1);

    // One voxel missing: three outer faces go, and its three inner neighbours' faces appear. The count stays 24 quads.
    Mesh seven = mesh_cub(model_with(2, 2, 2, {0, 1, 2, 3, 4, 5, 6}));
    CHECK(seven.triangle_count() == 48);
    // The model's index order: x fastest, then y, then z. Voxel 6 is (0,1,1): a model with only it sits at its own place.
    Mesh only6 = mesh_cub(model_with(2, 2, 2, {6}));
    REQUIRE(mesh_bounds(only6, lo, hi));
    expect_vec(lo, -1, 0, 0);
    expect_vec(hi, 0, 1, 1);
}

TEST_CASE("mesher: every face's normal points out of the solid, and the top is the brightest") {
    Mesh m = mesh_cub(model_with(1, 1, 1, {0}));
    REQUIRE(m.triangle_count() == 12);
    int seen[3][2] = {};
    for (size_t t = 0; t < m.triangle_count(); ++t) {
        Vec3 n = normalized(tri_normal(m, t));
        // The centroid of an outward face is on the side the normal points to.
        const MeshVertex* v = &m.tris[3 * t];
        Vec3 centroid = {(v[0].x + v[1].x + v[2].x) / 3, (v[0].y + v[1].y + v[2].y) / 3, (v[0].z + v[1].z + v[2].z) / 3};
        CHECK(dot(n, centroid) > 0.1f);
        int axis = std::fabs(n.x) > 0.5f ? 0 : std::fabs(n.y) > 0.5f ? 1 : 2;
        float s = axis == 0 ? n.x : axis == 1 ? n.y : n.z;
        ++seen[axis][s > 0 ? 1 : 0];
        // The vertex colour is the voxel's (200, 100, 50) times the face's shade.
        float shade = face_shade(axis, s > 0);
        CHECK(((v[0].color >> 16) & 0xFF) == static_cast<uint32_t>(std::min(255.0f, 200 * shade + 0.5f)));
        CHECK((v[0].color >> 24) == 0xFF);
    }
    for (auto& a : seen)
        for (int n : a) CHECK(n == 2);   // two triangles per face
    CHECK(face_shade(2, true) > face_shade(2, false));
    for (int axis = 0; axis < 3; ++axis)
        for (bool pos : {false, true}) CHECK(face_shade(2, true) >= face_shade(axis, pos));
}

TEST_CASE("mesher: a box is six faces centred on the node") {
    Mesh b = mesh_box(10, 6, 14, 0x336699);
    CHECK(b.triangle_count() == 12);
    Vec3 lo, hi;
    REQUIRE(mesh_bounds(b, lo, hi));
    expect_vec(lo, -5, -3, -7);
    expect_vec(hi, 5, 3, 7);
    for (size_t t = 0; t < b.triangle_count(); ++t) {
        const MeshVertex* v = &b.tris[3 * t];
        Vec3 centroid = {(v[0].x + v[1].x + v[2].x) / 3, (v[0].y + v[1].y + v[2].y) / 3, (v[0].z + v[1].z + v[2].z) / 3};
        CHECK(dot(tri_normal(b, t), centroid) > 0);
    }
    CHECK(mesh_box(0, 1, 1, 0).empty());
    CHECK_FALSE(mesh_bounds(Mesh{}, lo, hi));
}

TEST_CASE("playback: play, pause, loop, scrub and the speed") {
    Playback p;
    p.play();
    CHECK_FALSE(p.playing);   // no clip: nothing to play
    p.set_length(1000);
    p.scrub(240);
    CHECK(p.time_ms == 240);
    p.scrub(5000);
    CHECK(p.time_ms == 1000);
    p.scrub(-5);
    CHECK(p.time_ms == 0);

    p.advance(100);
    CHECK(p.time_ms == 0);   // paused
    p.play();
    p.advance(100);
    CHECK(p.time_ms == doctest::Approx(100));
    p.set_speed(2);
    p.advance(100);
    CHECK(p.time_ms == doctest::Approx(300));
    p.set_speed(0.5f);
    p.advance(100);
    CHECK(p.time_ms == doctest::Approx(350));
    p.set_speed(50);
    CHECK(p.speed == kMaxSpeed);
    p.set_speed(0);
    CHECK(p.speed == kMinSpeed);

    p.set_speed(1);
    p.scrub(950);
    p.advance(100);   // loops
    CHECK(p.playing);
    CHECK(p.time_ms == doctest::Approx(50));

    p.loop = false;
    p.scrub(950);
    p.advance(100);   // stops at the end
    CHECK_FALSE(p.playing);
    CHECK(p.time_ms == 1000);
    p.play();   // from the end it starts again
    CHECK(p.playing);
    CHECK(p.time_ms == 0);

    p.pause();
    CHECK_FALSE(p.playing);
    p.set_length(0);
    CHECK(p.time_ms == 0);
}

TEST_CASE("view: the figure at a scrubbed 240 ms is drawn in the pose of the clip at 240 ms") {
    std::ifstream f(KYUBU_SOURCE_DIR "/tests/fixtures/kanim/design_example.anim.toml", std::ios::binary);
    REQUIRE(f.good());
    std::string clip_text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    anim::ClipLoad cl = anim::parse_clip(clip_text, "design_example.anim.toml");
    REQUIRE(cl.ok);
    std::ifstream rf(KYUBU_SOURCE_DIR "/tools/animator/rigs/standin.rig.toml", std::ios::binary);
    REQUIRE(rf.good());
    std::string rig_text((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    RigLoad rig = parse_rig(rig_text, "standin.rig.toml");
    REQUIRE(rig.ok);

    Playback pb;
    pb.set_length(anim::resolve_duration(cl.clip, 1000));
    REQUIRE(pb.length_ms == 1000);
    pb.scrub(240);
    Solved at240 = solve(rig.rig, anim::Pose{}, cl.clip, view_timing(pb));
    REQUIRE(at240.applied.ok);

    // The same clip evaluated by hand at 240 ms of a 1000 ms run.
    anim::Timing t;
    t.elapsed_ms = 240;
    t.duration_ms = 1000;
    t.run_ms = 1000;
    anim::Pose want;
    anim::apply_clip(cl.clip, t, want);
    const int hand = static_cast<int>(anim::Part::HandR);
    for (int i = 0; i < 3; ++i) CHECK(at240.pose.parts[hand].rot[i] == want.parts[hand].rot[i]);
    CHECK(at240.pose.parts[hand].rot[0] < -10);   // on its way to -110 at 350 ms

    pb.scrub(0);
    Solved at0 = solve(rig.rig, anim::Pose{}, cl.clip, view_timing(pb));
    int node = rig.rig.find("weapon_r");
    REQUIRE(node >= 0);
    CHECK(at0.world[node].t[1] != doctest::Approx(at240.world[node].t[1]));   // the weapon moved between the two times
}
