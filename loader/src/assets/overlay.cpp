#include "overlay.h"

#include <algorithm>

#include "events.h"
#include "formats.h"
#include "gametypes.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "modmanager.h"
#include "registry.h"
#include "symbols.h"
#include "json.hpp"

namespace kyubu {

Assets& Assets::get() {
    static Assets* a = new Assets();
    return *a;
}

std::string Assets::db_for_category(const std::string& c) {
    if (c == "models") return "data1.db";
    if (c == "sounds") return "data2.db";
    if (c == "icons") return "data3.db";
    if (c == "text") return "data4.db";
    return {};
}

std::string Assets::category_for_db(const std::string& db) {
    if (db == "data1.db") return "models";
    if (db == "data2.db") return "sounds";
    if (db == "data3.db") return "icons";
    if (db == "data4.db") return "text";
    return {};
}

static std::string key_for_file(const std::string& cat, const std::string& file) {
    std::string f = lower(file);
    if (cat == "models") {
        if (ends_with(f, ".vox") || ends_with(f, ".cub")) return file.substr(0, file.size() - 4) + ".cub";
        return {};
    }
    if (cat == "sounds") return ends_with(f, ".wav") ? file : std::string();
    if (cat == "icons") return ends_with(f, ".png") ? file : std::string();
    if (cat == "text") return (f == "dict_en.xml" || f == "dict_de.xml") ? f : std::string();
    return {};
}

void Assets::build_index() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    index_.clear();
    Loader& l = L();
    auto add = [&](const std::string& cat, const std::string& file, Source s) {
        std::string key = key_for_file(cat, file);
        if (key.empty()) {
            Log::get().write(KY_LOG_WARN, s.layer, KY_SIDE_NONE, "asset " + cat + "/" + file + " ignored: unsupported file type");
            return;
        }
        index_[db_for_category(cat)][key].push_back(std::move(s));
    };
    if (l.dev_mode) {
        std::string dev = join_path(l.kyubu_dir, "dev-assets");
        for (const char* cat : {"models", "sounds", "icons", "text"}) {
            std::string dir = join_path(dev, cat);
            for (auto& f : list_dir(dir)) {
                Source s;
                s.layer = "dev-assets";
                s.disk = join_path(dir, f);
                s.mtime = file_mtime(s.disk);
                add(cat, f, s);
            }
        }
    }
    auto mods = ModManager::get().ordered();
    std::reverse(mods.begin(), mods.end());   // later mods win
    for (Mod* m : mods) {
        if (!m->active() || !m->pkg) continue;
        std::string root = normalize_rel(m->manifest.assets);
        for (auto& rel : m->pkg->list(root)) {
            std::string sub = rel.substr(root.size() + (root.empty() ? 0 : 1));
            auto slash = sub.find('/');
            if (slash == std::string::npos) continue;
            std::string cat = sub.substr(0, slash), file = sub.substr(slash + 1);
            if (file.find('/') != std::string::npos || db_for_category(cat).empty()) continue;
            Source s;
            s.layer = m->id;
            s.mod = m;
            s.rel = rel;
            if (!m->pkg->is_zip()) s.disk = *m->pkg->materialize(rel, "");
            s.mtime = m->pkg->mtime(rel);
            add(cat, file, s);
        }
    }
    watched_.clear();
    for (auto& [db, keys] : index_)
        for (auto& [k, srcs] : keys)
            for (auto& s : srcs)
                if (!s.disk.empty()) watched_[s.disk] = s.mtime;
    size_t n = 0;
    for (auto& [db, keys] : index_) n += keys.size();
    KINFO("Asset overlay: %u overridden or added key(s)", (unsigned)n);
}

std::optional<std::vector<uint8_t>> Assets::load_source(const std::string& db, const std::string& key, const Source& s,
                                                         std::string* err) {
    std::optional<std::vector<uint8_t>> raw;
    std::string name;
    if (!s.disk.empty()) { raw = read_file(s.disk); name = s.disk; }
    else if (s.mod && s.mod->pkg) { raw = s.mod->pkg->read(s.rel); name = s.rel; }
    if (!raw) { *err = "cannot read " + name; return std::nullopt; }
    std::string cat = category_for_db(db);
    if (cat == "models") {
        std::string e;
        std::optional<VoxelModel> m = ends_with(lower(name), ".vox") ? parse_vox(*raw, &e) : parse_cub(*raw, &e);
        if (!m) { *err = name + ": " + e; return std::nullopt; }
        if (auto v = validate_model(*m); !v.empty()) { *err = name + ": " + v; return std::nullopt; }
        return write_cub(*m);
    }
    if (cat == "sounds") {
        if (auto v = validate_wav(*raw); !v.empty()) { *err = name + ": " + v; return std::nullopt; }
        return raw;
    }
    if (cat == "icons") {
        if (auto v = validate_png(*raw); !v.empty()) { *err = name + ": " + v; return std::nullopt; }
        return raw;
    }
    (void)key;
    return raw;
}

