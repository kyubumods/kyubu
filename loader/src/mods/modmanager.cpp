#include "modmanager.h"

#include "api.h"
#include "guard.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "modconfig.h"
#include "symbols.h"

namespace kyubu {

const char* mod_state_name(ModState s) {
    switch (s) {
        case ModState::Loaded: return "loaded";
        case ModState::Disabled: return "disabled";
        case ModState::Failed: return "failed";
        case ModState::Faulted: return "faulted";
        case ModState::SkippedSide: return "skipped";
        case ModState::New: return "new";
        case ModState::Missing: return "missing";
    }
    return "?";
}

static DWORD g_call_tls = TLS_OUT_OF_INDEXES;
CallFrame* current_call() {
    if (g_call_tls == TLS_OUT_OF_INDEXES) return nullptr;
    return (CallFrame*)TlsGetValue(g_call_tls);
}
KyubuSide current_side_tls() { auto c = current_call(); return c ? c->side : KY_SIDE_NONE; }
Mod* current_mod_tls() { auto c = current_call(); return c ? c->mod : nullptr; }

ModManager& ModManager::get() {
    static ModManager* m = new ModManager();
    return *m;
}

uint32_t ModManager::host_sides() const {
    switch (L().host.host) {
        case KY_HOST_CLIENT: return KY_SIDE_CLIENT | KY_SIDE_SERVER;   // single-player hosts both
        case KY_HOST_SERVER: return KY_SIDE_SERVER;
        default: return 0;
    }
}

void ModManager::discover_and_resolve() {
    Loader& l = L();
    if (g_call_tls == TLS_OUT_OF_INDEXES) g_call_tls = TlsAlloc();
    make_dirs(l.mods_dir);
    candidates_.clear();
    for (auto& name : list_dir(l.mods_dir)) {
        std::string p = join_path(l.mods_dir, name);
        bool is_dir = dir_exists(p);
        if (!is_dir && !ends_with(lower(name), ".zip")) {
            KWARN("mods/%s ignored: not a mod folder or .zip", name.c_str());
            continue;
        }
        auto pkg = Package::open(p);
        if (!pkg || !pkg->exists("mod.toml")) {
            KWARN("mods/%s ignored: no mod.toml at its root", name.c_str());
            continue;
        }
        Candidate c;
        c.path = p;
        auto text = pkg->read("mod.toml");
        std::string src = "mods/" + name + "/mod.toml";
        c.manifest = Manifest::parse(std::string(text->begin(), text->end()), src, c.errors);
        candidates_.push_back(std::move(c));
    }
    KINFO("Discovered %u mod package(s) in %s", (unsigned)candidates_.size(), l.mods_dir.c_str());

    Version api{KYUBU_API_VERSION_MAJOR, KYUBU_API_VERSION_MINOR, 0, ""};
    auto res = resolve_mods(candidates_, [&](const std::string& id) { return l.safe_mode ? false : l.config.is_enabled(id); },
                            host_sides(), api);
    mods_.clear();
    for (size_t i = 0; i < res.mods.size(); ++i) {
        auto& r = res.mods[i];
        auto m = std::make_unique<Mod>();
        m->id = r.id.empty() ? file_name(r.path) : r.id;
        m->path = r.path;
        if (r.manifest) m->manifest = *r.manifest;
        else m->manifest_ok = false;
        m->reason = r.reason;
        switch (r.status) {
            case ResolveStatus::Load: m->state = ModState::Loaded; break;
            case ResolveStatus::Disabled: m->state = ModState::Disabled; break;
            case ResolveStatus::Failed: m->state = ModState::Failed; break;
            case ResolveStatus::SkippedSide: m->state = ModState::SkippedSide; break;
        }
        if (l.safe_mode && r.status == ResolveStatus::Disabled) m->reason = "skipped (safe mode)";
        for (int s = 1; s <= 2; ++s) {
            m->ctx[s].mod = m.get();
            m->ctx[s].side = (KyubuSide)s;
        }
        mods_.push_back(std::move(m));
    }
    order_.clear();
    for (size_t i = 0; i < res.order.size(); ++i) order_[res.order[i]] = (int)i;
    for (auto& m : mods_) {
        auto it = order_.find(m->id);
        m->order = it == order_.end() ? 1000000 : it->second;
        if (m->state == ModState::Failed) KERROR("mod %s: not loaded: %s", m->id.c_str(), m->reason.c_str());
        else if (m->state == ModState::Disabled) KINFO("mod %s: %s", m->id.c_str(), m->reason.c_str());
        else if (m->state == ModState::SkippedSide) KDEBUG("mod %s: skipped, %s", m->id.c_str(), m->reason.c_str());
    }
    std::string ord;
    for (auto& id : res.order) ord += (ord.empty() ? "" : ", ") + id;
    KINFO("Load order: %s", ord.empty() ? "(no mods)" : ord.c_str());
}

void ModManager::load_native() {
    Loader& l = L();
    for (Mod* m : ordered()) {
        m->pkg = Package::open(m->path);
        m->data_dir = join_path(join_path(l.kyubu_dir, "data"), m->id);
        m->cache_dir = join_path(join_path(l.kyubu_dir, "cache"), m->id + "-" + m->manifest.version.str());
        if (!m->pkg) { mark_faulted(m, "package could not be opened"); m->state = ModState::Failed; continue; }
        // the mod's options, read before any of its entry points runs (docs/config.md)
        std::string config_error;
        if (!config_attach(m, join_path(l.kyubu_dir, "config"), &config_error)) {
            m->state = ModState::Failed;
            m->reason = config_error;
            Log::get().write(KY_LOG_ERROR, m->id, KY_SIDE_NONE, "not loaded: " + m->reason);
            continue;
        }
        // symbol maps shipped by the mod
        for (auto& rel : m->pkg->list(m->manifest.symbols)) {
            if (!ends_with(lower(rel), ".toml")) continue;
            auto text = m->pkg->read(rel);
            std::vector<std::string> errors;
            int n = SymbolTable::get().load_text(std::string(text->begin(), text->end()), m->id + ":" + rel,
                                                 l.host.build_id, m->id, errors);
            for (auto& e : errors) Log::get().write(KY_LOG_ERROR, m->id, KY_SIDE_NONE, "symbol map: " + e);
            if (n) Log::get().write(KY_LOG_INFO, m->id, KY_SIDE_NONE, fmt("loaded %d symbol(s) from %s", n, rel.c_str()));
        }
        // native entry points: common (all present sides), client, server
        for (const char* key : {"common", "client", "server"}) {
            auto it = m->manifest.native.find(key);
            if (it == m->manifest.native.end()) continue;
            std::vector<KyubuSide> sides;
            if (!strcmp(key, "common")) {
                for (KyubuSide s : {KY_SIDE_CLIENT, KY_SIDE_SERVER})
                    if (m->has_side(s) && (host_sides() & s)) sides.push_back(s);
            } else {
                KyubuSide s = !strcmp(key, "client") ? KY_SIDE_CLIENT : KY_SIDE_SERVER;
                if (host_sides() & s) sides.push_back(s);
            }
            if (sides.empty()) continue;
            auto file = m->pkg->materialize(it->second, m->cache_dir);
            if (!file) { m->state = ModState::Failed; m->reason = "native." + std::string(key) + ": file not found: " + it->second; break; }
            HMODULE h = LoadLibraryExW(widen(*file).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!h) {
                m->state = ModState::Failed;
                m->reason = fmt("native.%s: LoadLibrary failed (error %lu) for %s", key, GetLastError(), file->c_str());
                break;
            }
            m->native[key] = h;
            if (auto nt = nt_headers((uintptr_t)h)) m->code_ranges.push_back({(uintptr_t)h, (uintptr_t)h + nt->OptionalHeader.SizeOfImage});
            auto query = (KyubuModQueryFn)GetProcAddress(h, "kyubu_mod_query");
            auto init = (KyubuModInitFn)GetProcAddress(h, "kyubu_mod_init");
            auto shut = (KyubuModShutdownFn)GetProcAddress(h, "kyubu_mod_shutdown");
            if (!query || !init) { m->state = ModState::Failed; m->reason = fmt("native.%s: missing export kyubu_mod_query or kyubu_mod_init", key); break; }
            KyubuModQuery q{};
            q.size = sizeof q;
            int qr = 0;
            if (!invoke(m, KY_SIDE_NONE, "kyubu_mod_query", [&] { qr = query(&q); })) break;
            q.id[sizeof q.id - 1] = 0;
            if (qr != 0) { m->state = ModState::Failed; m->reason = fmt("kyubu_mod_query returned %d", qr); break; }
            if (m->id != q.id) {
                m->state = ModState::Failed;
                m->reason = fmt("native.%s reports id '%s' but mod.toml says '%s'", key, q.id, m->id.c_str());
                break;
            }
            if (!api_version_supported(q.api_version)) {
                m->state = ModState::Failed;
                m->reason = fmt("native.%s was built for API %u.%u; loader implements %d.%d", key, q.api_version >> 16,
                                q.api_version & 0xFFFF, KYUBU_API_VERSION_MAJOR, KYUBU_API_VERSION_MINOR);
                break;
            }
            for (KyubuSide s : sides) {
                int ir = 0;
                if (!invoke(m, s, "kyubu_mod_init", [&] { ir = init(api_table(), m->handle(s), s); })) break;
                if (ir != 0) { m->state = ModState::Failed; m->reason = fmt("kyubu_mod_init(%s) returned %d", side_name(s), ir); break; }
                if (shut) { m->shutdown_fns.push_back(shut); m->shutdown_sides.push_back(s); }
            }
            if (m->state != ModState::Loaded) break;
        }
        if (m->state == ModState::Failed) {
            Log::get().write(KY_LOG_ERROR, m->id, KY_SIDE_NONE, "not loaded: " + m->reason);
            Hooks::get().remove_all(m->id);
        } else if (m->state == ModState::Loaded) {
            Log::get().write(KY_LOG_INFO, m->id, KY_SIDE_NONE,
                             "loaded " + m->manifest.name + " " + m->manifest.version.str());
        }
    }
}

Mod* ModManager::find(const std::string& id) {
    for (auto& m : mods_) if (m->id == id && m->state != ModState::Failed) return m.get();
    for (auto& m : mods_) if (m->id == id) return m.get();
    return nullptr;
}

Mod* ModManager::from_handle(KyubuModHandle h, KyubuSide* side) {
    auto c = (ModContext*)h;
    if (!c) return nullptr;
    ModContext probe;
    if (!safe_read((uintptr_t)c, &probe, sizeof probe) || probe.magic != 0x4B594D43) return nullptr;
    if (side) *side = c->side;
    return c->mod;
}

std::vector<Mod*> ModManager::ordered() {
    std::vector<Mod*> v;
    for (auto& m : mods_) if (m->state == ModState::Loaded || m->state == ModState::Faulted) v.push_back(m.get());
    std::sort(v.begin(), v.end(), [](Mod* a, Mod* b) { return a->order < b->order; });
    return v;
}

int ModManager::order_of(const std::string& owner) const {
    auto it = order_.find(owner);
    return it == order_.end() ? (owner == "kyubu" ? 1 << 30 : 1 << 29) : it->second;
}

std::string ModManager::mod_for_address(uintptr_t a) {
    for (auto& m : mods_)
        for (auto& r : m->code_ranges)
            if (a >= r.first && a < r.second) return m->id;
    return {};
}

bool ModManager::invoke(Mod* mod, KyubuSide side, const char* what, const std::function<void()>& fn) {
    if (mod && mod->state != ModState::Loaded) return false;
    CallFrame f{mod, side, current_call()};
    if (g_call_tls == TLS_OUT_OF_INDEXES) g_call_tls = TlsAlloc();
    TlsSetValue(g_call_tls, &f);
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    GuardFrame gf;
    DWORD code = guarded_call_raw([](void* p) { (*(const std::function<void()>*)p)(); }, (void*)&fn,
                                  mod ? mod->id.c_str() : "kyubu", &gf);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    TlsSetValue(g_call_tls, f.prev);
    if (mod) {
        double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(fq.QuadPart);
        mod->ms_total += ms;
        mod->ms_frame += ms;
        ++mod->calls;
    }
    if (code) {
        std::string why = fmt("%s in %s at %s", exception_name(code), what, describe_address(gf.fault_address).c_str());
        if (mod) mark_faulted(mod, why, side);
        else KERROR("loader callback faulted: %s", why.c_str());
        return false;
    }
    return true;
}

void ModManager::mark_faulted(Mod* mod, const std::string& why, KyubuSide side) {
    if (mod->state == ModState::Faulted) return;
    mod->state = ModState::Faulted;
    mod->reason = why;
    Log::get().write(KY_LOG_ERROR, mod->id, side != KY_SIDE_NONE ? side : current_side_tls(), "FAULTED and disabled: " + why);
    Hooks::get().remove_all(mod->id);   // stop the game from calling into it
}

void ModManager::frame_stats_roll() {
    for (auto& m : mods_) { m->ms_last_frame = m->ms_frame; m->ms_frame = 0; }
}

std::vector<std::string> ModManager::summary_lines() {
    std::vector<std::string> out;
    for (auto& m : mods_) {
        std::string sides;
        if (m->manifest.sides & KY_SIDE_CLIENT) sides += "client";
        if (m->manifest.sides & KY_SIDE_SERVER) sides += sides.empty() ? "server" : "+server";
        out.push_back(fmt("%-20s %-10s %-14s %-9s %s", m->id.c_str(), m->manifest.version.str().c_str(), sides.c_str(),
                          mod_state_name(m->state), m->reason.c_str()));
    }
    return out;
}

}  // namespace kyubu
