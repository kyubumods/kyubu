// The clip registry. See animation_registry.h.
#include "animation_registry.h"

#include <cstring>
#include <iterator>

#include "sha256.h"

namespace kyubu::game::anim {

const ClipEntry* ClipSet::find(const std::string& id) const {
    auto it = clips.find(id);
    return it == clips.end() ? nullptr : it->second.get();
}

bool clip_name_from_file(const std::string& file, std::string& name) {
    size_t slash = file.find_last_of("/\\");
    std::string base = slash == std::string::npos ? file : file.substr(slash + 1);
    const size_t suffix = std::strlen(kClipSuffix);
    if (base.size() <= suffix) return false;
    std::string tail = base.substr(base.size() - suffix);
    for (char& c : tail)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (tail != kClipSuffix) return false;
    std::string n = base.substr(0, base.size() - suffix);
    if (n.size() > kMaxClipName) return false;
    for (char& c : n) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    name = n;
    return true;
}

std::string clip_set_hash(const ClipSet& set) {
    Sha256 h;
    for (auto& [id, e] : set.clips) {
        h.update(id.data(), id.size());
        h.update("\n", 1);
        h.update(e->text_hash.data(), e->text_hash.size());
        h.update("\n", 1);
    }
    uint8_t digest[32];
    h.final(digest);
    return Sha256::to_hex(digest);
}

std::unique_ptr<ClipSet> build_clip_set(const std::vector<ClipSource>& sources, const ClipSet* prev,
                                        std::vector<BuildLine>* lines) {
    auto say = [&](BuildLine::Kind k, const std::string& id, const std::string& file, std::string text) {
        if (lines) lines->push_back({k, id, file, std::move(text)});
    };
    auto set = std::make_unique<ClipSet>();
    std::map<std::string, std::string> claimed;   // id -> the first file that gave it, loaded or not
    for (const ClipSource& s : sources) {
        std::string name;
        if (!clip_name_from_file(s.file, name)) {
            say(BuildLine::Error, "", s.file,
                s.file + ": not loaded: the name before '.anim.toml' must be 1 to 64 characters of a-z, 0-9, '_', '-' "
                         "and '.'");
            continue;
        }
        std::string id = s.mod_id + ":" + name;
        auto [first, fresh] = claimed.emplace(id, s.file);
        if (!fresh) {
            say(BuildLine::Error, id, s.file,
                s.file + ": not loaded: the clip id '" + id + "' is already taken by " + first->second);
            continue;
        }
        const ClipEntry* old = prev ? prev->find(id) : nullptr;
        auto keep = [&] {
            if (!old) return;
            set->clips[id] = prev->clips.find(id)->second;
            say(BuildLine::Kept, id, s.file,
                s.file + ": the clip '" + id + "' keeps its previous version (from " + old->file + ")");
        };
        if (s.unreadable) {
            say(BuildLine::Error, id, s.file, s.file + ": not loaded: the file could not be read");
            keep();
            continue;
        }
        std::string text_hash = Sha256::hex_of(s.text.data(), s.text.size());
        if (old && old->text_hash == text_hash) {
            set->clips[id] = prev->clips.find(id)->second;
            continue;
        }
        ClipLoad r = parse_clip(s.text, s.file);
        if (!r.ok) {
            for (const ClipError& e : r.errors) say(BuildLine::Error, id, s.file, format_error(e));
            keep();
            continue;
        }
        auto e = std::make_shared<ClipEntry>();
        e->id = id;
        e->mod_id = s.mod_id;
        e->file = s.file;
        e->text_hash = std::move(text_hash);
        e->text = s.text;
        e->clip = std::move(r.clip);
        set->clips[id] = std::move(e);
        say(old ? BuildLine::Reloaded : BuildLine::Loaded, id, s.file,
            std::string(old ? "reloaded" : "loaded") + " the clip '" + id + "' from " + s.file);
    }
    if (prev)
        for (auto& [id, e] : prev->clips)
            if (!claimed.count(id))
                say(BuildLine::Removed, id, e->file, "removed the clip '" + id + "': " + e->file + " is gone");
    set->hash = clip_set_hash(*set);
    return set;
}

std::unique_ptr<ClipSet> clip_set_with(const ClipSet& prev, const ClipSource& source, std::vector<BuildLine>* lines) {
    std::unique_ptr<ClipSet> one = build_clip_set({source}, nullptr, lines);
    if (one->clips.empty()) return nullptr;
    auto set = std::make_unique<ClipSet>();
    set->clips = prev.clips;
    for (auto& [id, e] : one->clips) set->clips[id] = e;
    set->hash = clip_set_hash(*set);
    return set;
}

ClipRegistry::Hold::~Hold() {
    if (reg_) reg_->holds_.fetch_sub(1, std::memory_order_seq_cst);
}

ClipRegistry::ClipRegistry() {
    auto empty = std::make_unique<ClipSet>();
    empty->hash = clip_set_hash(*empty);
    cur_.store(empty.get(), std::memory_order_seq_cst);
    owned_.push_back(std::move(empty));
}

ClipRegistry::~ClipRegistry() = default;

// Seq_cst throughout: the count goes up before the pointer is read, and publish checks the count after it stores the new pointer.
// So a reader that read a replaced set is counted when publish checks, unless it has already left; a reader that starts after the
// store reads the new set. Acquire and release alone could let the check overtake the store (a store-load reorder).
ClipRegistry::Hold ClipRegistry::hold() const {
    holds_.fetch_add(1, std::memory_order_seq_cst);
    return Hold(this, cur_.load(std::memory_order_seq_cst));
}

void ClipRegistry::publish(std::unique_ptr<ClipSet> next) {
    if (!next) return;
    const ClipSet* p = next.get();
    owned_.push_back(std::move(next));
    cur_.store(p, std::memory_order_seq_cst);
    reclaim();
}

void ClipRegistry::reclaim() {
    if (holds_.load(std::memory_order_seq_cst) != 0) return;   // a reader may hold a replaced set: keep them all for now
    const ClipSet* cur = cur_.load(std::memory_order_seq_cst);
    for (auto it = owned_.begin(); it != owned_.end();) it = it->get() == cur ? std::next(it) : owned_.erase(it);
}

}  // namespace kyubu::game::anim