std::optional<ResolvedAsset> Assets::resolve_overlay(const std::string& db, const std::string& key,
                                                     std::vector<std::string>* shadowed) {
    std::vector<Source> srcs;
    std::vector<Provider> provs;
    {
        std::lock_guard<std::recursive_mutex> g(mu_);
        auto d = index_.find(db);
        if (d != index_.end()) {
            auto k = d->second.find(key);
            if (k != d->second.end()) srcs = k->second;
        }
        provs = providers_;
    }
    std::optional<ResolvedAsset> found;
    for (auto& s : srcs) {
        if (s.mod && !s.mod->active()) continue;
        if (found) { if (shadowed) shadowed->push_back(s.layer); continue; }
        std::string err;
        auto data = load_source(db, key, s, &err);
        if (!data) { Log::get().write(KY_LOG_ERROR, s.layer, KY_SIDE_NONE, "asset skipped: " + err); continue; }
        found = ResolvedAsset{std::move(*data), s.layer};
    }
    if (found) return found;
    for (auto it = provs.rbegin(); it != provs.rend(); ++it) {
        auto& p = *it;
        if (p.db != db || !glob_match(p.glob, key) || !p.mod->active()) continue;
        void* out = nullptr;
        uint32_t len = 0;
        KyubuResult r = KY_E_NOT_FOUND;
        ModManager::get().invoke(p.mod, current_side_tls(), "asset provider", [&] { r = p.fn(db.c_str(), key.c_str(), &out, &len, p.user); });
        if (r != KY_OK || !out) continue;
        std::vector<uint8_t> data((uint8_t*)out, (uint8_t*)out + len);
        HeapFree(GetProcessHeap(), 0, out);
        Source s;
        s.layer = "provider:" + p.mod->id;
        std::string err, cat = category_for_db(db);
        if (cat == "models") {
            auto m = parse_cub(data, &err);
            if (!m) { Log::get().write(KY_LOG_ERROR, p.mod->id, KY_SIDE_NONE, "provider returned an invalid model for " + key + ": " + err); continue; }
            if (auto v = validate_model(*m); !v.empty()) { Log::get().write(KY_LOG_ERROR, p.mod->id, KY_SIDE_NONE, "provider model " + key + ": " + v); continue; }
        } else if (cat == "sounds") {
            if (auto v = validate_wav(data); !v.empty()) { Log::get().write(KY_LOG_ERROR, p.mod->id, KY_SIDE_NONE, key + ": " + v); continue; }
        } else if (cat == "icons") {
            if (auto v = validate_png(data); !v.empty()) { Log::get().write(KY_LOG_ERROR, p.mod->id, KY_SIDE_NONE, key + ": " + v); continue; }
        }
        return ResolvedAsset{std::move(data), s.layer};
    }
    if (db == "data1.db" && key == "kyubu-missing.cub") return ResolvedAsset{write_cub(missing_model()), "kyubu"};
    return std::nullopt;
}

