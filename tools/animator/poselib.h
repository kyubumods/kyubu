// The Animator's pose library (openspec add-ability-animation, task 6.8; design D7 item 6 and D12 "Baked poses"): the baked vanilla
// poses that `anim bake <pose id>` writes (docs/dev-tools.md, "Rig and bake files"), read from a folder outside the repository; the
// base pose of a `[borrow]` clip at the retimed clock; and the two edits that turn a baked pose into keys of the open clip (Capture
// at t, Bake to keys). Pure: no windows.h, no ImGui, no game. The numbers are in the files the folder holds: the program has none of
// the game's.
//
// The folder. A baked pose is `pose.json` with `kind` "bake", in a folder of its own named as the mod names it
// (`bake_pose<id>-<stamp>`). The library reads the folders directly under the chosen folder and under `<folder>/captures` (where
// `RECIPES.md` copies them). The pose id comes from the file (`pose_id` of each frame), not from the folder's name; when two folders
// hold the same pose, the one with the later name (the later stamp) is the one used. A `rig` or `capture` file is not a baked pose
// and is passed over without a word; a `bake` file that cannot be read is named in the scan's problems.
#pragma once
#include <string>
#include <vector>

#include "animation_eval.h"

namespace kanimator {

namespace anim = kyubu::game::anim;

constexpr size_t kMaxPoseFileBytes = 64u << 20;
constexpr size_t kMaxBakedFrames = 100000;
// Bake to keys keeps a draw only when it is this long after the last kept one: at most 30 keys a second (the mod's own rule).
constexpr double kBakeKeyGapMs = 1000.0 / 30.0;
// The pose ids of the game's pose catalogue: 0 to 73.
constexpr int kMaxPoseId = 73;

struct BakedFrame {
    double t_ms = 0;          // milliseconds from the start of the pose
    anim::Pose pose;          // the vectors the game used at the join site
};

struct BakedPose {
    int pose_id = -1;
    std::string source;       // the file it was read from
    std::string build;        // the game build the capture names
    std::vector<BakedFrame> frames;   // in time order, at least one
    double length_ms() const { return frames.empty() ? 0.0 : frames.back().t_ms; }
};

struct BakedLoad {
    bool ok = false;
    bool other_kind = false;  // a valid capture file of another kind (`rig`, `capture`, ...): not an error, not a pose
    BakedPose pose;
    std::string error;        // one sentence naming the file and the field
};

// Parses the text of a `pose.json`. `source` labels the error. Never throws. The format is `kyubu-capture/1`, the kind `bake`, and
// every frame has `t_ms`, `pose_id` and the six parts' `rot` (and `pos`, but for the tail); a `null` number reads as 0 (the mod
// writes it for a number that is not finite). The frames must be in time order.
BakedLoad parse_baked_pose(const std::string& json_text, const std::string& source);

// The pose at `t_ms`: linear between the two frames either side, held before the first frame and after the last. `*past_end` is set
// to whether `t_ms` is later than the last frame (the game draws pose 0 once the mode is over: the figure then shows the last frame,
// and the view says so).
anim::Pose sample_baked(const BakedPose& p, double t_ms, bool* past_end = nullptr);

class PoseLibrary {
public:
    struct Scan {
        int loaded = 0;                      // poses now in the library
        int replaced = 0;                    // files passed over because a later folder holds the same pose
        std::vector<std::string> problems;   // a `bake` file that could not be used: one sentence each
    };
    // Forgets what the library held and reads `folder`. An empty or missing folder gives an empty library and a problem.
    Scan scan(const std::string& folder);
    // For tests and callers that already have a pose; replaces the same id.
    void add(BakedPose p);
    void clear();

