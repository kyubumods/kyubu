// Hook framework: chained inline/IAT/vtable hooks, mid-function hooks and byte patches.
#pragma once
#include "common.h"

#include <map>
#include <mutex>

namespace kyubu {

constexpr int32_t kLoaderPriority = -1000000;   // loader-internal hooks sit closest to the original

enum class HookKind { Function, Import, VTable, Mid, Patch };
const char* hook_kind_name(HookKind k);

struct HookInfo {
    KyubuId id;
    HookKind kind;
    std::string owner;
    uintptr_t address;       // target function, slot address, or site
    std::string description;
    int32_t priority;
};

// Callback used by mid hooks (after the loader's dispatcher has set up guard/side/timing).
using MidCallback = std::function<void(KyubuCpuContext*)>;

class Hooks {
public:
    static Hooks& get();
    bool init();

    // Orders owners: lower value = earlier in load order. Loader sets this from the mod manager.
    std::function<int(const std::string&)> owner_order;
    // Wraps each mid-hook invocation (guard, side, timing). Receives owner and the call.
    std::function<void(const std::string& owner, KyubuSide side, const std::function<void()>&)> invoke_wrapper;

    KyubuResult hook_function(const std::string& owner, uintptr_t target, void* handler, int32_t priority,
                              void** next_slot, KyubuId* out_id, std::string* err = nullptr);
    KyubuResult hook_import(const std::string& owner, HMODULE module, const std::string& dll,
                            const std::string& function, void* handler, int32_t priority, void** next_slot,
                            KyubuId* out_id, std::string* err = nullptr);
    KyubuResult hook_vtable(const std::string& owner, void* object, uint32_t index, void* handler, int32_t priority,
                            void** next_slot, KyubuId* out_id, std::string* err = nullptr);
    KyubuResult hook_mid(const std::string& owner, uintptr_t site, KyubuSide side, MidCallback cb, KyubuId* out_id,
                         std::string* err = nullptr);
    KyubuResult patch(const std::string& owner, uintptr_t address, const void* bytes, uint32_t len, KyubuId* out_id,
                      std::string* err = nullptr);

    KyubuResult remove(const std::string& owner, KyubuId id);   // owner "" = any
    void remove_all(const std::string& owner);

    std::vector<HookInfo> list(const std::string& owner = "");
    size_t count(const std::string& owner);

    // Re-applies vtable hooks whose slot was overwritten behind our back. On Windows, d3d9 rewrites a device's
    // Present/Reset slots during Reset(). A new value from the same module as the hooked original is adopted
    // as the new original and the chain is re-linked; a value from another module (an overlay or another
    // hooking tool) is left alone and reported once. Returns the number of slots repaired.
    int repair_vtables();

    // Finds the import address table slot of dll!function in `module` (function may be "#123" for ordinals).
    static void** find_iat_slot(HMODULE module, const std::string& dll, const std::string& function);

private:
    struct Node {
        KyubuId id;
        std::string owner;
        void* handler;
        int32_t priority;
        uint64_t seq;
        void** next_slot;
    };
    struct Chain {                // one per hooked target (function, IAT slot, vtable slot)
        HookKind kind;
        uintptr_t key;            // target address / slot address
        void* original = nullptr; // trampoline or original pointer
        void** entry = nullptr;   // where the head pointer lives (thunk variable or the slot itself)
        uint8_t* thunk = nullptr; // function hooks: jmp [entry]
        std::vector<Node> nodes;
        std::string description;
        bool foreign_reported = false;   // vtable slot taken over by another module (reported once)
    };
public:
    struct MidEntry { KyubuId id; std::string owner; KyubuSide side; MidCallback cb; uint64_t seq; };
    struct MidSite {
        uintptr_t site = 0;
        uint32_t len = 0;
        uint8_t* stub = nullptr;
        uint8_t saved[16];
        std::vector<MidEntry> entries;
    };
private:
    struct Range { uintptr_t start, end; std::string owner; KyubuId id; };
    struct PatchRec { KyubuId id; std::string owner; uintptr_t address; std::vector<uint8_t> original; };

    void relink(Chain& c);
    bool sort_less(const Node& a, const Node& b);
    std::string range_conflict(uintptr_t start, uintptr_t end, const std::string& owner, KyubuId* conflict_id = nullptr);
    uint8_t* alloc_exec(size_t n);
    friend void mid_dispatch_impl(MidSite* s, KyubuCpuContext* ctx);

    std::recursive_mutex mu_;
    bool mh_ready_ = false;
    KyubuId next_id_ = 1;
    uint64_t seq_ = 0;
    std::map<uintptr_t, Chain> chains_;      // key: target or slot
    std::map<KyubuId, uintptr_t> node_chain_;
    std::map<uintptr_t, MidSite*> mids_;
    std::map<KyubuId, uintptr_t> mid_ids_;
    std::vector<Range> ranges_;
    std::map<KyubuId, PatchRec> patches_;
    uint8_t* exec_pool_ = nullptr;
    size_t exec_used_ = 0, exec_cap_ = 0;
};

// Suspends every other thread of the process for the lifetime of the object.
class ThreadFreezer {
public:
    ThreadFreezer();
    ~ThreadFreezer();
private:
    std::vector<HANDLE> threads_;
};

}  // namespace kyubu
