// The pose provider (openspec add-ability-animation, design D4): a function hook on draw_creature and a mid hook at its join site.
// Phase 1, the borrowed look. When the creature's mode is a mod ability with a bound clip that has a `[borrow]` table,
// the detour writes the borrowed POSE id and the retimed clock into the draw record (+8 and +4), adds the clip's hide groups to the mask,
// calls the original and puts the record back. The creature itself is never changed.
// Phase 2, the override. A clip with tracks is applied at the join site, where the record holds every smoothed pose vector:
// the tracks are written over the vectors for this one draw, and the detour puts the game's values back when the draw returns. The record
// is the game's eased state, so a value left in it would be eased from in the next frame: an `add` would pile up and a blend would drift.
// The decisions live in plan_borrow / plan_draw and the writes in apply_tracks / restore_pose (no hooks), so a unit test runs them.
#pragma once
#include "common.h"

#include <string>

#include "animation_eval.h"

typedef struct cube_CreatureData cube_CreatureData;   // cubeworld/types.h

namespace kyubu::game {

// The first current_mode the provider looks at: 111 is `kyubu:placeholder`, which has no clip, and a vanilla mode (0..110) pays one compare.
constexpr uint32_t kProviderFirstMode = 112;
// The largest pose id of the draw record (the pose switch gives ids 0 to 73). A `[borrow] mode` above it is not a pose.
constexpr uint32_t kMaxPoseId = 73;

// The game's own mask bits for the loader's hide groups (mapping atlas, creatures: 1 head, 2 hair, 8 body, 4 shoulders, 0x40 weapons).
// The loader's bits (anim::HideGroup) are different numbers: never OR them into the game's mask.
uint32_t game_hide_bits(uint32_t groups);

// What one draw becomes: the record's pose id (+8), its time in ms (+4), the mask bits to add to argument 9, and the clip whose tracks
// the join site writes (null: none). The clip belongs to a published set: the draw's Hold (draw_detour) keeps that set alive until the draw ends.
struct BorrowPlan {
    uint32_t pose_id = 0;
    uint32_t time_ms = 0;
    uint32_t hide_bits = 0;
    const anim::Clip* tracks = nullptr;
    anim::Timing timing;
};

// `elapsed_ms` is the mode time (record +4, or the preview clock). `ability_duration_ms` is the ability's, for `duration = "ability"` (0: unknown).
// False, with the plan untouched, for a clip with no `[borrow]` (plan_draw gives a tracks-only clip pose 0) and for a borrowed id that is not a pose id.
bool plan_borrow(const anim::Clip& clip, float elapsed_ms, float ability_duration_ms, BorrowPlan* out);

// The signals an `expr` track can read at a draw (design D11), from the creature's own data: `charge` is `charged_mp`
// (verified, probe ability-costs), `speed` the length of `velocity` on the ground plane (x and y; verified, probe creature-fields) in
// the game's own units (about 10 at a walk), and `rank` the local player's rank in the mod ability `mode` (112 to 255, the ranks
// file of add-ability-api). Another creature, a vanilla mode and the Animator's preview have a rank of 1. A value that is not finite
// reads 0, and a negative charge is 0.
anim::Signals creature_signals(const cube_CreatureData& data, bool is_local, uint32_t mode);

// What the hook knows at a draw.
struct DrawFacts {
    uint32_t mode = 0;               // the creature's current_mode
    uint32_t record_time_ms = 0;     // the record's +4
    bool is_local = false;           // the creature is the local player (only it takes the Animator's preview)
    uint64_t creature_id = 0;        // its game id, for a binding of its own (ability_animation_bind_for); 0: the ability's clip
};
// The clip of this draw: the preview clock for the local player when it is on, else the clip bound to the mod ability whose number is
// `mode` (112 to 255). False when nothing changes: the draw is then the game's own. One compare for a vanilla mode.
// A clip with tracks and no `[borrow]` gets pose 0 (the game's neutral pose) and the record's own time; a clip with both keeps the borrowed
// pose as the base of its tracks. While the join site is off (pose_tracks_on), tracks are not drawn: one ERROR line names the clip and the
// reason, a clip with `[borrow]` keeps its borrowed look, and a tracks-only clip gives the game's own draw.
bool plan_draw(const DrawFacts& facts, BorrowPlan* out);

// Phase 2. The draw record as the join site sees it: each part's position and euler are three floats at a fixed
// offset. `hand_r` is the game's hand B and `hand_l` its hand A. The root's position has x and y only, and the tail has no position:
// a track on a channel with no vector writes nothing.
constexpr size_t kDrawRecordSize = 0x890;
// The game's vectors, as they were before apply_tracks wrote over them.
struct PoseSave {
    uint32_t pos_parts = 0;   // bit (1 << Part) of each position written
    uint32_t rot_parts = 0;   // the same for the eulers
    anim::Pose base;
};
// The record's vectors as a pose (a channel with no vector reads 0).
void read_pose(const void* record, anim::Pose* out);
// Evaluates the clip over the record's vectors (`add`, `set`, the blend weight) and writes the parts and channels its tracks touch;
// everything else keeps the game's value. `save` gets what was overwritten. False, with the record untouched, when nothing applies.
// A track on a part the record has no model for (its model slot is 0) is skipped, and `lacking` gets those parts (bit 1 << Part); a
// part in `hidden` (the game's hide bits, part bits) is not counted as lacking. `lacking` is set whether or not anything is written.
bool apply_tracks(void* record, const anim::Clip& clip, const anim::Timing& timing, PoseSave* save, uint32_t* lacking = nullptr,
                  uint32_t hidden = 0);
// True the first time a mode (or 256, the preview) and a part are given here, false after: the "lacks a part" log line is written once.
bool first_lacking_part(uint32_t key, anim::Part part);
// Puts back what apply_tracks overwrote.
void restore_pose(void* record, const PoseSave& save);

// True while the join site's mid hook is installed. False with the reason in `*why` (never installed, or the hook was refused).
bool pose_tracks_on(std::string* why = nullptr);
// Tests: 1 the site is on, 0 it is off for `why`, -1 back to the real state.
void force_pose_tracks_for_test(int state, const char* why = nullptr);
// Tests: every "logged once" flag back to unset, so a test sees the line its own draw writes.
void forget_logs_for_test();

// Installs the detour and the join site's mid hook when `animation_pose` is available; otherwise one INFO line says the provider is off and
// why. A refused mid hook leaves the detour in place (borrowed looks) and turns tracks off with one ERROR line. Called by game::boot.
void install_animation_pose_hook();
void remove_animation_pose_hook();
bool animation_pose_hook_installed();

}  // namespace kyubu::game
