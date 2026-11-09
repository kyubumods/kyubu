#include "channels.h"

#include "animations.h"
#include "events.h"
#include "json.hpp"
#include "loader.h"
#include "log.h"
#include "modmanager.h"
#include "net_transport.h"
#include "registry.h"

namespace kyubu {

Channels& Channels::get() {
    static Channels* c = new Channels();
    return *c;
}

static bool owns_name(Mod* mod, const std::string& name) {
    auto colon = name.find(':');
    return mod && colon != std::string::npos && name.substr(0, colon) == mod->id && colon + 1 < name.size();
}

KyubuResult Channels::reg(Mod* mod, KyubuSide side, const std::string& name, KyubuChannelFn fn, void* user) {
    if (!fn || (side != KY_SIDE_CLIENT && side != KY_SIDE_SERVER)) return KY_E_INVALID_ARG;
    if (!owns_name(mod, name)) return KY_E_NAMESPACE;
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (handlers_[side].count(name)) return KY_E_DUPLICATE;
    handlers_[side][name] = {mod, fn, user};
    return KY_OK;
}

KyubuResult Channels::send(Mod* mod, KyubuSide from, const std::string& name, uint32_t conn, const void* data,
                           uint32_t len) {
    if (from != KY_SIDE_CLIENT && from != KY_SIDE_SERVER) return KY_E_SIDE;
    if (len > KY_CHANNEL_MAX_PAYLOAD) return KY_E_TOO_LARGE;
    if (len && !data) return KY_E_INVALID_ARG;
    if (name.find(':') == std::string::npos) return KY_E_INVALID_ARG;
    (void)mod;
    KyubuSide to = from == KY_SIDE_CLIENT ? KY_SIDE_SERVER : KY_SIDE_CLIENT;
    Loader& l = L();
    // Loopback exists only when this process hosts both sides (single-player / menu).
    bool loopback = l.host.host == KY_HOST_CLIENT && l.mode == KY_MODE_MENU_OR_SINGLEPLAYER;
    // Direction rules are side checks: only the server may address clients, only clients the server.
    if (from == KY_SIDE_CLIENT && conn != KY_CONN_SERVER) return KY_E_SIDE;
    if (from == KY_SIDE_SERVER && conn == KY_CONN_SERVER) return KY_E_SIDE;
    if (!loopback) return NetTransport::get().send(from, name, conn, data, len);   // a joined game (D10)
    if (from == KY_SIDE_SERVER && conn != KY_CONN_LOCAL_CLIENT && conn != KY_CONN_ALL_CLIENTS) return KY_E_NOT_FOUND;
    std::lock_guard<std::recursive_mutex> g(mu_);
    Msg m;
    m.name = name;
    m.from_conn = from == KY_SIDE_CLIENT ? KY_CONN_LOCAL_CLIENT : KY_CONN_SERVER;
    if (len) m.data.assign((const uint8_t*)data, (const uint8_t*)data + len);   // copy: sender may reuse its buffer
    queue_[to].push_back(std::move(m));
    ++sent_[name];
    return KY_OK;
}

void Channels::deliver(KyubuSide side, std::string name, uint32_t from_conn, std::vector<uint8_t> data) {
    if (side != KY_SIDE_CLIENT && side != KY_SIDE_SERVER) return;
    std::lock_guard<std::recursive_mutex> g(mu_);
    Msg m;
    m.name = std::move(name);
    m.from_conn = from_conn;
    m.data = std::move(data);
    queue_[side].push_back(std::move(m));
}

void Channels::pump(KyubuSide side) {
    std::deque<Msg> batch;
    {
        std::lock_guard<std::recursive_mutex> g(mu_);
        batch.swap(queue_[side]);   // messages sent while delivering wait for the next update
    }
    for (auto& m : batch) {
        Handler h;
        {
            std::lock_guard<std::recursive_mutex> g(mu_);
            auto it = handlers_[side].find(m.name);
            if (it == handlers_[side].end()) {
                KDEBUG("channel %s: no %s handler, message dropped", m.name.c_str(), side_name(side));
                continue;
            }
            h = it->second;
            ++delivered_[m.name];
        }
        ModManager::get().invoke(h.mod, side, "channel handler", [&] {
            h.fn(m.from_conn, m.data.empty() ? nullptr : m.data.data(), (uint32_t)m.data.size(), h.user);
        });
    }
}

void Channels::remove_mod(Mod* mod) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (auto& hs : handlers_)
        for (auto it = hs.begin(); it != hs.end();) it = it->second.mod == mod ? hs.erase(it) : std::next(it);
}

void Channels::unreg(KyubuSide side, const std::string& name) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    handlers_[side].erase(name);
}

uint64_t Channels::sent(const std::string& n) const {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = sent_.find(n);
    return it == sent_.end() ? 0 : it->second;
}

uint64_t Channels::delivered(const std::string& n) const {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = delivered_.find(n);
    return it == delivered_.end() ? 0 : it->second;
}

std::string session_manifest_json() {
    Loader& l = L();
    nlohmann::json j;
    j["protocol"] = 1;
    j["loader_api"] = fmt("%d.%d", KYUBU_API_VERSION_MAJOR, KYUBU_API_VERSION_MINOR);
    j["build"] = l.host.build_id;
    j["host"] = l.host.host == KY_HOST_SERVER ? "server" : "client";
    j["mods"] = nlohmann::json::array();
    for (Mod* m : ModManager::get().ordered()) {
        nlohmann::json s = nlohmann::json::array();
        if (m->manifest.sides & KY_SIDE_CLIENT) s.push_back("client");
        if (m->manifest.sides & KY_SIDE_SERVER) s.push_back("server");
        j["mods"].push_back({{"id", m->id}, {"version", m->manifest.version.str()}, {"sides", s},
                             {"requires_server", m->manifest.requires_server}});
    }
    for (auto& n : Registries::get().names()) {
        std::string h;
        Registries::get().hash(n, &h);
        j["registries"][n] = h;
    }
    j["animations"] = Animations::get().hash();   // the clip set: string ids only, no numbers
    return j.dump();
}

Interfaces& Interfaces::get() {
    static Interfaces* i = new Interfaces();
    return *i;
}

KyubuResult Interfaces::publish(Mod* mod, const std::string& name, uint32_t version, void* iface) {
    if (!iface) return KY_E_INVALID_ARG;
    if (!owns_name(mod, name)) return KY_E_NAMESPACE;
    std::lock_guard<std::recursive_mutex> g(mu_);
    map_[name] = {mod, version, iface};
    return KY_OK;
}

KyubuResult Interfaces::get_iface(const std::string& name, uint32_t min_version, void** out, uint32_t* out_version) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = map_.find(name);
    if (it == map_.end() || !it->second.mod->active()) return KY_E_NOT_FOUND;
    if (out_version) *out_version = it->second.version;
    if (it->second.version < min_version) return KY_E_VERSION;
    if (out) *out = it->second.ptr;
    return KY_OK;
}

void Interfaces::remove(const std::string& name) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    map_.erase(name);
}

void Interfaces::remove_mod(Mod* mod) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (auto it = map_.begin(); it != map_.end();) it = it->second.mod == mod ? map_.erase(it) : std::next(it);
}

}  // namespace kyubu
