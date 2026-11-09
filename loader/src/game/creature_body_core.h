// Creature bodies, the pure part (openspec add-dragon-boss, capability creature-body).
//
// A body definition (rig + clips + one model slot per piece) and a body instance (the playing clip, the cross-fade, the joint offsets,
// the markers passed) with the pose evaluation. No game, no file access, no log: the same rig and clip code as the Animator
// (tools/animator/rig.cpp and the loader's animation_* sources), so a clip at a time gives the Animator's matrices. creature_body.cpp
// holds the game side (API, draw hooks, death burst).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "rig.h"

namespace kyubu::game::body {

namespace anim = kyubu::game::anim;

constexpr int kMaxClips = 64;
constexpr int kMaxPieces = 64;   // the Animator's rig bound (kanimator::kMaxNodes)
constexpr int kMaxGroups = 8;    // offset groups, each with a weight

struct PieceSpec {
    std::string node;          // the rig node that carries the model
    uint32_t slot = 0;         // the model slot the piece is drawn with
    int size[3] = {0, 0, 0};   // the model's declared size in voxels (the game puts a matrix at the model's corner)
};
struct ClipSpec {
    std::string name;   // 1 to 64 of a-z, 0-9, '_', '-', '.'
    std::string text;   // the text of an .anim.toml
};

struct BodyDef {
    struct ClipDef {
        std::string name;
        anim::Clip clip;
        float duration_ms = 0;
    };
    struct Piece {
        int node = -1;   // an index into rig.nodes
        std::string name;
        uint32_t slot = 0;
        int size[3] = {0, 0, 0};
    };
    std::string name;
    kanimator::Rig rig;
    std::vector<ClipDef> clips;
    std::vector<Piece> pieces;   // in rig order
    int clip_index(const std::string& n) const;
    int piece_index(const std::string& n) const;
};

struct DefineResult {
    std::shared_ptr<const BodyDef> def;   // null when refused
    std::string error;                    // why, with the piece, clip, file line named
};

// Builds a definition. Refused when: the rig fails the Animator's check; a rig node with a model has no PieceSpec (the error names the
// node); a PieceSpec names no node of the rig or one without a model; a clip fails the clip validator, has a duration of 0 or less, or
// names a pose value (a part) that no node of the rig reads; a clip name is bad or repeated.
DefineResult define(const std::string& name, const std::string& rig_text, const std::vector<ClipSpec>& clips, const std::vector<PieceSpec>& pieces);

// A marker of the playing clip that the time passed since the last advance.
struct MarkerHit {
    int clip = -1;
    std::string name;
};

struct Instance {
    std::shared_ptr<const BodyDef> def;
    float scale = 1.0f;   // blocks per voxel

    // Plays a clip: from `start_ms`, looping or held at its end, with a cross-fade of `fade_ms` from what plays now (0: at once).
    // False when the clip index is out of range.
    bool play(int clip, float start_ms, bool loop, float fade_ms);
    // Moves the time on by dt_ms; markers the playing clip passed are added to `markers`.
    void advance(float dt_ms, std::vector<MarkerHit>* markers);
    // Sets (replaces) the offset of one pose value (part, channel; the three axes), added to the clip's, with weight group `group`.
    void set_offset(anim::Part part, anim::Channel ch, const float v[3], int group);
    void clear_offsets();
    void set_group_weight(int group, float weight);   // 0..1; every group starts at 1

    int playing() const { return cur_; }
    float time_ms() const { return cur_t_; }
    bool looping() const { return cur_loop_; }
    float fade_left_ms() const { return fade_left_; }

    // The pose now: the clip at its time, cross-faded, plus the weighted offsets.
    anim::Pose pose() const;
    // The pose of clip `clip` alone at `t_ms` (no fade, no offsets), without moving the instance: for posed points on the server.
    anim::Pose pose_of(int clip, float t_ms, bool loop) const;
    kanimator::Solved solve() const { return kanimator::solve_pose(def->rig, pose()); }

private:
    friend struct InstanceAccess;
    anim::Pose sample(int clip, float t_ms, bool loop) const;
    int cur_ = -1, prev_ = -1;
    float cur_t_ = 0, prev_t_ = 0;
    bool cur_loop_ = false, prev_loop_ = false;
    float fade_ms_ = 0, fade_left_ = 0;
    float off_[anim::kPartCount][2][3] = {};
    bool off_set_[anim::kPartCount][2] = {};
    int8_t off_group_[anim::kPartCount][2] = {};
    float group_w_[kMaxGroups] = {1, 1, 1, 1, 1, 1, 1, 1};
};

// The world matrix (4x4, column-major, translation at 12..14, blocks) of piece `piece` for a body at `pos` (blocks) turned by `yaw` radians
// about z: pos + Rz(yaw) * scale * (node matrix * (point - size/2)). With a point of (size/2) it is the node's own centre.
void piece_matrix(const BodyDef& def, const kanimator::Solved& solved, int piece, const float pos[3], float yaw, float scale, float out[16]);
// The world point (blocks) of the voxel-grid point `p` (voxels, 0..size) of a piece.
void piece_point(const BodyDef& def, const kanimator::Solved& solved, int piece, const float p[3], const float pos[3], float yaw, float scale, float out[3]);

}  // namespace kyubu::game::body
