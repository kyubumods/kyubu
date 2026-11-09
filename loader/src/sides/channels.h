// Message channels between logical sides. Single-player uses an in-process loopback that copies
// payloads and delivers them on the receiving side's next update, exactly like a network would.
#pragma once
#include "common.h"

#include <deque>
#include <map>
#include <mutex>

namespace kyubu {

struct Mod;

class Channels {
public:
    static Channels& get();

    KyubuResult reg(Mod* mod, KyubuSide side, const std::string& name, KyubuChannelFn fn, void* user);
    KyubuResult send(Mod* mod, KyubuSide from, const std::string& name, uint32_t connection, const void* data, uint32_t len);
    // A message from the other end of a joined game (net_transport.cpp, any thread): delivered at `side`'s next update.
    void deliver(KyubuSide side, std::string name, uint32_t from_conn, std::vector<uint8_t> data);
    // Delivers messages queued for `side` (called once per update of that side).
    void pump(KyubuSide side);
    void remove_mod(Mod* mod);
    void unreg(KyubuSide side, const std::string& name);

    // Vanilla-server check of requires_server mods: no meta port yet, so a server is never known to be Kyubu before the join.
    bool remote_supports_channels() const { return false; }
    uint64_t sent(const std::string& name) const;
    uint64_t delivered(const std::string& name) const;

private:
    struct Handler { Mod* mod; KyubuChannelFn fn; void* user; };
    struct Msg { std::string name; uint32_t from_conn; std::vector<uint8_t> data; };
    mutable std::recursive_mutex mu_;
    std::map<std::string, Handler> handlers_[3];   // index by side
    std::deque<Msg> queue_[3];
    std::map<std::string, uint64_t> sent_, delivered_;
};

// JSON description of this session: loader/build, mods and registry hashes.
std::string session_manifest_json();

// Named interfaces published by mods.
class Interfaces {
public:
    static Interfaces& get();
    KyubuResult publish(Mod* mod, const std::string& name, uint32_t version, void* iface);
    KyubuResult get_iface(const std::string& name, uint32_t min_version, void** out, uint32_t* out_version);
    void remove_mod(Mod* mod);
    void remove(const std::string& name);
private:
    struct Entry { Mod* mod; uint32_t version; void* ptr; };
    std::recursive_mutex mu_;
    std::map<std::string, Entry> map_;
};

}  // namespace kyubu