    const BakedPose* find(int pose_id) const;
    std::vector<int> ids() const;            // sorted
    size_t size() const { return poses_.size(); }
    const std::string& folder() const { return folder_; }

private:
    std::string folder_;
    std::vector<BakedPose> poses_;           // sorted by pose id
};

// ---- the settings the Animator keeps between runs (a small JSON file the window writes; the path is app.cpp's)

struct Settings {
    std::string library_folder;   // the pose library folder, or empty for none
    std::string game_folder;      // the game folder the `In game` button drops its request in (task 7.6), or empty; the button itself is not kept
    std::string sound_folder;     // the Sounds panel's folder of .wav files (sounds.h), or empty
    float sound_volume = 1;       // the Sounds panel's volume, 0 to 2
};
// An unreadable text, or a field that is not a string, gives that field empty: a missing or damaged file is not an error.
Settings parse_settings(const std::string& text);
std::string settings_text(const Settings& s);
// Where the window keeps it: %APPDATA%/kyubu-animator/settings.json, or empty when there is no APPDATA (the setting is then not kept).
std::string settings_file_path();

// What a clip's base pose is at a time.
struct BaseResult {
    enum class Kind : unsigned char {
        Neutral,        // the clip has no `[borrow]`: the rig's neutral pose
        Borrowed,       // the baked frames of the borrowed pose, at the retimed clock
        Missing,        // the clip borrows a pose the library lacks: the neutral pose, and the view names the pose
    } kind = Kind::Neutral;
    anim::Pose pose;
    int pose_id = -1;           // Borrowed and Missing
    double pose_ms = 0;         // Borrowed: the time in the pose (offset_ms + elapsed * speed, 0 or more)
    bool past_end = false;      // Borrowed: `pose_ms` is later than the baked pose lasts
};
// `elapsed_ms` is the clip's time, as the evaluator takes it. The clock is the loader's: borrowed_mode_time().
BaseResult base_pose_for(const PoseLibrary& lib, const anim::Clip& clip, float elapsed_ms);

// What the view says when the base is not the neutral pose or the pose is not in the library: "" for a neutral base.
std::string base_label(const BaseResult& r);

// ---- the two edits. Each takes the clip's text and gives the new text in `out`; false with a sentence in `err`. Neither validates.

// Capture at t: the baked pose's vectors at `pose_ms` become one key each, at `clip_u` (0..1 of the clip's duration), in `set` tracks:
// the `pos` of each part but the tail's, and the `rot` of each part (11 tracks). A part and channel that has a `set` track already
// gets the key in it (a key within kKeyEpsilonMs of that time is replaced, keeping its ease; `duration_ms` turns the fractions into
// milliseconds); one that has none gets a new `set` track at the end of the file. `clip` is the clip as parsed from `text`.
struct CaptureResult {
    int tracks_made = 0;
    int keys_added = 0;
    int keys_replaced = 0;
    int over_add_tracks = 0;    // part and channels that also have an `add` track: the new `set` track runs after it and replaces it
};
bool capture_at(const std::string& text, const anim::Clip& clip, const BakedPose& pose, double pose_ms, float clip_u,
                float duration_ms, std::string& out, std::string& err, CaptureResult* result = nullptr);

// Bake to keys: every draw of the pose (thinned to at most 30 a second, the first and the last always kept) becomes a key in the 11
// `set` tracks, at t_ms / `duration_ms` of the clip. A track that exists as `set` for the part and channel is replaced as a whole;
// an `add` track is left alone. Draws later than `duration_ms` are dropped (`*dropped`). The pose starts at the clip's time 0 and
// plays at its own speed: a clip that still has a `[borrow]` of the same pose draws it twice, so remove the borrow after. Refused when
// fewer than two keys remain.
struct BakeResult {
    int tracks_made = 0;
    int tracks_replaced = 0;
    int keys = 0;               // per track
    int dropped = 0;            // draws past the clip's end
    int over_add_tracks = 0;
};
bool bake_to_keys(const std::string& text, const anim::Clip& clip, const BakedPose& pose, float duration_ms, std::string& out,
                  std::string& err, BakeResult* result = nullptr);

}  // namespace kanimator
