// The Animator's editing model (openspec add-ability-animation, tasks 6.1 and 6.7): one clip file as text, its undo stack and what
// the file looked like when it was last read or written. No ImGui and no API calls: tests/unit/test_animator_model.cpp
// runs it without the game.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kanimator {

constexpr size_t kUndoDepth = 100;   // design D7, item 7: the undo stack keeps 100 steps
constexpr size_t kMaxNameLen = 64;   // animation_define_from_text: the name is 1 to 64 characters
constexpr int64_t kAutosaveMs = 30000;   // design D7, item 7: an autosave this long after the first unsaved edit, then at most this often

// The rule of animation_define_from_text: 1 to 64 of a-z, 0-9, '_', '-' and '.'.
bool valid_clip_name(const std::string& name);
// The clip name of a file path: the file name without ".anim.toml". False when the file name is another or the name is not valid.
bool name_from_path(const std::string& path, std::string& name);
// The text of a new clip: a duration in ms (the preview clock needs one for tracks) and one track.
std::string new_clip_text(const std::string& name);

// Size and last write time of a file. A missing file has size -1.
struct FileStamp {
    int64_t size = -1;
    int64_t mtime = 0;
    bool operator==(const FileStamp& o) const { return size == o.size && mtime == o.mtime; }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};
FileStamp stamp_of(const std::string& path);
// Where the autosave of a clip file goes: beside it, with ".autosave" added. The loader reads only "*.anim.toml", so a game running in
// dev mode never takes the autosave for a clip.
std::string autosave_path(const std::string& clip_path);

class ClipDoc {
public:
    // Reads the file. False with `err` set when it cannot be read; the document is then unchanged.
    bool open(const std::string& path, std::string& err);
    // A clip that is not on disk yet. `text` is its start: nothing is written until save().
    void start_new(const std::string& path, const std::string& text);
    // Writes the text to `path + ".tmp"`, then replaces the file with it: the file is the old text or the new, never half.
    bool save(std::string& err);
    // Reads the file again after an outside edit. The undo stack is cleared: undo would bring back the older text. When the file
    // holds the text the document already has, nothing changes but the known state, and the undo stack stays.
    bool reload(std::string& err);
    // Takes `text` as the clip text: one undo step. The same text is no step.
    void set_text(const std::string& text);
    // Steps back one set_text. False when there is nothing to undo.
    bool undo();
    // True when the file on disk differs from the state last read or written.
    bool disk_changed() const;
    // Keeps the text as it is and takes the file on disk as the known state. A later save overwrites the outside edit.
    void accept_disk();

    // Autosave. `now_ms` is any steady clock in ms. When the text is unsaved, differs from the last autosave and `interval_ms` has
    // passed since the first unsaved edit (or the last autosave), the text goes to autosave_path() (a temp file, then a replace).
    // When nothing is unsaved, an autosave this document wrote is removed. True when it wrote. A document with no path has none.
    bool autosave_tick(int64_t now_ms, int64_t interval_ms, std::string& err);
    // An autosave of this clip that a run before this one left and that differs from the text: true with `text` set. An autosave
    // that holds the text already is removed, and gives false.
    bool find_autosave(std::string& text);
    // Removes the autosave file (the author said discard, or the document was saved).
    void drop_autosave();

    const std::string& path() const { return path_; }
    const std::string& text() const { return text_; }
    bool dirty() const { return text_ != saved_text_; }
    size_t undo_depth() const { return undo_.size(); }

private:
    void forget_autosave_state();

    std::string path_;
    std::string text_;
    std::string saved_text_;
    std::vector<std::string> undo_;   // oldest first; the text before each step
    FileStamp known_;
    std::string autosaved_text_;       // what this document last wrote to the autosave file
    bool autosave_written_ = false;    // the file at autosave_path() is this document's
    int64_t dirty_since_ms_ = -1;      // the first autosave_tick that saw the text unsaved; -1 when it is saved
    int64_t last_autosave_ms_ = -1;
};

}  // namespace kanimator
