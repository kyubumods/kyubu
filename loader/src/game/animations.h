// The clip registry's link to the mods: scans `<assets>/animations/*.anim.toml` of every enabled mod, logs the result,
// publishes the set and, in dev mode, reloads a changed file. The registry itself is animation_registry.h (pure).
#pragma once
#include "common.h"

#include <atomic>
#include <map>
#include <mutex>

#include "animation_registry.h"

namespace kyubu {

struct Mod;

class Animations {
public:
    static Animations& get();

    // Reads every clip file and publishes a new set. Main thread only (boot, a mod reload, the dev poll).
    void scan();
    // Dev mode: scans again when a clip file changed, appeared or went away. At most one look every 500 ms.
    void poll_changes();

    // The editor's text path: validates `text` as a clip file and publishes it as "<mod_id>:<name>", with no file
    // written. It replaces a clip of that id that a file gives, and a later scan() keeps the replacement (the file's text is
    // not read again until the game restarts). False, with every error one per line in `*errors`, when the name or the text is
    // bad: nothing changes. Any thread; the swap is under a lock.
    bool define_from_text(const std::string& mod_id, const std::string& name, const std::string& text, std::string* errors);

    // The preview clock: the local player is drawn in the pose of `clip_id` at `t_ms`. `set_preview` is false, with
    // `*why`, when the clip is not in the set. The provider (4.2, 5.1) reads `preview()`: `preview_on()` is the cheap test for
    // every draw (an atomic), `preview()` fills `*out` and is false when the clock is off. The clip is looked up at the draw, so
    // a clip that went away gives the game's pose.
    struct Preview {
        std::string clip_id;
        float t_ms = 0;
    };
    bool set_preview(const std::string& clip_id, double t_ms, std::string* why);
    void clear_preview();
    bool preview_on() const { return preview_on_.load(std::memory_order_acquire); }
    bool preview(Preview* out) const;

    // The binding: the clip an ability is drawn with, by id on both sides ("demo:slam" to "demo:slam_clip"). One clip per
    // ability: bind() replaces. api.cpp's ability_animation_bind has checked the ability and the clip; the pose provider (4.2) reads
    // bound_clip() for a mod ability's id at the draw and looks the clip up in clips(): a clip that went away gives the game's pose.
    // Any thread. A binding outlives a scan(): the clip is looked up at the draw.
    void bind(const std::string& ability_id, const std::string& clip_id);
    bool bound_clip(const std::string& ability_id, std::string* clip_id) const;
    void clear_bindings();   // tests

    // The binding for one creature (its game id, never 0), checked as bind() is: while that creature runs the ability, it is drawn with
    // this clip and its markers come from it; others keep the ability's clip. An empty `clip_id` drops it. bound_clip(ability, creature)
    // gives the creature's own binding first, then the ability's. drop_creature() forgets one creature's (it was destroyed), and
    // clear_creature_bindings() every one (the world was left). Any thread.
    void bind_for(const std::string& ability_id, uint64_t creature_id, const std::string& clip_id);
    bool bound_clip(const std::string& ability_id, uint64_t creature_id, std::string* clip_id) const;
    void drop_creature(uint64_t creature_id);
    void clear_creature_bindings();

    // The markers an ability's step crosses (design D6): the names of the markers of the clip bound to `ability_id` that lie in
    // (from_ms, to_ms] of the ability's mode time, in the order crossed, once each (once per lap for a `loop` clip). `from_ms` is -1 on the
    // first step of an activation, so that a marker at t = 0 is told once. `ability_duration_ms` is the ability's, for `duration = "ability"`.
    // Empty when the ability has no clip, the clip is gone or has no markers, or the duration is unknown. It reads the mode time the step
    // runner gives it and nothing the draw knows, so the server side gets the same names. Any thread; no lock beyond the binding's.
    std::vector<std::string> ability_markers(const std::string& ability_id, double from_ms, double to_ms, float ability_duration_ms) const;
    // The same for one creature: its own binding (bind_for) first, then the ability's. Creature id 0 is the ability's alone.
    std::vector<std::string> ability_markers(const std::string& ability_id, uint64_t creature_id, double from_ms, double to_ms,
                                             float ability_duration_ms) const;

    // The current set, held: pointers taken from `clips().set()` (ClipSet::find) stay valid while the Hold lives. Safe from any thread;
    // no lock, two atomic operations. Keep the Hold for as long as a pointer into the set is used.
    game::anim::ClipRegistry::Hold clips() const { return reg_.hold(); }
    std::string hash() const { return reg_.hold().set().hash; }   // for the session manifest

private:
    struct File { Mod* mod; std::string rel; std::string shown; };
    std::vector<File> list_files(std::map<std::string, uint64_t>* stamps);
    // Logs the replaced clip sets that a publish or a reclaim freed. `expected` is the number of sets the registry would hold
    // with nothing freed. Writer only (write_mu_ held).
    void log_freed(size_t expected);

    game::anim::ClipRegistry reg_;
    std::mutex write_mu_;                                         // scan() and define_from_text() publish one at a time
    std::map<std::string, game::anim::ClipSource> memory_;        // id -> the editor's text; it outlives a scan
    mutable std::mutex bind_mu_;
    std::map<std::string, std::string> bound_;                    // ability id -> clip id
    std::map<std::pair<std::string, uint64_t>, std::string> bound_for_;   // (ability id, creature id) -> clip id
    mutable std::mutex preview_mu_;
    std::atomic<bool> preview_on_{false};
    Preview preview_;
    std::map<std::string, uint64_t> watched_;   // shown path -> mtime, as of the last scan
    uint64_t last_poll_ = 0;
    bool scanned_ = false;
};

}  // namespace kyubu
