#pragma once
#include "common.h"
#include "manifest.h"
#include "package.h"
#include "resolver.h"

#include <map>
#include <memory>

namespace kyubu {

enum class ModState { Loaded, Disabled, Failed, Faulted, SkippedSide, New, Missing };
// "new" and "missing" are only for the manage listing (D15, mods_rescan): a package added to mods\ after boot
// is New until a restart, a package whose files are gone is Missing.
const char* mod_state_name(ModState s);

struct Mod;
struct ModConfig;

// The opaque KyubuModHandle handed to mods: one per (mod, logical side).
struct ModContext {
    uint32_t magic = 0x4B594D43;   // 'KYMC'
    Mod* mod = nullptr;
    KyubuSide side = KY_SIDE_NONE;
};

struct Mod {
    std::string id;
    std::string path;
    Manifest manifest;
    bool manifest_ok = true;                 // false when mod.toml does not parse (the state is failed)
    std::unique_ptr<Package> pkg;
    ModState state = ModState::Loaded;
    std::string reason;
    int order = 0;
    ModContext ctx[3];                       // [KY_SIDE_CLIENT], [KY_SIDE_SERVER]
    std::map<std::string, HMODULE> native;   // entry key -> module
    std::vector<std::pair<uintptr_t, uintptr_t>> code_ranges;
    std::vector<KyubuModShutdownFn> shutdown_fns;
    std::vector<KyubuSide> shutdown_sides;
    // statistics
    double ms_total = 0, ms_frame = 0, ms_last_frame = 0;
    uint64_t calls = 0;
    std::string data_dir;
    std::string cache_dir;
    std::shared_ptr<ModConfig> config;       // declared options (modconfig.h); null when the mod has none

    KyubuModHandle handle(KyubuSide s) { return (KyubuModHandle)&ctx[s == KY_SIDE_SERVER ? 2 : 1]; }
    bool active() const { return state == ModState::Loaded; }
    bool has_side(KyubuSide s) const { return (manifest.sides & s) != 0; }
};

// Thread-local "who is running" record, set by invoke().
struct CallFrame { Mod* mod; KyubuSide side; CallFrame* prev; };
CallFrame* current_call();
KyubuSide current_side_tls();
Mod* current_mod_tls();

class ModManager {
public:
    static ModManager& get();

    // Scans mods/ and resolves against config/host. Does not load code.
    void discover_and_resolve();
    // Loads symbol maps and native entry points of every resolved mod (PreInit).
    void load_native();

    Mod* find(const std::string& id);
    Mod* from_handle(KyubuModHandle h, KyubuSide* side = nullptr);
    std::vector<Mod*> ordered();                 // loaded mods in load order
    const std::vector<std::unique_ptr<Mod>>& all() const { return mods_; }
    int order_of(const std::string& owner) const;
    std::string mod_for_address(uintptr_t addr);

    // Runs fn as a callback of `mod` on `side`: guard, fault handling, timing, current-side.
    // Returns false if the mod is not active or the callback faulted.
    bool invoke(Mod* mod, KyubuSide side, const char* what, const std::function<void()>& fn);
    void mark_faulted(Mod* mod, const std::string& why, KyubuSide side = KY_SIDE_NONE);

    void frame_stats_roll();   // called once per frame: ms_frame -> ms_last_frame
    std::vector<std::string> summary_lines();

    uint32_t host_sides() const;

private:
    std::vector<std::unique_ptr<Mod>> mods_;
    std::vector<Candidate> candidates_;
    std::map<std::string, int> order_;
};

}  // namespace kyubu
