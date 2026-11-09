// The Animator's posing by hand (openspec add-ability-animation, task 6.4): pick a part with a ray, the turn and move handles of the
// part in the frame of the rig, the maths of a drag on a handle, and the one function that writes a key into the clip's text. Pure:
// no windows.h, no ImGui, no game. app.cpp only feeds it the pointer and draws what it returns.
//
// The frame of a handle comes from the rig, not from the program: a ring turns about the axis its rot step turns about, in the frame
// that step runs in (the parent's matrix times the steps written before it). So the x ring of a part whose steps are z, y, x is the
// innermost one and the z ring the outermost, as the rig reference words it (docs/animations.md, "Frame and order"). A value the rig
// never reads has no handle.
#pragma once
#include <string>
#include <vector>

#include "animation_eval.h"
#include "camera.h"
#include "rig.h"

namespace kanimator {

namespace anim = kyubu::game::anim;

// ---------------------------------------------------------------------------------------------------------------------------------
// Picking a part

// The box around a node's model in the node's own frame (cubmesh.h `mesh_bounds`). A node with no model has none.
struct NodeBox {
    bool valid = false;
    Vec3 lo, hi;
};

// The node whose box the ray crosses first, or -1. `world` is one matrix per node (Solved::world). `distance` gets how far along the
// ray (a unit direction) the box was entered.
int pick_node(const Rig& rig, const std::vector<Affine>& world, const std::vector<NodeBox>& boxes, const Ray& ray,
              float* distance = nullptr);

// The node a click on `node` edits: the node itself when it answers to a clip part (`Node::part`), else its nearest ancestor that
// does. -1 when none does (a model node of a rig that names no part above it).
int part_node(const Rig& rig, int node);

// ---------------------------------------------------------------------------------------------------------------------------------
// Handles

enum class HandleKind { Turn, Move };

struct HandleAxis {
    bool present = false;   // the rig reads this component of the part's vector
    Vec3 axis;              // unit, in the figure's frame: the ring's normal, or the arrow's direction
    float per_unit = 1;     // Move: the length one unit of the value moves the part (voxels, or more where the rig scales)
    float sign = 1;         // Turn: the value rises by `sign` degrees for a right-handed turn of one degree about `axis`
};

struct Handles {
    bool ok = false;        // false: nothing is selected, or no node of the rig reads the part's vectors
    int edit_node = -1;     // the node `part_node` found
    int pivot = -1;         // the node whose steps read the part (the edit node, or its nearest ancestor that does)
    anim::Part part = anim::Part::Root;
    Vec3 origin;            // where the handles sit: the pivot's origin
    HandleAxis turn[3];     // the rot channel, x y z
    HandleAxis move[3];     // the pos channel, x y z
};

// The handles of the part a click on `selected` edits, at the pose of `solved` (its pose is the one the steps read).
Handles make_handles(const Rig& rig, const Solved& solved, int selected);

// The radius of a ring in voxels: a fixed share of the camera's distance, so the handles keep their size on the screen.
float handle_size(const Camera& cam);

struct HandleHit {
    bool hit = false;
    HandleKind kind = HandleKind::Turn;
    int axis = 0;
    float distance_px = 0;
};
// The handle nearest to the pixel, when one is within `max_px` of it. Rings are circles of `handle_size` about their axis, arrows
// run from the origin 1.35 times as far along theirs.
HandleHit pick_handle(const Camera& cam, const Viewport& vp, const Handles& h, float px, float py, float max_px = 8.0f);

// The points of a ring in the figure's frame, closed (the first point is repeated at the end). `segments` is at least 8.
std::vector<Vec3> ring_points(const Handles& h, int axis, float radius, int segments = 48);

// A drag on one handle. begin() takes the ray at the press; update() takes each later ray and gives the change of the value since the
// press, in degrees for a ring and in the part's own unit for an arrow. A ray that cannot say (seen edge on, or parallel to the
// arrow) leaves the last value.
class HandleDrag {
public:
    bool begin(const Handles& h, HandleKind kind, int axis, const Ray& at_press);
    bool update(const Ray& ray);
    float delta() const { return delta_; }
    bool active() const { return active_; }
    void end() { active_ = false; }

private:
    bool active_ = false;
    HandleKind kind_ = HandleKind::Turn;
    Vec3 origin_, axis_;
    float scale_ = 1;       // Turn: sign; Move: 1 / per_unit
    float start_ = 0;       // the measure at the press
    float last_ = 0;        // the last measure (Turn: for the unwrap)
    float total_ = 0;       // Turn: the angle turned, unwrapped, in degrees
    float delta_ = 0;
};

// The angle in degrees of the point where the ray meets the plane through `origin` with normal `axis`, about `axis`, from a fixed
// direction in that plane. False when the ray is nearly parallel to the plane.
bool ring_angle_deg(const Ray& ray, Vec3 origin, Vec3 axis, float& degrees);
// The parameter s of the point on the line origin + s * dir that is nearest the ray. False when they are nearly parallel.
bool line_param(const Ray& ray, Vec3 origin, Vec3 dir, float& s);

// ---------------------------------------------------------------------------------------------------------------------------------
// The edit

// One drag, as the model sees it: `delta` is added to component `axis` of the part's `channel` at `t_ms` of a clip `duration_ms` long.
struct PoseEdit {
    anim::Part part = anim::Part::Root;
    anim::Channel channel = anim::Channel::Rot;
    int axis = 0;               // 0 x, 1 y, 2 z
    float delta = 0;            // degrees for rot, voxels for pos
    float t_ms = 0;
    float duration_ms = 0;
    float weight = 1;           // the clip's blend weight at t_ms (anim::blend_weight): the track moves by delta / weight
};

struct PoseEditResult {
    bool ok = false;
    std::string text;           // the clip text after the edit (unchanged text when !ok)
    anim::Clip clip;            // that text, parsed
    std::string error;          // one sentence, when !ok
    int track = -1;             // the track written, an index into the clip's tracks
    int key = -1;               // the key written, an index into that track
    bool made_track = false;
    bool made_key = false;
    float before = 0;           // the component's value in the track at t_ms before the edit (0 for a new track)
    float after = 0;            // and after (clamped to the validator's range)
};

// Writes the key. The track is the LAST track of the clip on that part and channel; a part with none gets a new `add` track at the
// end of the text, with the posed key and a zero key at 0 and at 1 (a key at an end is that end's key). A key within half a
// millisecond of `t_ms` is changed; otherwise a key is made at `t_ms` in its place in the order, with the track's value there (the
// other two components too) and the ease of the key after it. The component becomes the track's value there plus `delta / weight`:
// for an `add` track that is the offset from the base pose, for a `set` track the value itself, and the figure moves by `delta`
// whatever the blend. A weight below 0.05 is refused: a key there shows next to nothing.
// Only the text that must change changes: comments, layout and every other key stay. Every number is written to 3 decimals
// (`t` to 5). The result is parsed again by the loader's validator and refused when that fails.
PoseEditResult edit_pose_key(const std::string& clip_text, const PoseEdit& edit);

}  // namespace kanimator