std::string Assets::merge_dictionaries(const std::string& key, const std::string& vanilla) {
    std::vector<Source> srcs;
    {
        std::lock_guard<std::recursive_mutex> g(mu_);
        auto d = index_.find("data4.db");
        if (d != index_.end()) {
            auto k = d->second.find(key);
            if (k != d->second.end()) srcs = k->second;
        }
    }
    std::reverse(srcs.begin(), srcs.end());   // lowest priority first, so later mods overwrite
    std::string text = vanilla;
    for (auto& s : srcs) {
        if (s.mod && !s.mod->active()) continue;
        std::string err;
        auto data = load_source("data4.db", key, s, &err);
        if (!data) continue;
        std::vector<std::string> rep, add;
        text = merge_dictionary(text, std::string(data->begin(), data->end()), &rep, &add);
        Log::get().write(KY_LOG_DEBUG, s.layer, KY_SIDE_NONE,
                         fmt("%s: %u entries replaced, %u added", key.c_str(), (unsigned)rep.size(), (unsigned)add.size()));
    }
    // Text registry entries become <speech key="ns:name"> entries of the matching language.
    std::string lang = key == "dict_de.xml" ? "de" : "en";
    auto& regs = Registries::get();
    std::lock_guard<std::recursive_mutex> g(regs.mutex());
    if (Registry* r = regs.get_registry("kyubu:text")) {
        std::string patch = "<root>\r\n";
        int n = 0;
        for (auto& e : r->entries) {
            auto j = nlohmann::json::parse(e.payload.begin(), e.payload.end(), nullptr, false);
            if (j.is_discarded() || !j.contains(lang)) continue;
            patch += "  <speech key=\"" + e.id + "\">\r\n    " + j[lang].get<std::string>() + "\r\n  </speech>\r\n";
            ++n;
        }
        patch += "</root>\r\n";
        if (n) text = merge_dictionary(text, patch, nullptr, nullptr);
    }
    return text;
}

std::optional<ResolvedAsset> Assets::resolve(const std::string& db, const std::string& key) {
    if (db == "data4.db" && is_dictionary(key)) {
        auto v = read_vanilla(db, key);
        if (!v) return std::nullopt;
        std::string merged = merge_dictionaries(key, std::string(v->begin(), v->end()));
        return ResolvedAsset{std::vector<uint8_t>(merged.begin(), merged.end()), "merged"};
    }
    if (auto o = resolve_overlay(db, key)) return o;
    auto v = read_vanilla(db, key);
    if (!v) return std::nullopt;
    return ResolvedAsset{std::move(*v), "vanilla"};
}

KyubuResult Assets::add_provider(Mod* mod, const std::string& db, const std::string& glob, KyubuAssetProviderFn fn,
                                 void* user, KyubuId* out) {
    if (category_for_db(db).empty() || glob.empty() || !fn) return KY_E_INVALID_ARG;
    std::lock_guard<std::recursive_mutex> g(mu_);
    Provider p{next_id_++, mod, db, glob, fn, user};
    providers_.push_back(p);
    if (out) *out = p.id;
    return KY_OK;
}

void Assets::remove_providers(Mod* mod) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    providers_.erase(std::remove_if(providers_.begin(), providers_.end(), [&](const Provider& p) { return p.mod == mod; }),
                     providers_.end());
}

void Assets::remove_provider(KyubuId id) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    providers_.erase(std::remove_if(providers_.begin(), providers_.end(), [&](const Provider& p) { return p.id == id; }),
                     providers_.end());
}

std::string Assets::served_by(const std::string& db, const std::string& key) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = served_.find(db + "/" + key);
    if (it != served_.end()) return it->second;
    auto d = index_.find(db);
    if (d != index_.end()) {
        auto k = d->second.find(key);
        if (k != d->second.end())
            for (auto& s : k->second) if (!s.mod || s.mod->active()) return s.layer;
    }
    // Not served yet and no file layer: the newest provider whose glob claims the key.
    for (auto it = providers_.rbegin(); it != providers_.rend(); ++it)
        if (it->db == db && it->mod->active() && glob_match(it->glob, key)) return "provider:" + it->mod->id;
    return "vanilla";
}

void Assets::record_served(const std::string& db, const std::string& key, const std::string& layer) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    served_[db + "/" + key] = layer;
}

void Assets::log_summary() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::map<std::string, std::pair<int, int>> per;   // layer -> (overrides, additions)
    for (auto& [db, keys] : index_) {
        for (auto& [k, srcs] : keys) {
            bool vanilla_has = true;
            if (db == "data1.db") {
                vanilla_has = false;
                if (Registry* r = Registries::get().get_registry("kyubu:models"))
                    for (auto& e : r->entries)
                        if (e.vanilla && std::string(e.payload.begin(), e.payload.end()) == k) { vanilla_has = true; break; }
            }
            for (auto& s : srcs) {
                auto& p = per[s.layer];
                (vanilla_has ? p.first : p.second)++;
            }
        }
    }
    for (auto& [layer, c] : per)
        Log::get().write(KY_LOG_INFO, layer, KY_SIDE_NONE,
                         fmt("assets: overrides %d vanilla key(s), adds %d new key(s)", c.first, c.second));
}

