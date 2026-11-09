// The Animator's timeline (openspec add-ability-animation, task 6.5): the rows, the maths of the time axis (snap, zoom, pan), the
// selection, and the edits of keys: add, move, delete, copy and paste. Pure: no windows.h, no ImGui, no game. app.cpp feeds it the
// pointer and draws what it returns; tests/unit/test_animator_timeline.cpp runs it without a window.
//
// Every edit is a function of the clip's text and gives the new text, parsed by the loader's validator: an edit that would make an
// invalid clip changes nothing. The text is edited through cliptext.h, so comments and layout stay.
#pragma once
#include <string>
#include <vector>

#include "animation_eval.h"
#include "cliptext.h"

namespace kanimator {

// ---------------------------------------------------------------------------------------------------------------------------------
// Rows and the selection

// One key of the clip: an index into its tracks and into that track's keys.
struct KeyRef {
    int track = -1;
    int key = -1;
    bool operator==(const KeyRef& o) const { return track == o.track && key == o.key; }
    bool operator<(const KeyRef& o) const { return track != o.track ? track < o.track : key < o.key; }
};
// Keys in order of (track, key), each once.
using Selection = std::vector<KeyRef>;
void select_add(Selection& sel, KeyRef k);
void select_toggle(Selection& sel, KeyRef k);
bool selected(const Selection& sel, KeyRef k);

// One row of the timeline: a part and a channel that the clip has a track for.
struct TimelineRow {
    anim::Part part = anim::Part::Root;
    anim::Channel channel = anim::Channel::Rot;
    int edit_track = -1;          // the LAST track on the part and channel: the one a new key goes to (as posing.h)
    std::vector<KeyRef> keys;     // the keys of every track on the part and channel, in order of time
};
// The rows in the order of the parts, `pos` before `rot`.
std::vector<TimelineRow> timeline_rows(const anim::Clip& clip);
// "hand_r.rot"
std::string row_label(const TimelineRow& row);

// ---------------------------------------------------------------------------------------------------------------------------------
// The time axis

constexpr float kDefaultSnapMs = 10.0f;   // the spec: 10 ms by default
constexpr float kMaxTimelineZoom = 64.0f;

// The time on the grid nearest `t_ms`, held inside the clip. The end of the clip is on the grid whatever the step. A step of 0 (or
// less) is no snap.
float snap_ms(float t_ms, float step_ms, float duration_ms);

// What part of the clip the timeline shows. At zoom 1 the whole clip fills the width.
struct TimelineView {
    float x = 0;            // the pixel of the left edge of the key area
    float width = 1;        // its width in pixels
    float length_ms = 0;    // the clip's duration
    float zoom = 1;         // 1 to kMaxTimelineZoom
    float start_ms = 0;     // the time at the left edge

    float px_per_ms() const;
    float x_of(float t_ms) const;
    float ms_at(float px) const;
    float visible_ms() const;
    // Takes the size and the length of this frame and keeps `zoom` and `start_ms` inside their bounds.
    void fit(float left, float w, float length);
    // Zooms by `factor` (above 1: closer) and keeps the time under the pixel `px` under it.
    void zoom_at(float px, float factor);
    // Moves the shown part by `dx` pixels (a drag to the right shows earlier times).
    void pan_px(float dx);
};

// The distance between two labelled ticks of the ruler: the smallest of 1, 2, 5, 10, 20, 50, ... ms that is `min_px` wide or more.
float tick_step_ms(float px_per_ms, float min_px);

// The key of the row nearest to `t_ms`, when one is within `tolerance_ms`; else {-1, -1}.
KeyRef key_near(const anim::Clip& clip, const TimelineRow& row, float t_ms, float tolerance_ms, float duration_ms);
// The keys of rows `row_a` to `row_b` (either order, held inside the rows) with a time from `ms_a` to `ms_b` (either order).
Selection keys_in_box(const anim::Clip& clip, const std::vector<TimelineRow>& rows, int row_a, int row_b, float ms_a, float ms_b,
                      float duration_ms);

// ---------------------------------------------------------------------------------------------------------------------------------
// The edits

struct KeyEditResult {
    bool ok = false;
    std::string text;         // the clip text after the edit (the text given when !ok, or when nothing had to change)
    anim::Clip clip;          // that text, parsed (when ok)
    std::string error;        // one sentence, when !ok
    Selection selection;      // the keys made or moved, as they are in the new clip
    int replaced = 0;         // keys that were where a moved or pasted key landed, and are gone
    int dropped = 0;          // paste: keys that fell past the end of the clip
    int made_tracks = 0;      // paste: tracks made for a part and channel that had none
};

// A key at `t_ms` on the last track of the part and channel, with the value the track has there and the ease of the key after it
// (the pose at that time does not change; an eased segment cut in two does change in between). A key already within half a millisecond is selected and nothing changes. The clip must have a
// track for the part and channel (a drag on a handle makes one).
KeyEditResult add_key(const std::string& clip_text, anim::Part part, anim::Channel channel, float t_ms, float duration_ms);

// For edits made in other files (keyinspector.h): `fail_edit` is a refused edit; `finish_edit` takes `out` as the new text when the
// loader's validator passes it and every key of `res.selection` is in the new clip, as every edit above does.
KeyEditResult fail_edit(const std::string& clip_text, const std::string& why);
KeyEditResult finish_edit(const std::string& clip_text, std::string out, KeyEditResult res);

// How far the selection can move toward `delta_ms`: its first key stops at 0 and its last at the end of the clip.
float clamp_move_ms(const anim::Clip& clip, const Selection& sel, float delta_ms, float duration_ms);

// Moves the selected keys by `delta_ms` (clamped by clamp_move_ms; the keys keep their distances). A key keeps its value and its
// ease. A key that is not selected and is within half a millisecond of where a moved key lands is replaced by it. When no key
// passes another only the `t` numbers change in the text; a key that passes another moves to its new place in the list.
KeyEditResult move_keys(const std::string& clip_text, const Selection& sel, float delta_ms, float duration_ms);

// Deletes the selected keys. Refused when a track would have no key left: the loader refuses such a track.
KeyEditResult delete_keys(const std::string& clip_text, const Selection& sel);

struct ClipboardKey {
    anim::Part part = anim::Part::Root;
    anim::Channel channel = anim::Channel::Rot;
    float offset_ms = 0;      // after the earliest key copied
    float v[3] = {0, 0, 0};
    anim::Ease ease = anim::Ease::Linear;
};
struct KeyClipboard {
    std::vector<ClipboardKey> keys;   // in order of time
    bool empty() const { return keys.empty(); }
};
// Times are kept in milliseconds, so a paste into a clip of another length keeps the timing.
KeyClipboard copy_keys(const anim::Clip& clip, const Selection& sel, float duration_ms);

// Pastes the keys with the earliest at `at_ms`, each on the last track of its own part and channel. A key within half a
// millisecond of a pasted one is replaced; a part and channel with no track gets a new `add` track; a key that would fall past the
// end of the clip is dropped. Refused when every key is dropped.
KeyEditResult paste_keys(const std::string& clip_text, const KeyClipboard& clipboard, float at_ms, float duration_ms);

}  // namespace kanimator
