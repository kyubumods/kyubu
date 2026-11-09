#include "models.h"

#include <map>
#include <unordered_map>

#include "events.h"
#include "gametypes.h"
#include "hooks.h"
#include "log.h"
#include "overlay.h"
#include "registry.h"
#include "symbols.h"

namespace kyubu {

namespace {
using LoadAllFn = void(__fastcall*)(void*, void*, const VcString*);
using ResizeFn = void(__fastcall*)(VcPtrVector*, void*, uint32_t);
using ModelCtorFn = void*(__fastcall*)(void*, void*, void*, int);
using ModelLoadFn = bool(__fastcall*)(void*, void*, const VcString*, void*, bool);

LoadAllFn g_loadall_next = nullptr;
ModelLoadFn g_load_next = nullptr;
ResizeFn g_resize = nullptr;
ModelCtorFn g_model_ctor = nullptr;
ModelLoadFn g_model_load = nullptr;
bool g_in_loadall = false;
std::unordered_map<void*, uint32_t> g_slot_of;   // Model* -> slot, built during loadAll
std::map<uint32_t, std::string> g_slot_key;      // slot -> key actually loaded

VcPtrVector* vec_of(void* loader) { return (VcPtrVector*)((char*)loader + 4); }

std::string registry_key(const RegEntry& e) { return std::string(e.payload.begin(), e.payload.end()); }

bool __fastcall model_load_hook(void* model, void* /*edx*/, const VcString* key, void* db, bool flag) {
    if (!g_in_loadall || !key) return g_load_next(model, nullptr, key, db, flag);
    auto it = g_slot_of.find(model);
    if (it == g_slot_of.end()) return g_load_next(model, nullptr, key, db, flag);
    uint32_t slot = it->second;
    std::string k = key->str();
    std::string base, name = k;
    if (auto p = k.find_last_of("/\\"); p != std::string::npos) { base = k.substr(0, p + 1); name = k.substr(p + 1); }
    // Is this vanilla slot remapped by a mod?
    auto& regs = Registries::get();
    std::string use = name;
    {
        std::lock_guard<std::recursive_mutex> g(regs.mutex());
        if (Registry* r = regs.get_registry("kyubu:models")) {
            for (auto& e : r->entries) {
                if (!e.vanilla || e.number != slot) continue;
                auto rm = r->remaps.find(e.id);
                if (rm != r->remaps.end()) {
                    use = std::string(rm->second.second.begin(), rm->second.second.end());
                    Log::get().write(KY_LOG_INFO, rm->second.first, KY_SIDE_NONE,
                                     fmt("slot %u (%s) loads %s (remap)", slot, e.id.c_str(), use.c_str()));
                }
                break;
            }
        }
    }
    g_slot_key[slot] = use;
    if (use == name) return g_load_next(model, nullptr, key, db, flag);
    VcString alt;
    vc_string_init(&alt, base + use);
    bool ok = g_load_next(model, nullptr, &alt, db, flag);
    vc_string_free(&alt);
    if (ok) return true;
    // A broken remap must not leave a vanilla slot empty: fall back to the vanilla model.
    KERROR("model slot %u: remapped key %s failed to load; using the vanilla model %s", slot, use.c_str(), name.c_str());
    g_slot_key[slot] = name;
    return g_load_next(model, nullptr, key, db, flag);
}

void* g_loader = nullptr;

void __fastcall loadall_hook(void* loader, void* /*edx*/, const VcString* base) {
    // loadAll resizes the vector to 2569, fills it with Model objects, opens data1.db and then
    // loads every slot. The Model* -> slot map is built when data1.db opens (see install_hooks).
    g_loader = loader;
    g_in_loadall = true;
    g_loadall_next(loader, nullptr, base);
    g_in_loadall = false;
    g_slot_of.clear();
    Models::get().on_loadall_done(loader);
}
}  // namespace

Models& Models::get() {
    static Models* m = new Models();
    return *m;
}

bool Models::install_hooks() {
    auto& st = SymbolTable::get();
    auto verified = [&](const char* n) -> uintptr_t {
        const Symbol* s = st.find(n);
        if (!s || s->status != "verified") return 0;
        return st.addr(n);
    };
    uintptr_t loadall = verified("ModelLoader::loadAll"), load = verified("Model::load");
    uintptr_t fill_done = 0;
    g_resize = (ResizeFn)verified("ModelVector::resize");
    g_model_ctor = (ModelCtorFn)verified("Model::Model");
    g_model_load = (ModelLoadFn)load;
    if (!loadall || !load || !g_resize || !g_model_ctor) {
        KWARN("model registry extension unavailable on this build (ModelLoader symbols not verified)");
        return false;
    }
    std::string err;
    if (Hooks::get().hook_function("kyubu", loadall, (void*)&loadall_hook, kLoaderPriority, (void**)&g_loadall_next, nullptr, &err) != KY_OK ||
        Hooks::get().hook_function("kyubu", load, (void*)&model_load_hook, kLoaderPriority, (void**)&g_load_next, nullptr, &err) != KY_OK) {
        KERROR("model registry: hook failed: %s", err.c_str());
        return false;
    }
    (void)fill_done;
    // After loadAll has created all vanilla Model objects (the loop after the resize), map them to slots.
    // We do that from the Database::open of data1.db inside loadAll, which happens right after the loop.
    return true;
}

void Models::on_data1_opened() {
    if (!g_in_loadall || !g_loader) return;
    VcPtrVector* v = vec_of(g_loader);
    g_slot_of.clear();
    for (uint32_t i = 0; i < v->size(); ++i) if (v->begin[i]) g_slot_of[v->begin[i]] = i;
}

void Models::on_loadall_done(void* loader) {
    loader_ = loader;
    extend();
    Events::get().emit_active("asset.models_loaded");
}

uint32_t Models::slot_count() { return loader_ ? vec_of(loader_)->size() : 0; }

void* Models::model_at(uint32_t slot) {
    if (!loader_ || slot >= slot_count()) return nullptr;
    return vec_of(loader_)->begin[slot];
}

std::string Models::key_of_slot(uint32_t slot) {
    auto it = g_slot_key.find(slot);
    return it == g_slot_key.end() ? std::string() : it->second;
}

void Models::extend() {
    if (!loader_) return;
    db_ = Assets::get().our_database("data1.db");
    if (!db_) { KERROR("model registry: cannot open data1.db; mod models not loaded"); return; }
    VcPtrVector* v = vec_of(loader_);
    void* engine = *(void**)((char*)loader_ + 0x10);
    auto& regs = Registries::get();
    std::vector<std::pair<uint32_t, std::string>> todo;
    int builtin = 0;   // Kyubu's own entries (the missing-content model)
    {
        std::lock_guard<std::recursive_mutex> g(regs.mutex());
        Registry* r = regs.get_registry("kyubu:models");
        if (!r) return;
        for (auto& e : r->entries) {
            if (e.vanilla) continue;
            if (e.owner == "kyubu") ++builtin;
            todo.push_back({e.number, e.placeholder ? "kyubu-missing.cub" : registry_key(e)});
        }
    }
    uint32_t need = v->size();
    for (auto& [n, k] : todo) need = std::max(need, n + 1);
    if (need > v->size()) g_resize(v, nullptr, need);
    int loaded = 0;
    for (auto& [n, key] : todo) {
        if (!v->begin[n]) {
            void* mem = game_new(0x60);
            g_model_ctor(mem, nullptr, engine, 0);
            v->begin[n] = mem;
        }
        VcString k;
        vc_string_init(&k, key);
        bool ok = g_model_load(v->begin[n], nullptr, &k, db_, true);
        vc_string_free(&k);
        g_slot_key[n] = key;
        if (ok) { ++loaded; continue; }
        // Show the missing-content model rather than leaving the slot empty.
        KERROR("model slot %u: loading %s failed (no such asset?); showing kyubu:missing instead", n, key.c_str());
        VcString mk;
        vc_string_init(&mk, "kyubu-missing.cub");
        g_model_load(v->begin[n], nullptr, &mk, db_, true);
        vc_string_free(&mk);
        g_slot_key[n] = "kyubu-missing.cub";
    }
    // Fill any other empty slot beyond vanilla with the missing-content model so the game never sees null.
    for (uint32_t i = 2569; i < v->size(); ++i) {
        if (v->begin[i]) continue;
        void* mem = game_new(0x60);
        g_model_ctor(mem, nullptr, engine, 0);
        v->begin[i] = mem;
        VcString k;
        vc_string_init(&k, "kyubu-missing.cub");
        g_model_load(mem, nullptr, &k, db_, true);
        vc_string_free(&k);
        g_slot_key[i] = "kyubu-missing.cub";
    }
    KINFO("Model slots: %u total: vanilla's 2569, %d from mods, %d Kyubu placeholder(s)", v->size(),
          std::max(0, loaded - builtin), builtin);
}

int Models::reload_key(const std::string& key) {
    if (!loader_ || !db_) return 0;
    VcPtrVector* v = vec_of(loader_);
    int n = 0;
    for (auto& [slot, k] : g_slot_key) {
        if (k != key || slot >= v->size() || !v->begin[slot]) continue;
        VcString s;
        vc_string_init(&s, k);
        if (g_model_load(v->begin[slot], nullptr, &s, db_, true)) ++n;
        vc_string_free(&s);
    }
    if (n) KINFO("hot reload: %s reloaded into %d slot(s)", key.c_str(), n);
    return n;
}

void Models::apply_numbers() {
    if (!loader_) return;
    // extend() loads the key of every registry entry into the slot of its current number (and resizes, and fills the gaps), so the
    // slots follow the numbers by themselves. A pointer shuffle after it, by the keys the slots held before, would swap two moved
    // entries back (a mod model and kyubu:missing, renumbered by a world's id map, would show the placeholder).
    extend();
}

}  // namespace kyubu
