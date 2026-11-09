// Typed publish/subscribe bus. Handlers run in descending priority, then mod load order.
#pragma once
#include "common.h"

#include <map>
#include <mutex>

namespace kyubu {

struct Mod;

class Events {
public:
    static Events& get();

    using InternalFn = std::function<void(KyubuEvent*)>;

    KyubuResult subscribe(Mod* mod, const std::string& name, KyubuSide side, int32_t priority, KyubuEventFn fn,
                          void* user, KyubuId* out);
    KyubuId subscribe_internal(const std::string& name, KyubuSide side, int32_t priority, InternalFn fn);
    KyubuResult unsubscribe(Mod* mod, KyubuId id);
    void unsubscribe_all(Mod* mod);

    // Emits to handlers of `side`. Returns true when a handler cancelled a cancellable event.
    bool emit(const std::string& name, KyubuSide side, void* data = nullptr, uint32_t size = 0, bool cancellable = false,
              bool network_thread = false);
    // Emits to both sides that are currently active (client first).
    bool emit_active(const std::string& name, void* data = nullptr, uint32_t size = 0, bool cancellable = false);

    bool has_subscribers(const std::string& name, KyubuSide side);
    std::vector<std::string> names_seen();   // every event name emitted so far (for docs checks)
    std::vector<std::pair<std::string, uint64_t>> counts();

    // Events that are known to be unavailable on this build (subscribe returns KY_E_UNAVAILABLE).
    void set_unavailable(const std::string& name, const std::string& why);

private:
    struct Sub {
        KyubuId id;
        Mod* mod;             // nullptr = loader internal
        KyubuSide side;
        int32_t priority;
        uint64_t seq;
        KyubuEventFn fn = nullptr;
        void* user = nullptr;
        InternalFn internal;
        bool removed = false;
    };
    std::recursive_mutex mu_;
    std::map<std::string, std::vector<std::shared_ptr<Sub>>> subs_;
    std::map<std::string, std::string> unavailable_;
    std::map<std::string, uint64_t> seen_;
    KyubuId next_ = 1;
    uint64_t seq_ = 0;
};

bool side_active(KyubuSide s);

}  // namespace kyubu
