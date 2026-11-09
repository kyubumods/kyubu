// The clip registry's link to the mods. See animations.h.
#include "animations.h"

#include "loader.h"
#include "log.h"
#include "modmanager.h"

namespace kyubu {

using namespace game::anim;

Animations& Animations::get() {
    static Animations* a = new Animations();
    return *a;
}

// Every clip file of every active mod, in load order and then by path, so that "the first stays" means the same on
// every machine.
std::vector<Animations::File> Animations::list_files(std::map<std::string, uint64_t>* stamps) {
    std::vector<File> out;
    for (Mod* m : ModManager::get().ordered()) {
        if (!m->active() || !m->pkg) continue;
        std::string root = normalize_rel(m->manifest.assets);
        std::string dir = root.empty() ? "animations" : root + "/animations";
        std::vector<std::string> rels;
        for (auto& rel : m->pkg->list(dir)) {
            if (rel.size() <= dir.size() + 1 || rel.find('/', dir.size() + 1) != std::string::npos) continue;
            // A file without the suffix is not a clip. A file with the suffix and a bad name gets an error in scan().
            std::string lower = rel;
            for (char& c : lower) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            if (!ends_with(lower, kClipSuffix)) continue;
            rels.push_back(rel);
        }
        std::sort(rels.begin(), rels.end());
        for (auto& rel : rels) {
            File f{m, rel, m->pkg->path() + "/" + rel};
            if (stamps) (*stamps)[f.shown] = m->pkg->mtime(rel);
            out.push_back(std::move(f));
        }
    }
    return out;
}

void Animations::scan() {
    std::lock_guard<std::mutex> lock(write_mu_);
    std::map<std::string, uint64_t> stamps;
    std::vector<ClipSource> sources;
    for (File& f : list_files(&stamps)) {
        // The editor's version of a clip stands until the game restarts: the file is not read, and the poll's stamp is kept.
        std::string name;
        if (clip_name_from_file(f.shown, name) && memory_.count(f.mod->id + ":" + name)) {
            KINFO("animation: %s: not read: the editor's version of '%s:%s' is in use", f.shown.c_str(), f.mod->id.c_str(),
                  name.c_str());
            continue;
        }
        auto data = f.mod->pkg->read(f.rel);
        if (!data) {
            // An editor may hold the file while it saves: look once more at the next poll, then wait for a change.
            auto w = watched_.find(f.shown);
            if (w == watched_.end() || w->second != 0) stamps[f.shown] = 0;
            sources.push_back({f.mod->id, f.shown, "", true});
            continue;
        }
        sources.push_back({f.mod->id, f.shown, std::string(data->begin(), data->end()), false});
    }
    for (auto& [id, src] : memory_) sources.push_back(src);
    std::vector<BuildLine> lines;
    const ClipSet* prev = scanned_ ? reg_.current_locked() : nullptr;
    auto next = build_clip_set(sources, prev, &lines);
    size_t count = next->clips.size();
    std::string hash = next->hash;
    bool changed = !scanned_ || hash != reg_.current_locked()->hash;
    const size_t sets_before = reg_.alive();
    reg_.publish(std::move(next));
    if (scanned_) log_freed(sets_before + 1);   // the first scan frees only the empty set, which is not a reload
    for (auto& l : lines) {
        if (l.kind == BuildLine::Error) KERROR("animation: %s", l.text.c_str());
        else if (l.kind == BuildLine::Kept) KWARN("animation: %s", l.text.c_str());
        else KINFO("animation: %s%s", scanned_ ? "hot reload: " : "", l.text.c_str());
    }
    if (changed) KINFO("animation: %u clip(s), set hash %s", (unsigned)count, hash.c_str());
    watched_ = std::move(stamps);
    scanned_ = true;
}

void Animations::poll_changes() {
    uint64_t now = GetTickCount64();
    if (now - last_poll_ < 500) return;
    last_poll_ = now;
    std::map<std::string, uint64_t> stamps;
    list_files(&stamps);
    if (stamps != watched_) {
        scan();
        return;
    }
    // A set the last publish could not free (a draw was holding it) is freed here, once no Hold is alive.
    std::lock_guard<std::mutex> lock(write_mu_);
    const size_t sets_before = reg_.alive();
    reg_.reclaim();
    log_freed(sets_before);
}

void Animations::log_freed(size_t expected) {
    const size_t now = reg_.alive();
    if (now < expected) KINFO("animation: freed %u replaced clip set(s), %u still held", (unsigned)(expected - now), (unsigned)(now - 1));
}

bool Animations::define_from_text(const std::string& mod_id, const std::string& name, const std::string& text,
                                  std::string* errors) {
    auto fail = [&](const std::string& msg) {
        if (errors) *errors = msg;
        return false;
    };
    std::string checked;
    const std::string label = "(editor)/" + name + kClipSuffix;
    if (name.empty() || !clip_name_from_file(label, checked) || checked != name)
        return fail("the clip name '" + name + "' must be 1 to 64 characters of a-z, 0-9, '_', '-' and '.'");
    const std::string id = mod_id + ":" + name;
    ClipSource src{mod_id, label, text, false};
    std::lock_guard<std::mutex> lock(write_mu_);
    std::vector<BuildLine> lines;
    auto next = clip_set_with(*reg_.current_locked(), src, &lines);
    if (!next) {
        std::string all;
        for (auto& l : lines) {
            if (!all.empty()) all += '\n';
            all += l.text;
        }
        return fail(all);
    }
    memory_[id] = std::move(src);
    const size_t count = next->clips.size();
    const std::string hash = next->hash;
    const size_t sets_before = reg_.alive();
    reg_.publish(std::move(next));
    log_freed(sets_before + 1);
    KINFO("animation: defined the clip '%s' from text (%u bytes); %u clip(s), set hash %s", id.c_str(), (unsigned)text.size(),
          (unsigned)count, hash.c_str());
    return true;
}

bool Animations::set_preview(const std::string& clip_id, double t_ms, std::string* why) {
    if (!reg_.hold().set().find(clip_id)) {
        if (why) *why = "no clip '" + clip_id + "' (anim list shows the loaded clips)";
        return false;
    }
    std::lock_guard<std::mutex> lock(preview_mu_);
    preview_.clip_id = clip_id;
    preview_.t_ms = (float)t_ms;
    preview_on_.store(true, std::memory_order_release);
    return true;
}

void Animations::clear_preview() {
    std::lock_guard<std::mutex> lock(preview_mu_);
    preview_on_.store(false, std::memory_order_release);
    preview_ = Preview();
}

void Animations::bind(const std::string& ability_id, const std::string& clip_id) {
    std::lock_guard<std::mutex> lock(bind_mu_);
    bound_[ability_id] = clip_id;
}

bool Animations::bound_clip(const std::string& ability_id, std::string* clip_id) const {
    std::lock_guard<std::mutex> lock(bind_mu_);
    auto it = bound_.find(ability_id);
    if (it == bound_.end()) return false;
    if (clip_id) *clip_id = it->second;
    return true;
}

void Animations::clear_bindings() {
    std::lock_guard<std::mutex> lock(bind_mu_);
    bound_.clear();
    bound_for_.clear();
}

void Animations::bind_for(const std::string& ability_id, uint64_t creature_id, const std::string& clip_id) {
    std::lock_guard<std::mutex> lock(bind_mu_);
    if (clip_id.empty()) bound_for_.erase({ability_id, creature_id});
    else bound_for_[{ability_id, creature_id}] = clip_id;
}

bool Animations::bound_clip(const std::string& ability_id, uint64_t creature_id, std::string* clip_id) const {
    if (creature_id != 0) {
        std::lock_guard<std::mutex> lock(bind_mu_);
        if (!bound_for_.empty()) {   // the common case: nobody has a binding of their own, no map look-up
            auto it = bound_for_.find({ability_id, creature_id});
            if (it != bound_for_.end()) {
                if (clip_id) *clip_id = it->second;
                return true;
            }
        }
    }
    return bound_clip(ability_id, clip_id);
}

void Animations::drop_creature(uint64_t creature_id) {
    std::lock_guard<std::mutex> lock(bind_mu_);
    for (auto it = bound_for_.begin(); it != bound_for_.end();) it = it->first.second == creature_id ? bound_for_.erase(it) : std::next(it);
}

void Animations::clear_creature_bindings() {
    std::lock_guard<std::mutex> lock(bind_mu_);
    bound_for_.clear();
}

std::vector<std::string> Animations::ability_markers(const std::string& ability_id, double from_ms, double to_ms,
                                                     float ability_duration_ms) const {
    return ability_markers(ability_id, 0, from_ms, to_ms, ability_duration_ms);
}

std::vector<std::string> Animations::ability_markers(const std::string& ability_id, uint64_t creature_id, double from_ms, double to_ms,
                                                     float ability_duration_ms) const {
    std::vector<std::string> names;
    std::string clip_id;
    if (!bound_clip(ability_id, creature_id, &clip_id)) return names;
    const auto hold = clips();   // keeps the set alive while `e` is read
    const ClipEntry* e = hold.set().find(clip_id);
    if (!e || e->clip.markers.empty()) return names;
    const float duration = resolve_duration(e->clip, ability_duration_ms);
    for (int i : markers_crossed(e->clip, (float)from_ms, (float)to_ms, duration)) names.push_back(e->clip.markers[(size_t)i].name);
    return names;
}

bool Animations::preview(Preview* out) const {
    if (!preview_on()) return false;
    std::lock_guard<std::mutex> lock(preview_mu_);
    if (!preview_on()) return false;
    if (out) *out = preview_;
    return true;
}

}  // namespace kyubu
