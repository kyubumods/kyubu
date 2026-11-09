// The Animator's rig: a tree of nodes, a file format for it, and a solver (openspec add-ability-animation, design D12, task 6.2).
// Text in, a `Rig` or a list of errors out; then a rig, a pose (or a clip and a time) in, a matrix per node out. Pure: no windows.h,
// no ImGui, no game. The program's code holds no number derived from the game; the numbers are in a `*.rig.toml` file. The file
// that ships in the repository is the stand-in rig (tools/animator/rigs/standin.rig.toml), whose numbers are made up.
//
// A rig file (the keys, all named; any other key is an error):
//
//   name = "stand-in rig"          # required: what the view says
//   [[node]]
//   name   = "head"                # required: 1 to 48 of a-z, 0-9, '_'; unique
//   parent = "body"                # every node but one (the root) has one
//   part   = "head"                # optional: the clip part a click on this node edits (`weapon_r` and `weapon_l` are accepted)
//   model  = "box 8 8 8"           # optional: `box W D H` (x, y, z in voxels, centred on the node) or the name of a .cub model
//   approximate = false            # optional: the parity check (7.5) sets it for a part outside the tolerance
//   slot   = 3                     # optional: the capture matrix (0 to 16) this node is compared with by the parity check (7.5)
//   steps  = [["translate", 0, 0, 11], ["rot_x", "head.rot.x"], ["rot_y", "head.rot.y"], ["rot_z", "head.rot.z"],
//             ["translate", "head.pos"]]
//
// Steps. The node's own matrix is the product of its steps in the order written, so the FIRST step is the outermost and the LAST acts
// on a point first (the rule of docs/animations.md, "Frame and order"). The node's matrix in the world is its parent's times its own.
//   translate  three numbers (x y z in voxels)
//   rot_x, rot_y, rot_z   one number, degrees; right-handed, x right, y forward, z up
//   scale      one number (all axes) or three
// The parity check reads one more table, optional, that says how a capture's matrices sit in the rig's frame (task 7.5):
//
//   [capture]
//   unit   = 0.086                 # the capture's length unit per voxel (world units of the game: its matrices are scaled by it)
//   yaw    = "ccw"                 # which way a frame's `body_yaw` turns the figure seen from above: "ccw" (the default) or "cw"
//   origin = "corner"              # where a capture matrix puts a model: "corner" (its lowest corner) or "centre" (the default)
//
// An argument is a number or the name of a value of the pose: `<part>.pos` (three numbers), `<part>.pos.x` (one), `<part>.rot` (three),
// `<part>.rot.x` (one), `<part>` being a clip part name. A leading '-' negates it (`-head.rot.x`). A name that is three numbers
// stands for all three, so `["translate", "head.pos"]` and `["translate", "head.pos.x", "head.pos.y", "head.pos.z"]` are the same.
//
// Particles (optional): emitters that ride on nodes and throw small cubes, as the game's own particles are (particles.h moves and
// draws them). The numbers of how the game moves its particles are the rig's, in a `[particles]` table that a rig with an emitter
// must have; an emitter's lengths and speeds are in blocks, as the game's particle record's are:
//
//   [particles]
//   block   = 11.63                # voxels of the rig in one block of the game
//   gravity = 30                   # blocks a second squared, down, for an emitter with `gravity = true`
//
//   [[emitter]]
//   name     = "breath"            # required: as a node's name; unique among the emitters
//   parent   = "head"              # required: the node it rides on
//   steps    = [["translate", 0, 18, -4], ["rot_x", -18]]   # its frame in the node's, as a node's steps: particles leave along +y
//   amount   = "tail.pos.x"        # the share of `rate` thrown: a number or one value of the pose (default 1); below 0 throws none
//   rate     = 240                 # required: particles a second at amount 1, 0 to 5000
//   step_ms  = 16                  # a burst every step_ms (default 16), all its particles from one point, as a game record is
//   speed    = [30, 40]            # blocks a second along +y: one number or [least, most] (default 0)
//   cone     = 8                   # degrees, 0 to 180: each particle's way is turned up to this from +y (default 0)
//   spreading = 1.5                # blocks a second, 0 to 64: each axis of the speed gets a random -spreading to +spreading
//   life     = [500, 700]          # required: milliseconds a particle lives, one number or [least, most], 1 to 60000
//   size     = 0.5                 # blocks, the side of the cube, above 0 to 16 (default 0.25)
//   size_end = 0.1                 # blocks, the side at the end of its life (default `size`)
//   color    = [1, 0.4, 0]         # 0 to 1, RGB (a fourth number, alpha, is read and not drawn: the game draws these opaque)
//   color_random = [1, 1, 0]       # optional: each particle takes a random mix of `color` and this
//   color_end = [0.4, 0.1, 0.05]   # optional: the colour it turns to over its life
//   gravity  = true                # falls by `[particles] gravity` (default true)
//   ground   = "pass"              # "pass" (through the ground, z 0) or "stop" (it lands there and stays) (default "pass")
//   spin     = 50                  # degrees a second at most, a random turn about each axis (default 0)
//   seed     = 1                   # the random numbers' seed, 0 to 2^31 (default 1)
//   at       = "strike"            # optional: throws only for `for_ms` from each of the clip's markers of this name
//   for_ms   = 100                 # with `at`: how long it throws from each marker, 1 to 60000 (default 100)
//   cone_min = 30                  # degrees, 0 to `cone`: each way is at least this far from +y (default 0)
//   offset   = [0.5, 3]            # blocks: each particle starts this far out across the frame's x-z plane, the way its own way
//                                  # leans (straight ways: a random side), so they rise from a ring round +y (default 0)
//
// Effects (optional, effects.h; lengths in blocks, so a rig with one needs the `[particles]` table too), each started by the clip's
// markers of one name and placed by its frame at that marker, as a mod spawns them in an ability's step:
//
//   [[shockwave]]                  # EMTweaks' shockwave (docs/emtweaks.md): a see-through ring of square cells, drawn by the mod's code
//   name   = "slam"                # required: as a node's name; unique among the shockwaves
//   parent = "ground"              # required: the node it rides on
//   steps  = [["rot_x", 90]]       # its frame: the ring lies in the frame's x-z plane, facing +y
//   at     = "strike"              # required: a ring starts at each marker of this name
//   inner_radius = 1               # EMTweaks' fields, with its names and units (a tick is 20 ms): blocks, at birth
//   thickness    = 0.6             # blocks (at least 0.01)
//   expansion    = 0.4             # blocks a tick, both radii; negative shrinks it
//   life_ticks   = 30              # 1 to 6000
//   fade_on_tick = 10              # the fade starts on this tick
//   fade_speed   = 0.05            # part of the alpha a tick; 0 never fades
//   color        = [1, 0.6, 0.1, 0.9]   # RGBA, 0 to 1: the alpha is the opacity at birth
//   pixel_size   = 0               # blocks: the cells' side; 0 is the mod's default (0.25); negative draws a smooth ring
//
//   [[pillars]]                    # rock pillars: stacks of chipped voxel stones that slide up out of the ground, stay, and sink back
//   name   = "spikes"              # required: as a node's name; unique among the pillars
//   parent = "ground"              # required
//   steps  = [["rot_x", 90]]       # its frame: the pillars stand on the frame's x-z plane and rise along +y
//   at     = "strike"              # required: a set of pillars starts at each marker of this name
//   count  = 20                    # required: pillars in a set, 1 to 256
//   radius = [3, 12]               # required: blocks, where their feet fall (evenly over the ring between)
//   clear  = [-3, -14, 3, 2]       # optional: blocks, x and y in the figure's frame (least x, least y, most x, most y): no foot falls
//                                  # in this box (the body standing there); a foot drawn in it is drawn again
//   wave   = 25                    # blocks a second: a pillar r out starts r / wave after the marker (default 0: all at once)
//   delay  = [0, 80]               # ms, a random wait more (default 0)
//   width  = [0.8, 1.6]            # blocks, the side of its foot (default 1)
//   height = [1.2, 3]              # blocks above the ground when it is all out (default 2)
//   stones = [2, 4]                # 1 to 8: the stones stacked in each, the lowest widest (default [2, 4])
//   rubble = [1, 4]                # 0 to 8: loose stones lying round its foot, not leaned (default [0, 3])
//   taper  = 0.6                   # 0 to 1: the top stone's side over the lowest's (default 1)
//   cell   = 0.15                  # blocks, 0.02 to 16: its voxels' side; bigger when a pillar would be over 40 across or 80 up
//                                  # (default 0.25)
//   lean   = 20                    # degrees, 0 to 60: each leans out from the centre by a random amount up to this (default 0)
//   turn   = 45                    # degrees, 0 to 180: each is turned about its axis by a random -turn to +turn (default 0)
//   rise_ms = 120                  # its time to come out, fast then slowing (default 120)
//   hold_ms = [700, 1200]          # out for this long (default 800)
//   sink_ms = 400                  # its time to go back in (default 400)
//   color  = [0.7, 0.4, 0.28]      # its stones; color_random as an emitter's (a pillar's own mix); color_top: the rim round each
//                                  # stone's top (default: its colour lightened). Each stone, voxel, seam and crack is shaded off it.
//   seed   = 1
//
// Sounds (optional, sounds.h): a `.wav` the window plays when its playback passes a marker, as a mod plays one from an ability's
// step. They have no place and no frame (a mod's own sound is heard as the player's, at one level), so they need no node and no
// `[particles]` table:
//
//   [[sound]]
//   file     = "hit.wav"           # required: a .wav file's name, looked up in the clip's mod's assets/sounds/, then the Sounds
//                                  # panel's folder (1 to 64 of letters, digits, '_', '-', '.'; it ends in .wav)
//   at       = "strike"            # required: it plays at each marker of this name
//   volume   = 1                   # 0 to 4 (default 1): 1 is the game's level at a sound option of 100, as `sound.play`'s volume
//   pitch    = 1                   # 0.25 to 4 (default 1): the speed it is played at, so 2 is an octave up and half as long
//   delay_ms = 0                   # 0 to 60000 (default 0): it plays this long after the marker
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"

