// The clip registry: a table of clips keyed by string id, built from the text of clip files.
//
// Pure code: no game, no file access, no log. The caller (animations.cpp) reads the files and logs the lines.
//
// A clip has no number. Nothing here goes in a save or in the id maps; only the hash of the set goes in the session
// manifest.
//
// The set is immutable once built. A reload builds a new set from the old one and publishes it with one pointer swap, so
// reading the set takes no lock. A reader takes a Hold; a replaced set is freed only when no Hold is alive, so a pointer
// taken through a Hold stays valid until the Hold ends.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "animation_load.h"

namespace kyubu::game::anim {

constexpr const char* kClipSuffix = ".anim.toml";
constexpr size_t kMaxClipName = 64;   // the part of the id after "<mod id>:"

// The text of one clip file. `file` is the path shown in the log; its last component gives the clip's name.
struct ClipSource {
    std::string mod_id;
    std::string file;
    std::string text;
    bool unreadable = false;   // the file is there and could not be read: an error, and the clip keeps its entry
};

struct ClipEntry {
    std::string id;          // "<mod id>:<name>"
    std::string mod_id;
    std::string file;        // the file that gave this version of the clip
    std::string text_hash;   // SHA-256 of that file's text, in hex
    std::string text;        // that file's text, for `anim show` and `anim sample` (kanim's commands read text)
    Clip clip;
};

struct ClipSet {
    std::map<std::string, std::shared_ptr<const ClipEntry>> clips;   // by id
    std::string hash;        // SHA-256 over every (id, text_hash) in id order, in hex

    const ClipEntry* find(const std::string& id) const;
};

// What happened to one file or one clip during a build. `text` is the whole log line.
struct BuildLine {
    enum Kind { Loaded, Reloaded, Removed, Kept, Error };
    Kind kind;
    std::string id;     // empty when the file gave no id
    std::string file;
    std::string text;
};

// The name of the clip in `file`: the last path component without ".anim.toml", ASCII letters in lower case.
// False when the suffix is missing or the name is not 1 to 64 characters of a-z, 0-9, '_', '-' and '.'.
bool clip_name_from_file(const std::string& file, std::string& name);

// Builds a set from `sources`, taken in order: an id that an earlier source claimed is rejected with a line that names
// both files. A clip with an error (or a file that could not be read) is rejected whole and the others are not touched.
// With `prev`, a rejected clip keeps its entry of `prev` (kind Kept), an unchanged text reuses its entry without a line,
// and an id of `prev` that no source gives is dropped (kind Removed).
std::unique_ptr<ClipSet> build_clip_set(const std::vector<ClipSource>& sources, const ClipSet* prev,
                                        std::vector<BuildLine>* lines);

// A copy of `prev` with the one clip of `source` added, or replacing the clip of that id (the editor's text path).
// Null when the clip is rejected, with the errors in `lines` as for build_clip_set; `prev` is not touched either way.
std::unique_ptr<ClipSet> clip_set_with(const ClipSet& prev, const ClipSource& source, std::vector<BuildLine>* lines);

// The hash of a set, from its entries. build_clip_set fills ClipSet::hash with it.
std::string clip_set_hash(const ClipSet& set);

// Holds the current set and the sets a publish replaced. A reader takes a Hold from any thread: two atomic operations, no lock.
// `publish`, `reclaim` and `current_locked` are for one thread at a time: the caller holds its write lock. Only publish replaces
// a set, and only the writer frees one, so the writer's own read of the current set cannot dangle.
class ClipRegistry {
public:
    // A reader's hold on the set that was current when it was taken. While the Hold lives, no replaced set is freed.
    class Hold {
    public:
        Hold(Hold&& other) noexcept : reg_(other.reg_), set_(other.set_) { other.reg_ = nullptr; }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        Hold& operator=(Hold&&) = delete;
        ~Hold();
        // Never null. Pointers taken from it (ClipSet::find) are valid until this Hold ends.
        const ClipSet& set() const { return *set_; }

    private:
        friend class ClipRegistry;
        Hold(const ClipRegistry* reg, const ClipSet* set) : reg_(reg), set_(set) {}
        const ClipRegistry* reg_;
        const ClipSet* set_;
    };

    ClipRegistry();
    ~ClipRegistry();
    ClipRegistry(const ClipRegistry&) = delete;
    ClipRegistry& operator=(const ClipRegistry&) = delete;

    Hold hold() const;
    const ClipSet* current_locked() const { return cur_.load(std::memory_order_seq_cst); }   // never null; writer only
    void publish(std::unique_ptr<ClipSet> next);
    // Frees every replaced set that no Hold covers. publish calls it; a reader may still hold the set, so a later call
    // (the dev poll) frees what the publish could not.
    void reclaim();
    // The sets alive: the current one and the replaced ones not yet freed. Writer only.
    size_t alive() const { return owned_.size(); }

private:
    std::atomic<const ClipSet*> cur_;
    mutable std::atomic<int> holds_{0};   // Holds alive, from any thread
    std::vector<std::unique_ptr<const ClipSet>> owned_;   // the current set and the replaced ones not yet freed
};

}  // namespace kyubu::game::anim
