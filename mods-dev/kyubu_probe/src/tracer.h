// kyubu_probe call tracer: temporary hooks on a list of functions, recording each call (live-probes, "Call
// tracer"). A tracer hook is a mid hook at the function's first instruction, so it needs no prototype: ECX, the
// stack arguments and the return address are read from the CPU state there. The hook engine decides which sites
// are safe; a refused site is reported and skipped. stop() removes every hook the tracer installed.
//
// A detour (add_detour) records calls the same way but is a full function hook, as the loader's own detours are:
// a pass-through handler for a __thiscall function whose callee pops a given number of 32-bit stack arguments. It
// checks a prototype in the running game: when the count is wrong the stack is off after the first call and the
// game crashes, so a session that runs through with the detour in place is the evidence (add-game-api task 2.1).
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kyubu_api.h"

#if defined(_MSC_VER)
#define KPROBE_FASTCALL __fastcall
#else
#define KPROBE_FASTCALL __attribute__((__fastcall__))
#endif

namespace kprobe {

constexpr uint32_t kMaxArgs = 8;
constexpr uint32_t kMaxDetourArgs = 4;   // stack arguments of a detoured function
constexpr int kDetourSlots = 4;          // detours hooked at the same time
constexpr size_t kMaxSites = 256;        // call sites tallied per function

struct Call {
    uint32_t t_ms;              // since start()
    uint32_t ecx;
    uint32_t args[kMaxArgs];    // the first N stack arguments ([esp+4], [esp+8], ...)
    uint32_t nargs;
    uint32_t ret;               // return address
    uint32_t thread;
    int side;                   // KyubuSide the loader ran the hook under
    bool has_regs;              // a mid hook: the registers below are the CPU state at the site
    uint32_t eax, ebx, edx, esi, edi;
};

// Every call is also tallied by where it came from: the return address, ECX and the thread. Unlike the last 32
// calls, the tally keeps a rare caller (another thread's) beside a frequent one (add-game-api task 2.2).
struct Site {
    uint32_t ret;
    uint32_t ecx;
    uint32_t thread;
    uint64_t count;
};

struct Mark {                   // something that happened while tracing, e.g. a key press
    uint32_t t_ms;
    std::string what;
};

class Tracer {
public:
    Tracer(const KyubuApi* api) : api_(api) {}
    ~Tracer();

    // Adds a function by mapping name or address expression (see util.h). Hooks are installed by start().
    bool add(const std::string& target, std::string* err);
    // Adds a __thiscall function to detour instead of trace: the callee pops `stack_args` (0 to kMaxDetourArgs)
    // 32-bit arguments and returns nothing or an integer. Its calls record every stack argument.
    bool add_detour(const std::string& target, uint32_t stack_args, std::string* err);
    bool remove(const std::string& target);
    void clear();                               // stops first
    void set_args(uint32_t n) { nargs_ = n > kMaxArgs ? kMaxArgs : n; }
    uint32_t args() const { return nargs_; }

    // Installs a hook on every target through `self`. Returns how many were installed; `report` gets one line
    // per target (installed, or why the engine refused it).
    int start(KyubuModHandle self, std::vector<std::string>* report);
    // Removes every hook the tracer installed. Returns how many were removed.
    int stop();
    bool running() const { return running_; }

    void mark(const std::string& what);
    uint32_t now_ms() const;

    struct Summary { std::string label; uintptr_t address; bool installed; std::string refused; uint64_t count; };
    std::vector<Summary> summary();
    // The last calls of a target (by label), oldest first; `first_ms` gets the times of its first calls.
    std::vector<Call> recent(const std::string& label, std::vector<uint32_t>* first_ms = nullptr);
    // Every call of a target (by label) tallied by return address, ECX and thread, most calls first; `dropped`
    // gets the calls from sites beyond the first kMaxSites.
    std::vector<Site> sites(const std::string& label, uint64_t* dropped = nullptr);
    std::vector<Mark> marks();
    uint64_t count(const std::string& label);
    std::vector<std::string> report(size_t calls_per_target = 4);   // text lines, for the log and the console

private:
    struct Target {
        Tracer* owner;
        std::string label;
        uintptr_t address;
        KyubuId id = 0;
        std::string refused;
        std::atomic<uint64_t> count{0};
        std::mutex mu;
        std::deque<Call> recent;                // the last 32 calls
        std::vector<uint32_t> first_ms;         // times of the first 256 calls
        std::vector<Site> sites;                // every call by return address, ECX and thread
        uint64_t sites_dropped = 0;             // calls from sites beyond kMaxSites
        int detour_args = -1;                   // -1: traced with a mid hook; else a detour popping this many
        int slot = -1;                          // the detour's handler slot, once hooked
        void* next = nullptr;                   // the detour's next function
    };
    static void KYUBU_CALL on_call(KyubuCpuContext* ctx, void* user);
    template <int Slot, typename... A>
    static uint64_t KPROBE_FASTCALL detour_thunk(void* ecx, void* edx, A... args);
    template <int Slot>
    static void* detour_handler(uint32_t stack_args);
    Target* find(const std::string& label);
    bool add_target(const std::string& target, int detour_args, std::string* err);
    KyubuResult hook(Target& t);
    void record(Target& t, uint32_t ecx, const uint32_t* args, uint32_t nargs, uint32_t ret,
                const KyubuCpuContext* regs = nullptr);

    const KyubuApi* api_;
    KyubuModHandle self_ = nullptr;
    std::mutex mu_;
    std::vector<std::unique_ptr<Target>> targets_;
    std::vector<std::unique_ptr<Target>> retired_;   // stopped targets a thread may still be inside: kept alive
    std::vector<Mark> marks_;
    uint32_t nargs_ = 4;
    bool running_ = false;
    uint64_t start_tick_ = 0;
};

}  // namespace kprobe
