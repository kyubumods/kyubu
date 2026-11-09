// The Animator's parity check, the mimic gate of design D12 (openspec add-ability-animation, task 7.5): the capture's pose vectors are
// solved with a rig, and each node's frame is compared with the part matrix the game left (docs/dev-tools.md, "Capture files"). The
// report gives, for each node, the largest position error in voxels and the largest angle error in degrees, and a node outside the
// tolerance is marked `approximate` in the rig file. Pure: no windows.h, no ImGui, no game. The program has none of the game's numbers:
// how a capture's matrices sit in the rig's frame (unit, yaw, origin) and which matrix a node answers to (`slot`) are in the rig file
// (`[capture]`, rig.h), which for the game's rig is private.
//
// What is compared, per frame and per node with a `slot` (the rig's root is the reference and is not compared):
//   - the node's centre, relative to the root's origin, in voxels. The rig's centre is its world matrix's translation. The capture's
//     is its matrix's translation, less the root's, divided by `unit`, with the frame's `body_yaw` turned back (the game draws the
//     figure turned by it; the rig's figure faces +y), and, when the game puts a model at its corner, moved by half the model's size
//     along the model's own axes. The error is the distance between the two, in voxels.
//   - the node's orientation, as the angle of the rotation that takes one to the other, in degrees. Both are orthonormalised first (the
//     matrix of a model carries its scale), keeping the handedness: a mirrored model in one and not the other is "mirror".
// Matrices the game did not fill (all zero: the creature has no model there) are not compared in that frame.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "rig.h"

namespace kanimator {

constexpr size_t kMaxCaptureBytes = 64u << 20;
constexpr size_t kMaxCaptureFrames = 20000;       // per file

// ---- a capture file

struct CaptureFrame {
    int index = 0;
    double t_ms = 0;
    std::string view;
    double body_yaw = 0;                          // degrees: the way the figure was turned for this frame (0 when the file has none)
    Pose pose;                                    // the vectors the game used at the join site
    float matrix[kCaptureSlots][16] = {};         // Direct3D row-vector form: three rows of axes, then the translation
    bool has_matrix[kCaptureSlots] = {};          // false when the sixteen numbers are all zero or one of them is not finite
};

struct Capture {
    std::string source;                           // what the report calls it: the folder's name, or the file's
    std::string kind;                             // `capture`, `rig`, `bake` or `bake-replay`
    std::string clip, build;
    std::vector<CaptureFrame> frames;
};

struct CaptureLoad {
    bool ok = false;
    Capture capture;
    std::string error;                            // one sentence naming the file and the field
};

// Parses the text of a `pose.json` of format `kyubu-capture/1`, of any kind. Every frame needs `vectors` (the six parts, as the pose
// library reads them) and `matrices` (17 arrays of 16 numbers). A matrix that is all zero, or has a `null` (a number that was not
// finite: the part is hidden), is read as absent (`has_matrix` false). Never throws.
CaptureLoad parse_capture(const std::string& json_text, const std::string& source);

// ---- the compare

struct Tolerance {
    double pos = 0.25;                            // voxels (accepted in task 1.3)
    double angle = 1.0;                           // degrees
};

struct NodeParity {
    int node = -1;                                // an index into Rig::nodes
    int slot = -1;
    int frames = 0;                               // frames in which the node was compared
    double max_pos = 0, max_angle = 0;
    std::string pos_at, angle_at;                 // "<capture> frame <index>" of the worst frame
    int mirrored = 0;                             // frames in which the handedness of the two differs
    // The verdict: false for a node that was never compared (it stays as it is).
    bool outside(const Tolerance& t) const { return frames > 0 && (max_pos > t.pos || max_angle > t.angle || mirrored > 0); }
};

struct ParityReport {
    bool ok = false;
    std::string error;                            // when !ok: one sentence
    std::vector<NodeParity> nodes;                // the rig's nodes that have a slot, but the root, in file order
    int captures = 0;
    int frames = 0;                               // frames over all captures
    int skipped = 0;                              // frames with no matrix for the root: nothing to compare against
    std::vector<std::string> notes;
};

// Compares every frame of every capture. `sizes` has one entry per rig node: the model's x, y and z size in voxels (0 for no model).
// A rig with no `[capture]` table, or whose root has no `slot`, is refused.
ParityReport run_parity(const Rig& rig, const std::vector<std::array<float, 3>>& sizes, const std::vector<Capture>& captures);

// The table the command prints: a line per node with the errors and a verdict, then the nodes outside the tolerance with the frame
// of their worst error.
std::string parity_table(const Rig& rig, const ParityReport& report, const Tolerance& tol);

// Sets `approximate = true` on each named node, in the text of a rig file, and leaves every other byte alone (comments, order, line
// endings). A node that has no `approximate` line gets one after its `name` line. It never clears a mark: a mark that a person wrote
// stays until a person removes it. False with `err` when a name is not a node of the text.
bool mark_approximate(const std::string& rig_text, const std::vector<std::string>& nodes, std::string& out, std::string& err);

// ---- the command

struct ParityCommand {
    std::vector<std::string> capture_paths;       // a `pose.json`, a capture folder, or a folder of capture folders
    std::string rig_path;
    std::string models_dir;                       // for the size of a named model; empty: none (a note says so)
    Tolerance tol;
    bool mark = false;                            // write `approximate = true` into the rig file for each node outside the tolerance
};

// Runs the command: the table on `out`, notes and errors on `err`. 0 = every node is within the tolerance, 3 = the report is written
// and a node is outside it, 1 = it could not be run (the reason on `err`). Never opens a window.
int run_parity_command(const ParityCommand& cmd, std::FILE* out, std::FILE* err);

}  // namespace kanimator
