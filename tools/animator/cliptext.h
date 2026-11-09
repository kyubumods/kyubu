// The Animator's edits of a clip's text (openspec add-ability-animation, tasks 6.4 and 6.5): the few changes a key can take, each
// made in the text itself so that comments, layout and every other key stay as the author wrote them. Pure: no windows.h, no ImGui.
// posing.cpp (a drag on a handle) and timeline.cpp (keys moved, deleted, pasted) are built on these.
//
// Every function takes the text and gives the new text in `out`; false with one sentence in `err` when the text cannot take the
// edit. None of them validates the clip: the caller parses the result with the loader's validator and drops it when that fails.
// A track is an index into the file's `[[track]]` tables, a key an index into that track's `keys`, as in anim::Clip.
#pragma once
#include <string>
#include <vector>

#include "animation_eval.h"

namespace kanimator {

namespace anim = kyubu::game::anim;

constexpr float kKeyEpsilonMs = 0.5f;   // two times this close are the same key

// A value to 3 decimals and a time to 5, with no trailing zeros. A time always has a decimal point (`1.0`, not `1`), as the files
// of the repository write it.
std::string fmt_value(double v);
std::string fmt_time(double t);
std::string vec_text(const float v[3]);
// `{ t = 0.35, v = [-110, 0, 10], ease = "ease_out" }`; a linear ease is not written.
std::string key_text(float t, const float v[3], anim::Ease ease);
// "\r\n" when the text has one, else "\n".
std::string text_eol(const std::string& text);

struct KeyTime {
    int track = -1;
    int key = -1;
    float t = 0;   // 0..1 of the duration
};
// Writes a new `t` into each key named. Only those numbers change.
bool set_key_times(const std::string& text, const std::vector<KeyTime>& times, std::string& out, std::string& err);
// Writes a new `v` into one key.
bool set_key_value(const std::string& text, int track, int key, const float v[3], std::string& out, std::string& err);
// Writes `with` in the place of one key: its time, value and ease.
bool replace_key(const std::string& text, int track, int key, const anim::Key& with, std::string& out, std::string& err);
// Takes one key out. A key alone on its line goes with the line (and a comment after it); a key that shares a line goes with its
// comma.
bool remove_key(const std::string& text, int track, int key, std::string& out, std::string& err);
// Puts a key into the track at its place in the order: after the keys at or before its time. A key between lines gets a line of
// its own with the indent of its neighbour. `index` gets where it landed. The track must have a key already, and its keys must be
// a `keys = [ ... ]` list of `{ ... }` tables.
bool insert_key(const std::string& text, int track, const anim::Key& key, std::string& out, std::string& err, int* index = nullptr);
// Adds an `add` track at the end of the file with these keys (one or more, in order). Refused when the file writes its tracks as
// a `track = [ ... ]` list: it cannot also have a `[[track]]` table.
bool append_track(const std::string& text, anim::Part part, anim::Channel channel, const std::vector<anim::Key>& keys,
                  std::string& out, std::string& err);
// The same with a mode written (`add` or `set`).
bool append_track(const std::string& text, anim::Part part, anim::Channel channel, anim::TrackMode mode,
                  const std::vector<anim::Key>& keys, std::string& out, std::string& err);
// Takes one `[[track]]` table out, from its header to the next table or the end of the file. Comment and blank lines just before
// the next table stay (they are most likely its own). Refused for a `track = [ ... ]` list.
bool remove_track(const std::string& text, int track, std::string& out, std::string& err);

}  // namespace kanimator
