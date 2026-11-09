// The Animator's offline sheet (openspec add-ability-animation, task 6.9; design D7 item 9 and D11): a labelled picture of the mimic,
// the clip drawn on the rig at some times, in the two fixed views of a capture. It needs no game and no window: the figure is drawn
// by a small software rasteriser, so `kyubu-animator --sheet <clip> --t <times> --out <png>` runs where there is no display (CI, a
// session). The layout, the font and the PNG are mods-dev/anim_capture/src/capture_sheet.h, the one source of both sheets; the
// header names the rig, so a sheet of the mimic is never taken for a capture of the game.
//
// Pure: no windows.h, no ImGui, no game. The command at the bottom reads and writes files.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "camera.h"
#include "capture_sheet.h"
#include "cubmesh.h"
#include "poselib.h"
#include "rig.h"

namespace kanimator {

namespace anim_capture = kyubu::anim_capture;

constexpr size_t kSheetMaxTimes = 12;        // as `anim capture`
constexpr int kSheetViewPx = 256;            // one picture's side before the sheet scales it (the cell is 256, so it is 1 to 1)
constexpr float kSheetGridHalf = 64;         // the ground grid, as the window's
constexpr float kSheetGridStep = 8;

// ---- the figure's meshes

// The mesh a node is drawn with: its box, its `.cub` model from `models_dir`, or (a named model that cannot be read) a box that
// stands for it, with `note` saying why. `note` is empty otherwise. An empty mesh for a node with no model.
Mesh node_mesh(const Rig& rig, size_t node, const std::string& models_dir, std::string& note);

// ---- the rasteriser

// One picture of the figure: `world[i]` places `meshes[i]`, and `extra` (the particles and pillars, particles.h and effects.h), when
// given, is drawn as it is, in the figure's frame; `glass` (the shockwaves) after everything, blended by its vertices' alpha, tested
// against the depth and not written to it. The background, a ground grid when `grid`, and the triangles (no culling, a depth
// buffer), `px` by `px`. A triangle with a vertex at or behind the camera is skipped.
anim_capture::Image render_figure(const std::vector<Mesh>& meshes, const std::vector<Affine>& world, const Camera& cam, int px,
                                  bool grid, const Mesh* extra = nullptr, const Mesh* glass = nullptr);

// The same, `w` by `h` (the video's frames), with a ground grid of `grid_half` voxels each way in steps of `grid_step` (0: none).
anim_capture::Image render_figure_wh(const std::vector<Mesh>& meshes, const std::vector<Affine>& world, const Camera& cam, int w, int h,
                                     float grid_half, float grid_step, const Mesh* extra = nullptr, const Mesh* glass = nullptr,
                                     int line_px = 1);   // line_px: the grid lines' width

// ---- the sheet

struct SheetRequest {
    const Rig* rig = nullptr;
    const std::vector<Mesh>* meshes = nullptr;   // per node, as the window builds them
    const anim::Clip* clip = nullptr;
    const PoseLibrary* library = nullptr;        // may be null: a `[borrow]` clip then gets the neutral pose, and says so
    std::string clip_name;                       // for the header
    float ability_ms = 1000;                     // what `duration = "ability"` means (kanim's own assumption)
    std::vector<float> fractions;                // times as fractions of the clip, 0 to 1 (kanim's `--t`)
    bool grid = true;
};

struct SheetResult {
    bool ok = false;
    anim_capture::Image image;
    std::vector<uint8_t> png;
    int rows = 0, cols = 0;
    std::string header;                          // what the sheet says it is
    std::vector<std::string> labels;             // per cell, row-major (a row per time, a column per view)
    std::vector<double> times_ms;                // per row
    std::vector<std::string> notes;              // things to know: a pose the library lacks
    std::string error;                           // when !ok: one sentence
};

// Draws `fractions` of the clip, front then left. One camera for the whole sheet (the bounds of all its frames), so a part's size
// and place can be compared from a cell to the next.
SheetResult make_mimic_sheet(const SheetRequest& req);

// The header of the sheet of a rig and a clip, at most `max_chars` long: "mimic - rig <rig> - no pose 12 - <clip>"; the clip
// goes first when it does not fit, then the base-pose note, then the rig's name is cut. `no_pose` is -1 for none. Characters the
// sheet's font lacks are kept (the font draws a '?').
std::string sheet_header(const std::string& rig_name, const std::string& clip_name, int no_pose, size_t max_chars);

// ---- the command

struct SheetCommand {
    std::string clip_path;
    std::string out_path;
    std::string times = "0,0.5,1";               // fractions of the clip, as `kanim sample --t`
    std::string rig_path;                        // empty: the first of rig_candidates that exists
    std::vector<std::string> rig_candidates;
    std::string models_dir;
    std::string library_dir;                     // empty: the folder kept in the settings
    float ability_ms = 1000;
};

// What a command draws, read from its files as --sheet reads them: the clip, the rig (`rig_path`, else the first candidate that
// exists), a mesh per node from `models_dir`, the pose library. False with `why` (one sentence); notes go to `out`.
struct SheetScene {
    anim::ClipLoad clip;
    RigLoad rig;
    std::vector<Mesh> meshes;
    PoseLibrary library;
    std::string clip_name;
};
bool load_sheet_scene(const SheetCommand& cmd, SheetScene& scene, std::FILE* out, std::string& why);

// Runs the command. 0 = the sheet is written, 1 = it could not be (the reason on `err`), notes on `out`. Never opens a window.
int run_sheet_command(const SheetCommand& cmd, std::FILE* out, std::FILE* err);

}  // namespace kanimator