void Assets::on_db_open(void* obj, const std::string& path) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::string name = lower(file_name(path));
    for (auto& c : name) if (c == '/') c = '\\';
    name = file_name(name);
    db_objects_[obj] = name;
    if (!category_for_db(name).empty() && !db_paths_.count(name)) db_paths_[name] = path;
}

std::string Assets::db_name_of(void* obj) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = db_objects_.find(obj);
    return it == db_objects_.end() ? std::string() : it->second;
}

std::string Assets::db_path(const std::string& db) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = db_paths_.find(db);
    return it == db_paths_.end() ? join_path(L().game_dir, db) : it->second;
}

// ------------------------------------------------------------------ game hooks
namespace {
using DbCtorFn = void*(__fastcall*)(void*, void*);
using DbOpenFn = bool(__fastcall*)(void*, void*, const VcString*);
using DbGetFn = bool(__fastcall*)(void*, void*, const VcString*, void**, uint32_t*);
using CloseFn = int(__cdecl*)(void*);

DbGetFn g_get_next = nullptr;
DbOpenFn g_open_next = nullptr;
CloseFn g_close_next = nullptr;
DbCtorFn g_db_ctor = nullptr;
DbOpenFn g_db_open = nullptr;
DbGetFn g_db_get_raw = nullptr;   // original function (trampoline chain end)
thread_local int t_internal_read = 0;

bool __fastcall db_get_hook(void* self, void* /*edx*/, const VcString* key, void** out, uint32_t* out_len) {
    Assets& a = Assets::get();
    std::string db = a.db_name_of(self);
    std::string k = key ? key->str() : std::string();
    if (t_internal_read || db.empty() || k.empty()) return g_get_next(self, nullptr, key, out, out_len);
    KyubuSide side = L().host.host == KY_HOST_SERVER ? KY_SIDE_SERVER : KY_SIDE_CLIENT;
    if (Events::get().has_subscribers("asset.request", side)) {
        KyubuAssetRequestData d{db.c_str(), k.c_str()};
        Events::get().emit("asset.request", side, &d, sizeof d);
    }
    if (db == "data4.db" && Assets::is_dictionary(k)) {
        if (!g_get_next(self, nullptr, key, out, out_len)) return false;
        std::vector<uint8_t> enc((uint8_t*)*out, (uint8_t*)*out + *out_len);
        auto plain = asset_decode(enc);
        std::string merged = a.merge_dictionaries(k, std::string(plain.begin(), plain.end()));
        auto re = asset_encode(std::vector<uint8_t>(merged.begin(), merged.end()));
        void* buf = game_new(re.size());
        memcpy(buf, re.data(), re.size());
        game_delete(*out);
        *out = buf;
        *out_len = (uint32_t)re.size();
        a.record_served(db, k, "merged");
        return true;
    }
    std::vector<std::string> shadowed;
    auto r = a.resolve_overlay(db, k, &shadowed);
    if (!r) {
        if (L().config.asset_trace) KDEBUG("asset %s/%s <- vanilla", db.c_str(), k.c_str());
        return g_get_next(self, nullptr, key, out, out_len);
    }
    auto enc = asset_encode(r->data);
    void* buf = game_new(enc.size());
    memcpy(buf, enc.data(), enc.size());
    *out = buf;
    *out_len = (uint32_t)enc.size();
    a.record_served(db, k, r->layer);
    if (L().config.asset_trace || !shadowed.empty()) {
        std::string sh;
        for (auto& s : shadowed) sh += (sh.empty() ? "" : ", ") + s;
        Log::get().write(shadowed.empty() ? KY_LOG_DEBUG : KY_LOG_INFO, "kyubu", KY_SIDE_NONE,
                         "asset " + db + "/" + k + " <- " + r->layer + (sh.empty() ? "" : " (shadowed: " + sh + ")"));
    }
    return true;
}

}  // namespace

// Implemented in game_hooks.cpp (world tracking needs the open/close hooks too).
void on_database_opened(void* obj, const std::string& path, bool ok);
void on_sqlite_closed(void* handle);

namespace {
bool __fastcall db_open_hook(void* self, void* /*edx*/, const VcString* path) {
    bool ok = g_open_next(self, nullptr, path);
    std::string p = path ? path->str() : std::string();
    Assets::get().on_db_open(self, p);
    on_database_opened(self, p, ok);
    return ok;
}
int __cdecl sqlite_close_hook(void* h) {
    on_sqlite_closed(h);
    return g_close_next(h);
}
}  // namespace

