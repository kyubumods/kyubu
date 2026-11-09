// The Animator's posing by hand: see posing.h.
#include "posing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "animation_load.h"
#include "cliptext.h"

namespace kanimator {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kMinWeight = 0.05f;     // below this blend weight a key shows next to nothing: the edit is refused

// ---------------------------------------------------------------------------------------------------------------------------------
// Matrices

float det3(const float r[3][3]) {
    return r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
           r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
}

bool invert3(const float r[3][3], float out[3][3]) {
    float d = det3(r);
    if (!(std::fabs(d) > 1e-12f)) return false;
    float id = 1.0f / d;
    out[0][0] = (r[1][1] * r[2][2] - r[1][2] * r[2][1]) * id;
    out[0][1] = (r[0][2] * r[2][1] - r[0][1] * r[2][2]) * id;
    out[0][2] = (r[0][1] * r[1][2] - r[0][2] * r[1][1]) * id;
    out[1][0] = (r[1][2] * r[2][0] - r[1][0] * r[2][2]) * id;
    out[1][1] = (r[0][0] * r[2][2] - r[0][2] * r[2][0]) * id;
    out[1][2] = (r[0][2] * r[1][0] - r[0][0] * r[1][2]) * id;
    out[2][0] = (r[1][0] * r[2][1] - r[1][1] * r[2][0]) * id;
    out[2][1] = (r[0][1] * r[2][0] - r[0][0] * r[2][1]) * id;
    out[2][2] = (r[0][0] * r[1][1] - r[0][1] * r[1][0]) * id;
    return true;
}

Vec3 mul3(const float r[3][3], Vec3 v) {
    return {r[0][0] * v.x + r[0][1] * v.y + r[0][2] * v.z, r[1][0] * v.x + r[1][1] * v.y + r[1][2] * v.z,
            r[2][0] * v.x + r[2][1] * v.y + r[2][2] * v.z};
}

Vec3 unit_axis(int k) { return {k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f}; }

// Two unit vectors that span the plane with normal `n` (unit), the same two for the same `n` every time.
void plane_basis(Vec3 n, Vec3& u, Vec3& v) {
    Vec3 helper = std::fabs(n.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    u = normalized(cross(n, helper));
    v = cross(n, u);
}

float range_limit(anim::Channel c) { return c == anim::Channel::Pos ? anim::kMaxPos : anim::kMaxRotDeg; }

PoseEditResult fail(const std::string& text, const std::string& why) {
    PoseEditResult r;
    r.text = text;
    r.error = why;
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// Picking

int pick_node(const Rig& rig, const std::vector<Affine>& world, const std::vector<NodeBox>& boxes, const Ray& ray, float* distance) {
    int best = -1;
    float best_t = 0;
    for (size_t i = 0; i < rig.nodes.size() && i < world.size() && i < boxes.size(); ++i) {
        if (!boxes[i].valid) continue;
        float inv[3][3];
        if (!invert3(world[i].r, inv)) continue;
        // The ray in the node's own frame. The direction is not renormalised, so the parameter is the same along the world ray.
        const Vec3 rel = {ray.origin.x - world[i].t[0], ray.origin.y - world[i].t[1], ray.origin.z - world[i].t[2]};
        const Vec3 o = mul3(inv, rel);
        const Vec3 d = mul3(inv, ray.dir);
        const float lo[3] = {boxes[i].lo.x, boxes[i].lo.y, boxes[i].lo.z};
        const float hi[3] = {boxes[i].hi.x, boxes[i].hi.y, boxes[i].hi.z};
        const float oo[3] = {o.x, o.y, o.z};
        const float dd[3] = {d.x, d.y, d.z};
        float t0 = 0, t1 = 1e30f;
        bool miss = false;
        for (int a = 0; a < 3 && !miss; ++a) {
            if (std::fabs(dd[a]) < 1e-9f) {
                if (oo[a] < lo[a] || oo[a] > hi[a]) miss = true;
                continue;
            }
            float ta = (lo[a] - oo[a]) / dd[a], tb = (hi[a] - oo[a]) / dd[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) miss = true;
        }
        if (miss) continue;
        if (best < 0 || t0 < best_t) {
            best = static_cast<int>(i);
            best_t = t0;
        }
    }
    if (best >= 0 && distance) *distance = best_t;
    return best;
}

int part_node(const Rig& rig, int node) {
    for (int n = node; n >= 0 && n < static_cast<int>(rig.nodes.size()); n = rig.nodes[n].parent)
        if (rig.nodes[n].has_part) return n;
    return -1;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Handles

namespace {

bool reads_part(const Node& n, Part p) {
    for (const Step& st : n.steps)
        for (const Arg& a : st.args)
            if (a.is_ref && a.part == p) return true;
    return false;
}

}  // namespace

Handles make_handles(const Rig& rig, const Solved& solved, int selected) {
    Handles h;
    if (selected < 0 || selected >= static_cast<int>(rig.nodes.size()) || solved.world.size() != rig.nodes.size()) return h;
    const int pn = part_node(rig, selected);
    if (pn < 0) return h;
    const Part part = rig.nodes[pn].part;
    int pivot = -1;
    for (int n = pn; n >= 0; n = rig.nodes[n].parent)
        if (reads_part(rig.nodes[n], part)) {
            pivot = n;
            break;
        }
    if (pivot < 0) return h;
    h.edit_node = pn;
    h.pivot = pivot;
    h.part = part;
    const Node& nd = rig.nodes[pivot];
    const Affine parent = nd.parent >= 0 ? solved.world[nd.parent] : Affine{};
    h.origin = {solved.world[pivot].t[0], solved.world[pivot].t[1], solved.world[pivot].t[2]};

    for (size_t i = 0; i < nd.steps.size(); ++i) {
        const Step& st = nd.steps[i];
        const bool is_rot = st.kind == StepKind::RotX || st.kind == StepKind::RotY || st.kind == StepKind::RotZ;
        if (!is_rot && st.kind != StepKind::Translate) continue;
        const Affine pre = parent * steps_prefix(nd, solved.pose, i);   // the frame this step runs in
        const float handed = det3(pre.r) < 0 ? -1.0f : 1.0f;            // a mirrored frame turns the other way on the screen
        if (is_rot) {
            if (st.args.empty()) continue;
            const Arg& a = st.args[0];
            if (!a.is_ref || a.part != part || a.channel != anim::Channel::Rot || a.axis < 0 || a.axis > 2) continue;
            HandleAxis& ha = h.turn[a.axis];
            if (ha.present) continue;   // the first step that reads it gives the ring
            const int k = st.kind == StepKind::RotX ? 0 : st.kind == StepKind::RotY ? 1 : 2;
            ha.axis = normalized(mul3(pre.r, unit_axis(k)));
            ha.sign = (a.negate ? -1.0f : 1.0f) * handed;
            ha.present = length(ha.axis) > 0.5f;
        } else {
            int slot = 0;
            for (const Arg& a : st.args) {
                const int count = a.count();
                if (a.is_ref && a.part == part && a.channel == anim::Channel::Pos) {
                    for (int c = 0; c < count; ++c) {
                        const int comp = a.axis >= 0 ? a.axis : c;
                        const int s = slot + c;
                        if (comp < 0 || comp > 2 || s > 2) continue;
                        HandleAxis& ha = h.move[comp];
                        if (ha.present) continue;
                        Vec3 d = mul3(pre.r, unit_axis(s));
                        if (a.negate) d = d * -1.0f;
                        const float len = length(d);
                        if (!(len > 1e-6f)) continue;
                        ha.axis = d * (1.0f / len);
                        ha.per_unit = len;
                        ha.present = true;
                    }
                }
                slot += count;
            }
        }
    }
    h.ok = true;
    return h;
}

float handle_size(const Camera& cam) { return cam.distance * 0.10f; }

namespace {

constexpr float kArrowReach = 1.35f;

float seg_distance_px(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 1e-9f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
    t = std::min(1.0f, std::max(0.0f, t));
    const float cx = ax + dx * t - px, cy = ay + dy * t - py;
    return std::sqrt(cx * cx + cy * cy);
}

}  // namespace

std::vector<Vec3> ring_points(const Handles& h, int axis, float radius, int segments) {
    std::vector<Vec3> pts;
    if (axis < 0 || axis > 2 || !h.turn[axis].present) return pts;
    segments = std::max(segments, 8);
    Vec3 u, v;
    plane_basis(h.turn[axis].axis, u, v);
    for (int i = 0; i <= segments; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i % segments) / static_cast<float>(segments);
        pts.push_back(h.origin + (u * std::cos(a) + v * std::sin(a)) * radius);
    }
    return pts;
}

HandleHit pick_handle(const Camera& cam, const Viewport& vp, const Handles& h, float px, float py, float max_px) {
    HandleHit best;
    if (!h.ok) return best;
    const float radius = handle_size(cam);
    auto consider = [&](HandleKind kind, int axis, float d) {
        if (d > max_px) return;
        if (!best.hit || d < best.distance_px) {
            best.hit = true;
            best.kind = kind;
            best.axis = axis;
            best.distance_px = d;
        }
    };
    for (int a = 0; a < 3; ++a) {
        if (h.turn[a].present) {
            const std::vector<Vec3> pts = ring_points(h, a, radius);
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                float ax, ay, bx, by;
                if (!project(cam, vp, pts[i], ax, ay) || !project(cam, vp, pts[i + 1], bx, by)) continue;
                consider(HandleKind::Turn, a, seg_distance_px(px, py, ax, ay, bx, by));
            }
        }
        if (h.move[a].present) {
            float ax, ay, bx, by;
            const Vec3 tip = h.origin + h.move[a].axis * (radius * kArrowReach);
            if (project(cam, vp, h.origin, ax, ay) && project(cam, vp, tip, bx, by))
                consider(HandleKind::Move, a, seg_distance_px(px, py, ax, ay, bx, by));
        }
    }
    return best;
}

bool ring_angle_deg(const Ray& ray, Vec3 origin, Vec3 axis, float& degrees) {
    const float denom = dot(ray.dir, axis);
    if (std::fabs(denom) < 0.05f) return false;   // seen edge on: the point would fly off
    const float t = dot(origin - ray.origin, axis) / denom;
    if (!(t > 0)) return false;
    const Vec3 p = ray.origin + ray.dir * t - origin;
    Vec3 u, v;
    plane_basis(axis, u, v);
    degrees = std::atan2(dot(p, v), dot(p, u)) * 180.0f / kPi;
    return true;
}

bool line_param(const Ray& ray, Vec3 origin, Vec3 dir, float& s) {
    // Nearest points of the ray (o1 + a d1) and the line (o2 + s d2); d1 and d2 are unit.
    const Vec3 w = ray.origin - origin;
    const float b = dot(ray.dir, dir);
    const float d = dot(ray.dir, w);
    const float e = dot(dir, w);
    const float den = 1.0f - b * b;
    if (den < 1e-4f) return false;
    s = (e - b * d) / den;   // the parameter along `dir`
    return true;
}

bool HandleDrag::begin(const Handles& h, HandleKind kind, int axis, const Ray& at_press) {
    active_ = false;
    if (!h.ok || axis < 0 || axis > 2) return false;
    const HandleAxis& ha = kind == HandleKind::Turn ? h.turn[axis] : h.move[axis];
    if (!ha.present) return false;
    kind_ = kind;
    origin_ = h.origin;
    axis_ = ha.axis;
    float m = 0;
    if (kind == HandleKind::Turn) {
        if (!ring_angle_deg(at_press, origin_, axis_, m)) return false;
        scale_ = ha.sign;
    } else {
        if (!line_param(at_press, origin_, axis_, m)) return false;
        scale_ = ha.per_unit > 1e-9f ? 1.0f / ha.per_unit : 1.0f;
    }
    start_ = last_ = m;
    total_ = 0;
    delta_ = 0;
    active_ = true;
    return true;
}

bool HandleDrag::update(const Ray& ray) {
    if (!active_) return false;
    float m = 0;
    if (kind_ == HandleKind::Turn) {
        if (!ring_angle_deg(ray, origin_, axis_, m)) return false;
        float step = m - last_;
        while (step > 180.0f) step -= 360.0f;   // the angle wraps at 180: keep the turn continuous
        while (step < -180.0f) step += 360.0f;
        total_ += step;
        last_ = m;
        delta_ = total_ * scale_;
    } else {
        if (!line_param(ray, origin_, axis_, m)) return false;
        delta_ = (m - start_) * scale_;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The edit

namespace {

std::string first_message(const anim::ClipLoad& cl) {
    if (cl.errors.empty()) return "the clip is not valid";
    return anim::format_error(cl.errors.front());
}

}  // namespace

PoseEditResult edit_pose_key(const std::string& text, const PoseEdit& e) {
    if (e.axis < 0 || e.axis > 2) return fail(text, "the axis is not x, y or z");
    if (!std::isfinite(e.delta) || !std::isfinite(e.t_ms)) return fail(text, "the drag is not a number");
    if (!(e.duration_ms > 0)) return fail(text, "the clip has no length to put a key in");

    const anim::ClipLoad before = anim::parse_clip(text, "clip");
    if (!before.ok) return fail(text, "the clip has errors: " + first_message(before));
    const anim::Clip& clip = before.clip;
    const float u = std::min(1.0f, std::max(0.0f, e.t_ms / e.duration_ms));
    const float limit = range_limit(e.channel);
    // The pose shows base + weight * (track - base) there, so the track moves by delta / weight to show a figure that moves by delta.
    if (!(e.weight >= kMinWeight) || !(e.weight <= 1.0f))
        return fail(text, "the clip blends in or out at this time (weight " + fmt_value(e.weight) +
                              "), so a key here would show next to nothing: scrub to where the weight is 1");
    const float delta = e.delta / e.weight;

    // The track a drag edits is a keys track: a generator (osc, spring, expr) has no keys, so a part that has only one gets a new
    // `add` track of keys, which stacks on it.
    int ti = -1;
    for (size_t i = 0; i < clip.tracks.size(); ++i)
        if (clip.tracks[i].part == e.part && clip.tracks[i].channel == e.channel && clip.tracks[i].source == anim::Source::Keys)
            ti = static_cast<int>(i);

    PoseEditResult res;
    res.track = ti;
    std::string out, err;

    if (ti < 0) {
        // A new `add` track at the end of the file: the posed key and a zero key at each end of the clip, so the pose does not hold
        // for the whole clip (the user's decision, 2026-10-10). A posed key at an end is that end's key.
        anim::Key posed;
        posed.v[e.axis] = std::min(limit, std::max(-limit, delta));
        res.before = 0;
        res.after = posed.v[e.axis];
        const bool at_start = u * e.duration_ms < kKeyEpsilonMs;
        const bool at_end = (1.0f - u) * e.duration_ms < kKeyEpsilonMs;
        posed.t = at_start ? 0.0f : at_end ? 1.0f : u;
        std::vector<anim::Key> rows;
        if (!at_start) rows.emplace_back();
        res.key = static_cast<int>(rows.size());
        rows.push_back(posed);
        if (!at_end) rows.emplace_back().t = 1.0f;
        if (!append_track(text, e.part, e.channel, rows, out, err)) return fail(text, err);
        res.made_track = true;
        res.made_key = true;
        res.track = static_cast<int>(clip.tracks.size());
    } else {
        const anim::Track& tr = clip.tracks[ti];
        int ki = -1;
        for (size_t i = 0; i < tr.keys.size(); ++i)
            if (std::fabs(tr.keys[i].t - u) * e.duration_ms < kKeyEpsilonMs) {
                ki = static_cast<int>(i);
                break;
            }
        anim::Key nk;
        if (ki >= 0) std::memcpy(nk.v, tr.keys[ki].v, sizeof nk.v);
        else anim::sample_track(tr, u, nk.v);
        res.before = nk.v[e.axis];
        nk.v[e.axis] = std::min(limit, std::max(-limit, nk.v[e.axis] + delta));
        res.after = nk.v[e.axis];

        if (ki >= 0) {
            if (!set_key_value(text, ti, ki, nk.v, out, err)) return fail(text, err);
            res.key = ki;
        } else {
            // The place in the order: after the keys at or before u. The segment the key splits keeps its shape.
            size_t j = 0;
            while (j < tr.keys.size() && tr.keys[j].t <= u) ++j;
            nk.t = u;
            nk.ease = j < tr.keys.size() ? tr.keys[j].ease : anim::Ease::Linear;
            if (!insert_key(text, ti, nk, out, err, &res.key)) return fail(text, err);
            res.made_key = true;
        }
    }

    anim::ClipLoad after = anim::parse_clip(out, "clip");
    if (!after.ok) return fail(text, "the edit made an invalid clip (" + first_message(after) + "); nothing was changed");
    if (res.track < 0 || static_cast<size_t>(res.track) >= after.clip.tracks.size() ||
        res.key < 0 || static_cast<size_t>(res.key) >= after.clip.tracks[res.track].keys.size())
        return fail(text, "the edit did not land on a key; nothing was changed");
    res.ok = true;
    res.text = std::move(out);
    res.clip = std::move(after.clip);
    return res;
}

}  // namespace kanimator