namespace kanimator {

using kyubu::game::anim::Applied;
using kyubu::game::anim::Clip;
using kyubu::game::anim::ClipError;
using kyubu::game::anim::Part;
using kyubu::game::anim::Pose;
using kyubu::game::anim::Timing;

// Bounds of a valid rig. A rig over any of them is refused.
constexpr int kMaxNodes = 64;
constexpr int kMaxSteps = 16;               // per node
constexpr size_t kMaxNodeName = 48;
constexpr size_t kMaxRigBytes = 1u << 20;
constexpr float kMaxConstant = 1000.0f;     // a constant argument, any axis (voxels, degrees or a scale)
constexpr int kCaptureSlots = 17;           // the part matrices of a capture (docs/dev-tools.md, "Capture files"); a node's `slot` is 0 to 16

enum class StepKind : uint8_t { Translate, RotX, RotY, RotZ, Scale };
const char* step_name(StepKind k);          // "translate", "rot_x", ...

// One argument of a step: a number, or `axis` (0 to 2) of `channel` (pos or rot) of a part's pose vector. `axis` -1 is all three.
struct Arg {
    bool is_ref = false;
    float value = 0;                        // the number, when !is_ref
    Part part = Part::Root;
    kyubu::game::anim::Channel channel = kyubu::game::anim::Channel::Pos;
    int axis = -1;
    bool negate = false;
    int count() const { return is_ref && axis < 0 ? 3 : 1; }   // the numbers it stands for
};

struct Step {
    StepKind kind = StepKind::Translate;
    std::vector<Arg> args;
    int line = 0;
};

struct Model {
    enum class Kind : uint8_t { None, Box, Named } kind = Kind::None;
    float box[3] = {0, 0, 0};               // Box: x, y, z size in voxels
    std::string name;                       // Named: the .cub model's name
};

struct Node {
    std::string name;
    int parent = -1;                        // an index into Rig::nodes; -1 for the root
    bool has_part = false;
    Part part = Part::Root;
    Model model;
    bool approximate = false;
    int slot = -1;                          // the capture matrix the parity check compares this node with; -1 for none
    std::vector<Step> steps;
    int line = 0;
};

constexpr int kMaxEmitters = 16;
constexpr float kMaxEmitRate = 5000;        // particles a second
constexpr float kMaxParticleLife = 60000;   // ms

// How the game moves its particles (the `[particles]` table): needed by a rig with an emitter.
struct ParticleWorld {
    bool present = false;
    float block = 1;                        // voxels in one block
    float gravity = 0;                      // blocks a second squared, down
};

// A number or a range of them: one number in the file gives lo == hi.
struct Range {
    float lo = 0, hi = 0;
};

// An `[[emitter]]`: see the top of this file.
struct Emitter {
    std::string name;
    int parent = -1;                        // an index into Rig::nodes
    std::vector<Step> steps;
    Arg amount;                             // a number (1 by default) or one value of the pose
    float rate = 0;
    float step_ms = 16;
    Range speed;                            // blocks a second
    float cone = 0;                         // degrees
    float spreading = 0;                    // blocks a second
    Range life{1000, 1000};                 // ms
    float size = 0.25f, size_end = 0.25f;   // blocks
    float color[3] = {1, 1, 1};
    bool has_color_random = false;
    float color_random[3] = {1, 1, 1};
    bool has_color_end = false;
    float color_end[3] = {1, 1, 1};
    bool gravity = true;
    bool ground_stop = false;
    float spin = 0;                         // degrees a second
    uint32_t seed = 1;
    std::string at;                         // a marker's name: throws only for `for_ms` from each such marker; empty: whenever amount allows
    float for_ms = 100;
    float cone_min = 0;                     // degrees
    Range offset;                           // blocks
    int line = 0;
};

constexpr int kMaxShockwaves = 16;
constexpr int kMaxPillarSets = 16;
constexpr int kMaxPillars = 256;            // in one set

// A `[[shockwave]]`: see the top of this file. The fields after `at` are EMTweaks' (api/emtweaks.h, EmShockwaveDef).
struct Shockwave {
    std::string name;
    int parent = -1;
    std::vector<Step> steps;
    std::string at;
    float inner_radius = 0, thickness = 0.5f, expansion = 0;   // blocks, blocks, blocks a tick
    uint32_t life_ticks = 40, fade_on_tick = 0;
    float fade_speed = 0;
    float color[4] = {1, 1, 1, 1};
    float pixel_size = 0;                   // blocks; 0: the mod's default; below 0: smooth
    int line = 0;
};

// A `[[pillars]]`: see the top of this file.
struct Pillars {
    std::string name;
    int parent = -1;
    std::vector<Step> steps;
    std::string at;
    int count = 0;
    Range radius;                           // blocks
    bool has_clear = false;
    float clear[4] = {0, 0, 0, 0};          // blocks, the figure's frame: least x, least y, most x, most y
    float wave = 0;                         // blocks a second; 0: all at once
    Range delay;                            // ms
    Range width{1, 1}, height{2, 2};        // blocks
    Range stones{2, 4}, rubble{0, 3};       // counts, rounded
    float taper = 1;
    float cell = 0.25f;                     // blocks
    float lean = 0, turn = 0;               // degrees
    float rise_ms = 120;
    Range hold_ms{800, 800};
    float sink_ms = 400;
    float color[3] = {0.45f, 0.36f, 0.26f};
    bool has_color_random = false;
    float color_random[3] = {1, 1, 1};
    bool has_color_top = false;
    float color_top[3] = {1, 1, 1};
    uint32_t seed = 1;
    int line = 0;
};

constexpr int kMaxSounds = 32;
constexpr size_t kMaxSoundFile = 64;        // bytes of a `file`
constexpr float kMaxSoundVolume = 4;
constexpr float kMinSoundPitch = 0.25f, kMaxSoundPitch = 4;

// A `[[sound]]`: see the top of this file.
struct RigSound {
    std::string file;                       // a .wav file's name
    std::string at;                         // the markers that play it
    float volume = 1;
    float pitch = 1;
    float delay_ms = 0;
    int line = 0;
};

// A sound file's name as `[[sound]] file` takes it: 1 to kMaxSoundFile of letters, digits, '_', '-', '.', ending in ".wav" (any case).
bool valid_sound_file(const std::string& name);

// How a capture's matrices sit in the rig's frame (the `[capture]` table). Absent: the rig cannot be checked by `--parity`.
struct CaptureMap {
    bool present = false;
    float unit = 1;                         // a capture length divided by this is in voxels
    float yaw_sign = 1;                     // +1: a frame's body_yaw turns the figure counter-clockwise about z; -1: clockwise
    bool corner = false;                    // a capture matrix puts a model at its lowest corner, not its centre
};

struct Rig {
    std::string name;
    CaptureMap capture;
    std::vector<Node> nodes;                // in file order
    std::vector<int> order;                 // node indexes, a parent before its children
    int root = -1;
    ParticleWorld particles;
    std::vector<Emitter> emitters;          // in file order
    std::vector<Shockwave> shockwaves;      // in file order
    std::vector<Pillars> pillars;           // in file order
    std::vector<RigSound> sounds;           // in file order
    bool has_effects() const { return !shockwaves.empty() || !pillars.empty(); }
    // The node of this name, or -1.
    int find(const std::string& node_name) const;
    // Bit (1 << Part) of each clip part some node's steps read or answers to: the parts this rig can show moving.
    uint32_t parts_used() const;
};

struct RigLoad {
    bool ok = false;
    Rig rig;
    std::vector<ClipError> errors;          // the clip validator's error type: source, line, field path, code, message, hint
};

// Parses and validates a rig. `source` labels the errors (a file name). Never throws. A rig is refused as a whole: `ok` is false and
// `rig` is empty whenever `errors` is not. Errors are in the order of the lines, at most kyubu::game::anim::kMaxErrors of them.
RigLoad parse_rig(const std::string& text, const std::string& source);

// A 3x4 affine matrix: the point p goes to r * p + t.
struct Affine {
    float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float t[3] = {0, 0, 0};
    void apply(const float p[3], float out[3]) const;
};
Affine operator*(const Affine& a, const Affine& b);   // a * b: b acts first

struct Solved {
    Pose pose;                              // the pose after the clip (solve) or the pose given (solve_pose)
    Applied applied;                        // what the clip wrote (solve); ok = false for solve_pose
    std::vector<Affine> world;              // one per node, in Rig::nodes order: the node's matrix in the figure's frame
    // Parts the clip wrote that no node of the rig reads or answers to: the rig lacks them, so nothing moves for them.
    uint32_t missing_parts = 0;
};

// The product of the first `count` steps of a node for a pose: the frame the step `count` runs in is the parent's world matrix
// times this. `count` above the node's steps takes all of them, so steps_prefix(n, pose, n.steps.size()) is the node's own matrix.
Affine steps_prefix(const Node& node, const Pose& pose, size_t count);
// The product of a list of steps for a pose (an emitter's frame in its node's).
Affine steps_matrix(const std::vector<Step>& steps, const Pose& pose);
// The number one argument stands for (its first, for a name of three numbers).
float arg_value(const Arg& a, const Pose& pose);
// The matrix of every node for a pose. The pose's vectors are the arguments of the steps.
Solved solve_pose(const Rig& rig, const Pose& pose);
// The clip at a time over `base`, through the loader's evaluator (apply_clip), then solve_pose. `base` is not changed.
Solved solve(const Rig& rig, const Pose& base, const Clip& clip, const Timing& at);

}  // namespace kanimator