bool Assets::install_hooks() {
    auto& st = SymbolTable::get();
    auto verified = [&](const char* n) -> uintptr_t {
        const Symbol* s = st.find(n);
        if (!s || s->status != "verified") return 0;
        return st.addr(n);
    };
    uintptr_t get = verified("Database::get"), open = verified("Database::open"), close = verified("sqlite3_close");
    g_db_ctor = (DbCtorFn)verified("Database::Database");
    g_db_open = (DbOpenFn)open;
    g_db_get_raw = (DbGetFn)get;
    bool ok = true;
    std::string err;
    if (open && Hooks::get().hook_function("kyubu", open, (void*)&db_open_hook, kLoaderPriority, (void**)&g_open_next, nullptr, &err) != KY_OK) {
        KERROR("asset overlay: cannot hook Database::open: %s", err.c_str());
        ok = false;
    }
    if (get && Hooks::get().hook_function("kyubu", get, (void*)&db_get_hook, kLoaderPriority, (void**)&g_get_next, nullptr, &err) != KY_OK) {
        KERROR("asset overlay: cannot hook Database::get: %s", err.c_str());
        ok = false;
    }
    if (close) Hooks::get().hook_function("kyubu", close, (void*)&sqlite_close_hook, kLoaderPriority, (void**)&g_close_next, nullptr, &err);
    if (!open || !get) {
        KWARN("asset overlay unavailable on this build (Database symbols not verified)");
        Events::get().set_unavailable("asset.request", "Database::get is not a verified symbol on this build");
        return false;
    }
    return ok;
}

void* Assets::our_database(const std::string& db) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = own_dbs_.find(db);
    if (it != own_dbs_.end()) return it->second;
    if (!g_db_ctor || !g_db_open) return nullptr;
    void* obj = game_new(8);
    g_db_ctor(obj, nullptr);
    VcString path;
    vc_string_init(&path, db_path(db));
    bool ok = g_db_open(obj, nullptr, &path);   // goes through our hook, which records the object
    vc_string_free(&path);
    if (!ok) { KERROR("cannot open %s for loader reads", db.c_str()); return nullptr; }
    own_dbs_[db] = obj;
    return obj;
}

std::optional<std::vector<uint8_t>> Assets::read_vanilla(const std::string& db, const std::string& key) {
    void* obj = our_database(db);
    if (!obj || !g_db_get_raw) return std::nullopt;
    VcString k;
    vc_string_init(&k, key);
    void* out = nullptr;
    uint32_t len = 0;
    ++t_internal_read;
    bool ok = (g_get_next ? g_get_next : g_db_get_raw)(obj, nullptr, &k, &out, &len);
    --t_internal_read;
    vc_string_free(&k);
    if (!ok || !out) return std::nullopt;
    std::vector<uint8_t> enc((uint8_t*)out, (uint8_t*)out + len);
    game_delete(out);
    return asset_decode(enc);
}

std::vector<std::pair<std::string, std::string>> Assets::poll_changes() {
    std::vector<std::pair<std::string, std::string>> changed;
    uint64_t now = GetTickCount64();
    if (now - last_poll_ < 500) return changed;
    last_poll_ = now;
    bool dirty = false;
    {
        std::lock_guard<std::recursive_mutex> g(mu_);
        for (auto& [path, mt] : watched_) {
            uint64_t cur = file_mtime(path);
            if (cur != mt) { dirty = true; break; }
        }
        // new files in dev-assets or mod asset folders
        if (!dirty && L().dev_mode) {
            size_t count = 0;
            std::string dev = join_path(L().kyubu_dir, "dev-assets");
            for (const char* cat : {"models", "sounds", "icons", "text"}) count += list_dir(join_path(dev, cat)).size();
            static size_t last_count = (size_t)-1;
            if (last_count != (size_t)-1 && count != last_count) dirty = true;
            last_count = count;
        }
    }
    if (!dirty) return changed;
    std::map<std::string, uint64_t> before;
    std::map<std::string, std::pair<std::string, std::string>> key_of;
    {
        std::lock_guard<std::recursive_mutex> g(mu_);
        before = watched_;
    }
    build_index();
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (auto& [db, keys] : index_)
        for (auto& [k, srcs] : keys)
            for (auto& s : srcs)
                if (!s.disk.empty()) {
                    auto b = before.find(s.disk);
                    if (b == before.end() || b->second != s.mtime) changed.push_back({db, k});
                }
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    return changed;
}

}  // namespace kyubu
