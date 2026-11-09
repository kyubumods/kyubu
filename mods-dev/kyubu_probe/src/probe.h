// kyubu_probe: the developer mod behind mapping/probes/ (live-probes). It is active only in dev mode; otherwise it
// logs why and installs nothing. Its tools (inspector, field finder, call tracer) are driven from the overlay
// window (F8), the console (kyubu_probe:<command>), or a script (probe.cfg in the mod folder) that runs unattended
// once the game is ready, which is how probe replays work. docs/probes.md is the manual.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "finder.h"
#include "inspector.h"
#include "tracer.h"
#include "util.h"

namespace kprobe {

using Print = std::function<void(const std::string&)>;

constexpr const char* kModId = "kyubu_probe";

class Probe {
public:
    // The mod's entry points (main.cpp). init returns 0: an inactive probe (dev mode off) stays loaded, idle.
    static int init(const KyubuApi* api, KyubuModHandle self, KyubuSide side);
    static void shutdown(KyubuSide side);
    static Probe* get();    // nullptr while inactive

    // Runs one command (without the "kyubu_probe:" prefix) for `side`, printing its output.
    void run(const std::string& line, KyubuSide side, const Print& out);
    // Starts a script (probe.cfg syntax) for `side` on the probe's worker thread; its output goes to the log.
    bool start_script(const std::string& text, KyubuSide side, std::string* err);
    bool script_running() const { return script_active_; }

    Finder& finder() { return finder_; }
    Tracer& tracer() { return tracer_; }
    Inspector& inspector() { return inspector_; }
    KyubuModHandle handle(KyubuSide side) const;

    struct Tally { int held = 0, failed = 0; };
    Tally tally() const { return tally_; }

    explicit Probe(const KyubuApi* api);
    ~Probe();

private:
    void activate(KyubuModHandle self, KyubuSide side);
    void log(KyubuSide side, const std::string& text);
    void draw();
    void on_key(KyubuEvent* ev);
    void worker();
    void step_script();
    void sample_watches();
    int hooks_owned();
    void remove_triggers();
    void cmd_inspect(const std::vector<std::string>& a, const std::string& line, KyubuSide side, const Print& out);
    bool player_view(const std::string& controller, const Print& out);
    void creatures_view(const std::string& world, const Print& out);
    bool controller(const std::string& expr, uintptr_t* out, std::string* err);
    bool creature_list(const std::string& world_expr, uintptr_t* world, std::vector<MapEntry>* out, std::string* err);
    void cmd_find(const std::vector<std::string>& a, const std::string& line, KyubuSide side, const Print& out);
    void cmd_trace(const std::vector<std::string>& a, const std::string& line, KyubuSide side, const Print& out);
    void cmd_watch(const std::vector<std::string>& a, const std::string& line, const Print& out);
    void cmd_expect(const std::vector<std::string>& a, const std::string& line, const Print& out);
    bool evaluate(const std::vector<std::string>& a, const std::string& line, bool* held, std::string* detail);
    bool snapshot(const std::string& why, const Print& out);
    std::string row_line(const Row& r) const;
    static void KYUBU_CALL on_find_hook(KyubuCpuContext* ctx, void* user);
    static void KYUBU_CALL on_find_event(KyubuEvent* ev, void* user);

    const KyubuApi* api_;
    KyubuHost host_ = KY_HOST_UNKNOWN;
    KyubuModHandle client_ = nullptr, server_ = nullptr;
    std::vector<std::pair<KyubuModHandle, KyubuId>> subs_;
    std::recursive_mutex mu_;             // one command at a time (console, script, UI)
    Finder finder_;
    Tracer tracer_;
    Inspector inspector_;
    EcxLookup ecx_;                       // ecx(<label>) in address expressions: the tracer's latest call

    // inspector
    const CwLayoutType* insp_t_ = nullptr;
    uintptr_t insp_addr_ = 0;
    std::vector<std::pair<const CwLayoutType*, uintptr_t>> insp_back_;   // before each follow
    std::vector<MapEntry> creatures_;     // the creature list the last `inspect creatures` read (the window's list)
    uintptr_t creatures_player_ = 0;
    std::string controller_expr_, world_expr_;   // the last addresses the views were given (reused without one)

    // finder
    int search_ = 0;                                   // the active search
    std::map<int, const CwLayoutType*> search_types_;  // searches over a typed object
    uint32_t snap_key_ = 0x76;                         // F7
    std::string snap_event_, snap_hook_;
    KyubuId snap_event_id_ = 0, snap_hook_id_ = 0;
    KyubuModHandle snap_owner_ = nullptr;
    std::atomic<uint32_t> last_hook_snap_{0};

    // watches: values sampled every 10 ms by the worker
    struct Watch { std::string name; VType type; uintptr_t address; std::vector<double> samples; int unreadable = 0; };
    std::mutex watch_mu_;
    std::vector<Watch> watches_;

    // script and worker
    std::thread worker_;
    std::atomic<bool> stop_{false}, world_{false}, script_active_{false};
    std::vector<std::string> script_;
    size_t pc_ = 0;
    KyubuSide script_side_ = KY_SIDE_NONE;
    uint64_t wait_until_ = 0;
    bool wait_world_ = false;
    std::string wait_check_;              // wait <s> until <check>: the check as an expect line ("expect - ...")
    uint64_t wait_check_next_ = 0, wait_started_ = 0;
    std::atomic<uint32_t> frame_{0};      // frame.begin's counter
    uint32_t script_frame0_ = 0;
    uint64_t script_t0_ = 0;
    Tally tally_;

    // UI
    bool ui_open_ = false;
    char ui_type_[64] = "cube::CreatureData", ui_addr_[128] = "", ui_field_[64] = "", ui_value_[64] = "";
    char ui_find_addr_[128] = "", ui_find_len_[32] = "0x100", ui_find_type_[8] = "f32", ui_find_val_[32] = "0";
    char ui_trace_[128] = "";
    std::vector<std::string> ui_lines_;   // output of the last action taken in the window
};

}  // namespace kprobe
