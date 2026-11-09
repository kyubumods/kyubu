#include "events.h"

#include <algorithm>

#include "loader.h"
#include "log.h"
#include "modmanager.h"

namespace kyubu {

bool side_active(KyubuSide s) {
    Loader& l = L();
    if (s == KY_SIDE_CLIENT) return l.host.host == KY_HOST_CLIENT;
    if (s == KY_SIDE_SERVER)
        return l.host.host == KY_HOST_SERVER || (l.host.host == KY_HOST_CLIENT && l.mode == KY_MODE_MENU_OR_SINGLEPLAYER);
    return false;
}

Events& Events::get() {
    static Events* e = new Events();
    return *e;
}

void Events::set_unavailable(const std::string& name, const std::string& why) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    unavailable_[name] = why;
}

KyubuResult Events::subscribe(Mod* mod, const std::string& name, KyubuSide side, int32_t priority, KyubuEventFn fn,
                              void* user, KyubuId* out) {
    if (!fn || name.empty() || (side != KY_SIDE_CLIENT && side != KY_SIDE_SERVER)) return KY_E_INVALID_ARG;
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (auto it = unavailable_.find(name); it != unavailable_.end()) {
        if (mod) Log::get().write(KY_LOG_WARN, mod->id, side, "cannot subscribe to " + name + ": " + it->second);
        return KY_E_UNAVAILABLE;
    }
    auto s = std::make_shared<Sub>();
    s->id = next_++;
    s->mod = mod;
    s->side = side;
    s->priority = priority;
    s->seq = seq_++;
    s->fn = fn;
    s->user = user;
    subs_[name].push_back(s);
    if (out) *out = s->id;
    return KY_OK;
}

KyubuId Events::subscribe_internal(const std::string& name, KyubuSide side, int32_t priority, InternalFn fn) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto s = std::make_shared<Sub>();
    s->id = next_++;
    s->mod = nullptr;
    s->side = side;
    s->priority = priority;
    s->seq = seq_++;
    s->internal = std::move(fn);
    subs_[name].push_back(s);
    return s->id;
}

KyubuResult Events::unsubscribe(Mod* mod, KyubuId id) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (auto& [n, v] : subs_)
        for (auto it = v.begin(); it != v.end(); ++it)
            if ((*it)->id == id && (!mod || (*it)->mod == mod)) {
                (*it)->removed = true;
                v.erase(it);
                return KY_OK;
            }
    return KY_E_NOT_FOUND;
}

void Events::unsubscribe_all(Mod* mod) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (auto& [n, v] : subs_) {
        for (auto& s : v) if (s->mod == mod) s->removed = true;
        v.erase(std::remove_if(v.begin(), v.end(), [&](auto& s) { return s->mod == mod; }), v.end());
    }
}

bool Events::has_subscribers(const std::string& name, KyubuSide side) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = subs_.find(name);
    if (it == subs_.end()) return false;
    for (auto& s : it->second) if (s->side == side) return true;
    return false;
}

std::vector<std::string> Events::names_seen() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<std::string> v;
    for (auto& [k, b] : seen_) v.push_back(k);
    return v;
}

std::vector<std::pair<std::string, uint64_t>> Events::counts() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    return std::vector<std::pair<std::string, uint64_t>>(seen_.begin(), seen_.end());
}

bool Events::emit(const std::string& name, KyubuSide side, void* data, uint32_t size, bool cancellable,
                  bool network_thread) {
    std::vector<std::shared_ptr<Sub>> list;
    {
        std::lock_guard<std::recursive_mutex> g(mu_);
        ++seen_[name];
        auto it = subs_.find(name);
        if (it == subs_.end()) return false;
        for (auto& s : it->second) if (s->side == side) list.push_back(s);
    }
    if (list.empty()) return false;
    auto& mm = ModManager::get();
    std::stable_sort(list.begin(), list.end(), [&](const std::shared_ptr<Sub>& a, const std::shared_ptr<Sub>& b) {
        if (a->priority != b->priority) return a->priority > b->priority;
        int oa = a->mod ? a->mod->order : -1, ob = b->mod ? b->mod->order : -1;
        if (oa != ob) return oa < ob;
        return a->seq < b->seq;
    });
    KyubuEvent ev{};
    ev.size = sizeof ev;
    ev.name = name.c_str();
    ev.side = side;
    ev.data = data;
    ev.data_size = size;
    ev.cancellable = cancellable ? 1 : 0;
    ev.network_thread = network_thread ? 1 : 0;
    for (auto& s : list) {
        if (s->removed) continue;
        if (s->mod) {
            mm.invoke(s->mod, side, name.c_str(), [&] { s->fn(&ev, s->user); });
        } else {
            mm.invoke(nullptr, side, name.c_str(), [&] { s->internal(&ev); });
        }
        if (cancellable && ev.cancelled) return true;
        ev.cancelled = 0;
    }
    return false;
}

bool Events::emit_active(const std::string& name, void* data, uint32_t size, bool cancellable) {
    bool c = false;
    if (side_active(KY_SIDE_CLIENT)) c |= emit(name, KY_SIDE_CLIENT, data, size, cancellable);
    if (side_active(KY_SIDE_SERVER)) c |= emit(name, KY_SIDE_SERVER, data, size, cancellable);
    return c;
}

}  // namespace kyubu
