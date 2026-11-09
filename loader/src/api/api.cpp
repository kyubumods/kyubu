// The KyubuApi function table handed to native mods (and wrapped by the Lua runtime).
#include "api.h"

#include <mutex>
#include <set>

#include "abilities.h"
#include "channels.h"
#include "console.h"
#include "events.h"
#include "game.h"
#include "gametypes.h"
#include "guard.h"
#include "hooks.h"
#include "host.h"
#include "imgui.h"
#include "manage.h"
#include "loader.h"
#include "log.h"
#include "modconfig.h"
#include "modmanager.h"
#include "overlay.h"
#include "registry.h"
#include "symbols.h"
#include "ui.h"

namespace kyubu {

namespace {

// Resolves the calling mod and side from a handle. Returns nullptr for bad handles.
Mod* who(KyubuModHandle h, KyubuSide* side = nullptr) {
    return ModManager::get().from_handle(h, side);
}

#define REQUIRE_MOD(h, side)                                        \
    KyubuSide side = KY_SIDE_NONE;                                  \
    Mod* mod = who(h, &side);                                       \
    if (!mod) return KY_E_INVALID_ARG;                              \
    if (mod->state == ModState::Faulted) return KY_E_FAULTED;

// Warns once per (mod, function) when a call is refused for being made from the wrong side.
void side_refused(const char* fn, KyubuSide side) {
    static std::mutex mu;
    static std::set<std::string> seen;
    Mod* m = current_mod_tls();
    std::string key = (m ? m->id : std::string("?")) + "/" + fn;
    std::lock_guard<std::mutex> g(mu);
    if (!seen.insert(key).second) return;
    Log::get().write(KY_LOG_WARN, m ? m->id : std::string("kyubu"), side,
                     std::string(fn) + " is " + (api_side_of(fn) == KY_SIDE_CLIENT ? "client" : "server") +
                         "-only and was called from the " + (side == KY_SIDE_CLIENT ? "client" : "server") + " side");
}

// True if `fn` may run on the side of the running callback (or `fallback` outside callbacks).
bool side_ok(const char* fn, KyubuSide fallback = KY_SIDE_NONE) {
    KyubuSide cur = current_side_tls();
    if (cur == KY_SIDE_NONE) cur = fallback;
    if (api_side_allowed(fn, cur)) return true;
    side_refused(fn, cur);
    return false;
}

std::mutex g_tasks_mu;
struct Task { Mod* mod; KyubuSide side; KyubuTaskFn fn; void* user; };
std::vector<Task> g_tasks;

// ----------------------------------------------------------------- core
void KYUBU_CALL k_log(KyubuModHandle self, KyubuLogLevel level, const char* msg) {
    KyubuSide side = KY_SIDE_NONE;
    Mod* m = who(self, &side);
    KyubuSide cs = current_side_tls();
    Log::get().write(level, m ? m->id : std::string("?"), cs != KY_SIDE_NONE ? cs : side, msg ? msg : "");
}
KyubuPhase KYUBU_CALL k_phase() { return L().phase.load(); }
KyubuSide KYUBU_CALL k_current_side() { return current_side_tls(); }
KyubuResult KYUBU_CALL k_host_info(KyubuHostInfo* out) {
    if (!out || out->size < sizeof(KyubuHostInfo)) return KY_E_INVALID_ARG;
    Loader& l = L();
    out->host = l.host.host;
    out->mode = l.mode;
    copy_out(l.host.build_id, out->build_id, sizeof out->build_id);
    out->image_base = l.host.base;
    out->image_size = l.host.image_size;
    out->dev_mode = l.dev_mode;
    out->safe_mode = l.safe_mode;
    return KY_OK;
}
KyubuResult KYUBU_CALL k_mod_id(KyubuModHandle self, char* buf, uint32_t cap) {
    Mod* m = who(self);
    return m ? copy_out(m->id, buf, cap) : KY_E_INVALID_ARG;
}
KyubuResult KYUBU_CALL k_game_dir(char* buf, uint32_t cap) { return copy_out(L().game_dir, buf, cap); }
KyubuResult KYUBU_CALL k_mod_data_dir(KyubuModHandle self, char* buf, uint32_t cap) {
    Mod* m = who(self);
    if (!m) return KY_E_INVALID_ARG;
    make_dirs(m->data_dir);
    return copy_out(m->data_dir, buf, cap);
}
KyubuResult KYUBU_CALL k_read_package_file(KyubuModHandle self, const char* path, void* buf, uint32_t cap, uint32_t* out_len) {
    Mod* m = who(self);
    if (!m || !path) return KY_E_INVALID_ARG;
    if (!m->pkg) return KY_E_INTERNAL;
    auto d = m->pkg->read(path);
    if (!d) return KY_E_NOT_FOUND;
    if (out_len) *out_len = (uint32_t)d->size();
    if (!buf || cap < d->size()) return KY_E_BUFFER;
    memcpy(buf, d->data(), d->size());
    return KY_OK;
}
KyubuResult KYUBU_CALL k_is_mod_loaded(const char* id, char* vbuf, uint32_t cap) {
    Mod* m = id ? ModManager::get().find(id) : nullptr;
    if (!m || !m->active()) return KY_E_NOT_FOUND;
    if (vbuf) copy_out(m->manifest.version.str(), vbuf, cap);
    return KY_OK;
}
void* KYUBU_CALL k_buffer_alloc(uint32_t size) { return HeapAlloc(GetProcessHeap(), 0, size ? size : 1); }
void KYUBU_CALL k_buffer_free(void* p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
KyubuResult KYUBU_CALL k_run_on_main_thread(KyubuModHandle self, KyubuTaskFn fn, void* user) {
    REQUIRE_MOD(self, side);
    if (!fn) return KY_E_INVALID_ARG;
    std::lock_guard<std::mutex> g(g_tasks_mu);
    g_tasks.push_back({mod, side, fn, user});
    return KY_OK;
}

// ----------------------------------------------------------------- symbols & memory
KyubuResult KYUBU_CALL k_resolve_symbol(const char* name, uintptr_t* out) {
    if (!name) return KY_E_INVALID_ARG;
    return SymbolTable::get().resolve(name, out);
}
uintptr_t KYUBU_CALL k_rva_to_addr(uint32_t rva) { return SymbolTable::get().rva_to_addr(rva); }
uint32_t KYUBU_CALL k_addr_to_rva(uintptr_t a) { return SymbolTable::get().addr_to_rva(a); }
KyubuResult KYUBU_CALL k_mem_read(uintptr_t addr, void* out, uint32_t len) {
    if (!out && len) return KY_E_INVALID_ARG;
    return safe_read(addr, out, len) ? KY_OK : KY_E_ACCESS;
}
KyubuResult KYUBU_CALL k_mem_write(KyubuModHandle self, uintptr_t addr, const void* data, uint32_t len) {
    REQUIRE_MOD(self, side);
    if (!data && len) return KY_E_INVALID_ARG;
    return safe_write(addr, data, len) ? KY_OK : KY_E_ACCESS;
}
void* KYUBU_CALL k_game_alloc(uint32_t n) { return game_new(n); }
void KYUBU_CALL k_game_free(void* p) { game_delete(p); }

// ----------------------------------------------------------------- hooks
KyubuResult report(Mod* mod, KyubuResult r, const std::string& what, const std::string& err) {
    if (r != KY_OK) Log::get().write(KY_LOG_ERROR, mod->id, current_side_tls(), what + " failed: " + err);
    return r;
}
KyubuResult KYUBU_CALL k_hook_function(KyubuModHandle self, uintptr_t target, void* handler, int32_t prio, void** next, KyubuId* id) {
    REQUIRE_MOD(self, side);
    std::string err;
    auto r = Hooks::get().hook_function(mod->id, target, handler, prio, next, id, &err);
    return report(mod, r, "hook_function(" + describe_address(target) + ")", err);
}
KyubuResult KYUBU_CALL k_hook_symbol(KyubuModHandle self, const char* sym, void* handler, int32_t prio, void** next, KyubuId* id) {
    REQUIRE_MOD(self, side);
    if (!sym) return KY_E_INVALID_ARG;
    uintptr_t a = 0;
    std::string why;
    auto r = SymbolTable::get().resolve(sym, &a, &why);
    if (r != KY_OK) return report(mod, r, std::string("hook_symbol(") + sym + ")", why.empty() ? "unknown symbol" : why);
    const Symbol* s = SymbolTable::get().find(sym);
    if (s && s->global) return report(mod, KY_E_INVALID_ARG, std::string("hook_symbol(") + sym + ")", "a global, not a function");
    std::string err;
    r = Hooks::get().hook_function(mod->id, a, handler, prio, next, id, &err);
    return report(mod, r, std::string("hook_symbol(") + sym + ")", err);
}
KyubuResult KYUBU_CALL k_hook_mid(KyubuModHandle self, uintptr_t addr, KyubuMidHookFn fn, void* user, KyubuId* id) {
    REQUIRE_MOD(self, side);
    if (!fn) return KY_E_INVALID_ARG;
    std::string err;
    KyubuSide s = side;
    auto r = Hooks::get().hook_mid(mod->id, addr, s, [fn, user](KyubuCpuContext* c) { fn(c, user); }, id, &err);
    return report(mod, r, "hook_mid(" + describe_address(addr) + ")", err);
}
KyubuResult KYUBU_CALL k_hook_import(KyubuModHandle self, const char* dll, const char* fn, void* handler, int32_t prio, void** next, KyubuId* id) {
    REQUIRE_MOD(self, side);
    if (!dll || !fn) return KY_E_INVALID_ARG;
    std::string err;
    auto r = Hooks::get().hook_import(mod->id, nullptr, dll, fn, handler, prio, next, id, &err);
    return report(mod, r, std::string("hook_import(") + dll + "!" + fn + ")", err);
}
KyubuResult KYUBU_CALL k_hook_vtable(KyubuModHandle self, void* obj, uint32_t index, void* handler, int32_t prio, void** next, KyubuId* id) {
    REQUIRE_MOD(self, side);
    std::string err;
    auto r = Hooks::get().hook_vtable(mod->id, obj, index, handler, prio, next, id, &err);
    return report(mod, r, fmt("hook_vtable(%p, %u)", obj, index), err);
}
KyubuResult KYUBU_CALL k_patch_bytes(KyubuModHandle self, uintptr_t addr, const void* bytes, uint32_t len, KyubuId* id) {
    REQUIRE_MOD(self, side);
    std::string err;
    auto r = Hooks::get().patch(mod->id, addr, bytes, len, id, &err);
    return report(mod, r, "patch_bytes(" + describe_address(addr) + ")", err);
}
KyubuResult KYUBU_CALL k_unhook(KyubuModHandle self, KyubuId id) {
    REQUIRE_MOD(self, side);
    return Hooks::get().remove(mod->id, id);
}

// ----------------------------------------------------------------- events
KyubuResult KYUBU_CALL k_subscribe(KyubuModHandle self, const char* ev, KyubuSide s, int32_t prio, KyubuEventFn fn, void* user, KyubuId* id) {
    REQUIRE_MOD(self, side);
    if (!ev) return KY_E_INVALID_ARG;
    KyubuSide use = s == KY_SIDE_NONE ? side : s;
    if (use == KY_SIDE_BOTH) {
        KyubuResult r = Events::get().subscribe(mod, ev, KY_SIDE_CLIENT, prio, fn, user, id);
        if (r != KY_OK) return r;
        return Events::get().subscribe(mod, ev, KY_SIDE_SERVER, prio, fn, user, nullptr);
    }
    return Events::get().subscribe(mod, ev, use, prio, fn, user, id);
}
KyubuResult KYUBU_CALL k_unsubscribe(KyubuModHandle self, KyubuId id) {
    REQUIRE_MOD(self, side);
    return Events::get().unsubscribe(mod, id);
}
KyubuResult KYUBU_CALL k_emit(KyubuModHandle self, const char* ev, KyubuSide s, void* data, uint32_t size, int cancellable, int* out_cancelled) {
    REQUIRE_MOD(self, side);
    if (!ev) return KY_E_INVALID_ARG;
    std::string n = ev;
    auto colon = n.find(':');
    if (colon == std::string::npos || n.substr(0, colon) != mod->id) return KY_E_NAMESPACE;   // custom events only
    KyubuSide use = s == KY_SIDE_NONE ? side : s;
    bool c = Events::get().emit(n, use, data, size, cancellable != 0);
    if (out_cancelled) *out_cancelled = c ? 1 : 0;
    return KY_OK;
}
void KYUBU_CALL k_event_cancel(KyubuEvent* ev) { if (ev && ev->cancellable) ev->cancelled = 1; }

// ----------------------------------------------------------------- assets
KyubuResult KYUBU_CALL k_asset_provider(KyubuModHandle self, const char* db, const char* glob, KyubuAssetProviderFn fn, void* user, KyubuId* id) {
    REQUIRE_MOD(self, side);
    if (!db || !glob) return KY_E_INVALID_ARG;
    return Assets::get().add_provider(mod, db, glob, fn, user, id);
}
KyubuResult KYUBU_CALL k_asset_read(const char* db, const char* key, void* buf, uint32_t cap, uint32_t* out_len) {
    if (!db || !key) return KY_E_INVALID_ARG;
    auto r = Assets::get().resolve(db, key);
    if (!r) return KY_E_NOT_FOUND;
    if (out_len) *out_len = (uint32_t)r->data.size();
    if (!buf || cap < r->data.size()) return KY_E_BUFFER;
    memcpy(buf, r->data.data(), r->data.size());
    return KY_OK;
}
KyubuResult KYUBU_CALL k_asset_served_by(const char* db, const char* key, char* buf, uint32_t cap) {
    if (!db || !key) return KY_E_INVALID_ARG;
    return copy_out(Assets::get().served_by(db, key), buf, cap);
}

// ----------------------------------------------------------------- registries
KyubuResult phase_is(KyubuPhase p) { return L().phase.load() == p ? KY_OK : KY_E_PHASE; }
KyubuResult KYUBU_CALL k_registry_create(KyubuModHandle self, const char* reg, uint32_t schema, int persistent) {
    REQUIRE_MOD(self, side);
    if (!reg) return KY_E_INVALID_ARG;
    if (auto r = phase_is(KY_PHASE_PREINIT); r != KY_OK) return r;
    return Registries::get().create(mod->id, reg, schema, persistent != 0);
}
KyubuResult KYUBU_CALL k_registry_add(KyubuModHandle self, const char* reg, const char* id, const void* p, uint32_t n, uint32_t* num) {
    REQUIRE_MOD(self, side);
    if (!reg || !id) return KY_E_INVALID_ARG;
    if (auto r = phase_is(KY_PHASE_REGISTER); r != KY_OK) return r;
    return Registries::get().add(mod->id, reg, id, p, n, num);
}
KyubuResult KYUBU_CALL k_ability_define(KyubuModHandle self, const char* id, const KyubuAbilityDef* def) {
    REQUIRE_MOD(self, side);
    if (!id || !def) return KY_E_INVALID_ARG;
    if (mod->manifest.trust != "full") {   // design, "Decisions from review": a restricted mod gets no ability API
        Log::get().write(KY_LOG_ERROR, mod->id, side, "ability_define needs trust \"full\"; this mod is \"" + mod->manifest.trust + "\"");
        return KY_E_TRUST;
    }
    if (auto r = phase_is(KY_PHASE_REGISTER); r != KY_OK) return r;
    return ability_define(mod->id, side, id, def);
}
KyubuResult KYUBU_CALL k_ability_number(const char* id, uint32_t* out) {
    if (!id || !out) return KY_E_INVALID_ARG;
    return Registries::get().lookup("kyubu:abilities", id, out);
}
KyubuResult KYUBU_CALL k_ability_name(uint32_t number, char* buf, uint32_t cap) {
    std::string s;
    auto r = Registries::get().name_of("kyubu:abilities", number, &s);
    return r == KY_OK ? copy_out(s, buf, cap) : r;
}
KyubuResult KYUBU_CALL k_ability_list(uint32_t index, char* buf, uint32_t cap, uint32_t* out_number, uint32_t* out_count) {
    const std::vector<RegEntry> all = ability_entries();   // sorted by number; empty before the freeze
    if (all.empty()) return KY_E_PHASE;
    if (out_count) *out_count = (uint32_t)all.size();
    if (index >= all.size()) return KY_E_NOT_FOUND;
    if (out_number) *out_number = all[index].number;
    return buf ? copy_out(all[index].id, buf, cap) : KY_OK;
}
KyubuResult KYUBU_CALL k_ability_slot_owner(int32_t class_type, int32_t spec, char* buf, uint32_t cap, uint32_t* out_number) {
    if (class_type < 1 || class_type > 4 || spec < 0 || spec > 1) return KY_E_INVALID_ARG;
    if (!ability_slots_ready()) return KY_E_PHASE;
    const AbilitySlotHolder h = ability_slot_holder(class_type, spec);
    if (h.id.empty()) return KY_E_NOT_FOUND;
    if (out_number) *out_number = h.number;
    return buf ? copy_out(h.id, buf, cap) : KY_OK;
}
KyubuResult KYUBU_CALL k_registry_remap(KyubuModHandle self, const char* reg, const char* id, const void* p, uint32_t n) {
    REQUIRE_MOD(self, side);
    if (!reg || !id) return KY_E_INVALID_ARG;
    if (auto r = phase_is(KY_PHASE_REGISTER); r != KY_OK) return r;
    return Registries::get().remap(mod->id, reg, id, p, n);
}
KyubuResult KYUBU_CALL k_registry_lookup(const char* reg, const char* id, uint32_t* out) {
    if (!reg || !id) return KY_E_INVALID_ARG;
    return Registries::get().lookup(reg, id, out);
}
KyubuResult KYUBU_CALL k_registry_name(const char* reg, uint32_t number, char* buf, uint32_t cap) {
    if (!reg) return KY_E_INVALID_ARG;
    std::string s;
    auto r = Registries::get().name_of(reg, number, &s);
    return r == KY_OK ? copy_out(s, buf, cap) : r;
}
KyubuResult KYUBU_CALL k_registry_payload(const char* reg, const char* id, void* buf, uint32_t cap, uint32_t* out_len) {
    if (!reg || !id) return KY_E_INVALID_ARG;
    std::vector<uint8_t> p;
    auto r = Registries::get().payload(reg, id, &p);
    if (r != KY_OK) return r;
    if (out_len) *out_len = (uint32_t)p.size();
    if (!buf || cap < p.size()) return p.empty() ? KY_OK : KY_E_BUFFER;
    memcpy(buf, p.data(), p.size());
    return KY_OK;
}
KyubuResult KYUBU_CALL k_registry_count(const char* reg, uint32_t* out) {
    if (!reg || !out) return KY_E_INVALID_ARG;
    return Registries::get().count(reg, out);
}
KyubuResult KYUBU_CALL k_registry_entry(const char* reg, uint32_t index, char* buf, uint32_t cap, uint32_t* num) {
    if (!reg) return KY_E_INVALID_ARG;
    std::string id;
    uint32_t n = 0;
    auto r = Registries::get().entry(reg, index, &id, &n);
    if (r != KY_OK) return r;
    if (num) *num = n;
    return copy_out(id, buf, cap);
}
KyubuResult KYUBU_CALL k_registry_hash(const char* reg, char* buf, uint32_t cap) {
    if (!reg) return KY_E_INVALID_ARG;
    if (!Registries::get().frozen()) return KY_E_PHASE;
    std::string h;
    auto r = Registries::get().hash(reg, &h);
    return r == KY_OK ? copy_out(h, buf, cap) : r;
}

// ----------------------------------------------------------------- channels
KyubuResult KYUBU_CALL k_channel_register(KyubuModHandle self, const char* ch, KyubuSide s, KyubuChannelFn fn, void* user) {
    REQUIRE_MOD(self, side);
    if (!ch) return KY_E_INVALID_ARG;
    return Channels::get().reg(mod, s == KY_SIDE_NONE ? side : s, ch, fn, user);
}
KyubuResult KYUBU_CALL k_channel_send(KyubuModHandle self, const char* ch, uint32_t conn, const void* data, uint32_t len) {
    REQUIRE_MOD(self, side);
    if (!ch) return KY_E_INVALID_ARG;
    KyubuSide from = current_side_tls() != KY_SIDE_NONE ? current_side_tls() : side;
    return Channels::get().send(mod, from, ch, conn, data, len);
}
KyubuResult KYUBU_CALL k_session_manifest(char* buf, uint32_t cap, uint32_t* out_len) {
    std::string s = session_manifest_json();
    if (out_len) *out_len = (uint32_t)s.size() + 1;
    return copy_out(s, buf, cap);
}

// ----------------------------------------------------------------- interfaces
KyubuResult KYUBU_CALL k_publish_interface(KyubuModHandle self, const char* name, uint32_t v, void* iface) {
    REQUIRE_MOD(self, side);
    if (!name) return KY_E_INVALID_ARG;
    return Interfaces::get().publish(mod, name, v, iface);
}
KyubuResult KYUBU_CALL k_get_interface(const char* name, uint32_t minv, void** out, uint32_t* v) {
    if (!name) return KY_E_INVALID_ARG;
    return Interfaces::get().get_iface(name, minv, out, v);
}

// ----------------------------------------------------------------- console & UI
KyubuResult KYUBU_CALL k_register_command(KyubuModHandle self, const char* name, const char* help, KyubuCommandFn fn, void* user) {
    REQUIRE_MOD(self, side);
    if (!name) return KY_E_INVALID_ARG;
    if (L().host.host != KY_HOST_CLIENT || !side_ok("register_command", side)) return KY_E_SIDE;
    return Console::get().reg(mod, name, help ? help : "", fn, user);
}
void* KYUBU_CALL k_imgui_context() { return Ui::get().ready() && side_ok("imgui_context") ? ImGui::GetCurrentContext() : nullptr; }
bool ui_ok(const char* fn) { return side_ok(fn) && Ui::get().frame_active(); }
int KYUBU_CALL k_ui_begin(const char* title, int* open) {
    if (!ui_ok("ui_begin") || !title) return 0;
    bool o = open ? *open != 0 : true;
    bool r = ImGui::Begin(title, open ? &o : nullptr);
    if (open) *open = o ? 1 : 0;
    return r ? 1 : 0;
}
void KYUBU_CALL k_ui_end() { if (ui_ok("ui_end")) ImGui::End(); }
void KYUBU_CALL k_ui_text(const char* t) { if (ui_ok("ui_text") && t) ImGui::TextUnformatted(t); }
int KYUBU_CALL k_ui_button(const char* l) { return ui_ok("ui_button") && l && ImGui::Button(l) ? 1 : 0; }
int KYUBU_CALL k_ui_checkbox(const char* l, int* v) {
    if (!ui_ok("ui_checkbox") || !l || !v) return 0;
    bool b = *v != 0;
    bool r = ImGui::Checkbox(l, &b);
    *v = b ? 1 : 0;
    return r ? 1 : 0;
}
int KYUBU_CALL k_ui_slider_float(const char* l, float* v, float mn, float mx) {
    return ui_ok("ui_slider_float") && l && v && ImGui::SliderFloat(l, v, mn, mx) ? 1 : 0;
}
int KYUBU_CALL k_ui_input_text(const char* l, char* buf, uint32_t cap) {
    return ui_ok("ui_input_text") && l && buf && cap && ImGui::InputText(l, buf, cap) ? 1 : 0;
}
void KYUBU_CALL k_ui_same_line() { if (ui_ok("ui_same_line")) ImGui::SameLine(); }
void KYUBU_CALL k_ui_separator() { if (ui_ok("ui_separator")) ImGui::Separator(); }
int KYUBU_CALL k_ui_want_input() { return side_ok("ui_want_input") && (Ui::get().wants_mouse() || Ui::get().wants_keyboard()); }

// ----------------------------------------------------------------- diagnostics
KyubuResult KYUBU_CALL k_mod_stats(const char* id, double* ms, uint32_t* hooks) {
    Mod* m = id ? ModManager::get().find(id) : nullptr;
    if (!m) return KY_E_NOT_FOUND;
    if (ms) *ms = m->ms_last_frame;
    if (hooks) *hooks = (uint32_t)Hooks::get().count(m->id);
    return KY_OK;
}
KyubuResult KYUBU_CALL k_mod_state(const char* id, char* buf, uint32_t cap) {
    Mod* m = id ? ModManager::get().find(id) : nullptr;
    if (!m) return KY_E_NOT_FOUND;
    return copy_out(mod_state_name(m->state), buf, cap);
}

// ----------------------------------------------------------------- mod options (1.1)
template <typename V, typename T>   // V: the option's value type, T: the ABI's
KyubuResult config_read(KyubuModHandle self, const char* key, OptType type, T* out) {
    REQUIRE_MOD(self, side);
    if (!key || !out) return KY_E_INVALID_ARG;
    OptValue v;
    KyubuResult r = config_get(mod, key, type, &v, nullptr);
    if (r == KY_OK) *out = (T)std::get<V>(v);
    return r;
}
KyubuResult KYUBU_CALL k_config_get_bool(KyubuModHandle self, const char* key, int* out) {
    return config_read<bool>(self, key, OptType::Bool, out);
}
KyubuResult KYUBU_CALL k_config_get_int(KyubuModHandle self, const char* key, int64_t* out) {
    return config_read<int64_t>(self, key, OptType::Int, out);
}
KyubuResult KYUBU_CALL k_config_get_float(KyubuModHandle self, const char* key, double* out) {
    return config_read<double>(self, key, OptType::Float, out);
}
KyubuResult KYUBU_CALL k_config_get_string(KyubuModHandle self, const char* key, char* buf, uint32_t cap) {
    REQUIRE_MOD(self, side);
    if (!key) return KY_E_INVALID_ARG;
    OptValue v;
    KyubuResult r = config_get(mod, key, OptType::String, &v, nullptr);
    return r == KY_OK ? copy_out(std::get<std::string>(v), buf, cap) : r;
}
KyubuResult KYUBU_CALL k_config_set(KyubuModHandle self, const char* key, const char* value) {
    REQUIRE_MOD(self, side);
    if (!key || !value) return KY_E_INVALID_ARG;
    return config_set_text(mod, key, value, nullptr);
}

// ----------------------------------------------------------------- mod management (1.2, mods/manage.cpp)
static uint32_t KYUBU_CALL k_mods_count(KyubuModHandle self) {
    REQUIRE_MOD(self, side);
    if (manage_permission(mod, side, "mods_count") != KY_OK) return 0;
    return manage_mods_count();
}
static KyubuResult KYUBU_CALL k_mods_get(KyubuModHandle self, uint32_t index, KyubuModInfo* out) {
    REQUIRE_MOD(self, side);
    const KyubuResult p = manage_permission(mod, side, "mods_get");
    if (p != KY_OK) return p;
    return manage_mods_get(index, out);
}
static KyubuResult KYUBU_CALL k_mods_package_hash(KyubuModHandle self, const char* id, uint8_t out[32]) {
    REQUIRE_MOD(self, side);
    const KyubuResult p = manage_permission(mod, side, "mods_package_hash");
    if (p != KY_OK) return p;
    return manage_package_hash(id, out);
}

static uint32_t KYUBU_CALL k_mods_option_count(KyubuModHandle self, const char* id) {
    REQUIRE_MOD(self, side);
    if (!id || !*id) return 0;
    if (manage_permission(mod, side, "mods_option_count") != KY_OK) return 0;
    return manage_option_count(id);
}

static KyubuResult KYUBU_CALL k_mods_option_get(KyubuModHandle self, const char* id, uint32_t index,
                                                KyubuOptionInfo* out) {
    REQUIRE_MOD(self, side);
    if (!id || !*id || !out) return KY_E_INVALID_ARG;
    const KyubuResult p = manage_permission(mod, side, "mods_option_get");
    if (p != KY_OK) return p;
    return manage_option_get(id, index, out);
}

static KyubuResult KYUBU_CALL k_mods_option_check(KyubuModHandle self, const char* id, const char* key,
                                                  const char* value) {
    REQUIRE_MOD(self, side);
    if (!id || !*id || !key || !*key || !value) return KY_E_INVALID_ARG;
    const KyubuResult p = manage_permission(mod, side, "mods_option_check");
    if (p != KY_OK) return p;
    return manage_option_check(id, key, value);
}

static KyubuResult KYUBU_CALL k_mods_option_set(KyubuModHandle self, const char* id, const char* key,
                                                const char* value) {
    REQUIRE_MOD(self, side);
    if (!id || !*id || !key || !*key || !value) return KY_E_INVALID_ARG;
    const KyubuResult p = manage_permission(mod, side, "mods_option_set");
    if (p != KY_OK) return p;
    return manage_option_set(id, key, value);
}

static KyubuResult KYUBU_CALL k_mods_rescan(KyubuModHandle self) {
    REQUIRE_MOD(self, side);
    const KyubuResult p = manage_permission(mod, side, "mods_rescan");
    if (p != KY_OK) return p;
    return manage_rescan();
}

static KyubuResult KYUBU_CALL k_mods_queue(KyubuModHandle self, const KyubuModOp* op) {
    REQUIRE_MOD(self, side);
    const KyubuResult p = manage_permission(mod, side, "mods_queue");
    if (p != KY_OK) return p;
    return manage_queue(op);
}

static KyubuResult KYUBU_CALL k_mods_queue_clear(KyubuModHandle self) {
    REQUIRE_MOD(self, side);
    const KyubuResult p = manage_permission(mod, side, "mods_queue_clear");
    if (p != KY_OK) return p;
    return manage_queue_clear();
}

static uint32_t KYUBU_CALL k_mods_queue_count(KyubuModHandle self) {
    REQUIRE_MOD(self, side);
    if (manage_permission(mod, side, "mods_queue_count") != KY_OK) return 0;
    return manage_queue_count();
}

static KyubuResult KYUBU_CALL k_mods_queue_get(KyubuModHandle self, uint32_t index, KyubuModOp* out) {
    REQUIRE_MOD(self, side);
    const KyubuResult p = manage_permission(mod, side, "mods_queue_get");
    if (p != KY_OK) return p;
    return manage_queue_get(index, out);
}

static KyubuResult KYUBU_CALL k_game_restart(KyubuModHandle self) {
    REQUIRE_MOD(self, side);
    const KyubuResult p = manage_permission(mod, side, "game_restart");
    if (p != KY_OK) return p;
    return restart_game();
}

KyubuApi make_table() {
    KyubuApi a{};
    a.size = sizeof(KyubuApi);
    a.version = KYUBU_API_VERSION;
    a.log = k_log; a.phase = k_phase; a.current_side = k_current_side; a.host_info = k_host_info;
    a.mod_id = k_mod_id; a.game_dir = k_game_dir; a.mod_data_dir = k_mod_data_dir;
    a.read_package_file = k_read_package_file; a.is_mod_loaded = k_is_mod_loaded;
    a.buffer_alloc = k_buffer_alloc; a.buffer_free = k_buffer_free; a.run_on_main_thread = k_run_on_main_thread;
    a.resolve_symbol = k_resolve_symbol; a.rva_to_addr = k_rva_to_addr; a.addr_to_rva = k_addr_to_rva;
    a.mem_read = k_mem_read; a.mem_write = k_mem_write; a.game_alloc = k_game_alloc; a.game_free = k_game_free;
    a.hook_function = k_hook_function; a.hook_symbol = k_hook_symbol; a.hook_mid = k_hook_mid;
    a.hook_import = k_hook_import; a.hook_vtable = k_hook_vtable; a.patch_bytes = k_patch_bytes; a.unhook = k_unhook;
    a.subscribe = k_subscribe; a.unsubscribe = k_unsubscribe; a.emit = k_emit; a.event_cancel = k_event_cancel;
    a.asset_provider = k_asset_provider; a.asset_read = k_asset_read; a.asset_served_by = k_asset_served_by;
    a.registry_create = k_registry_create; a.registry_add = k_registry_add; a.registry_remap = k_registry_remap;
    a.registry_lookup = k_registry_lookup; a.registry_name = k_registry_name; a.registry_payload = k_registry_payload;
    a.registry_count = k_registry_count; a.registry_entry = k_registry_entry; a.registry_hash = k_registry_hash;
    a.channel_register = k_channel_register; a.channel_send = k_channel_send; a.session_manifest = k_session_manifest;
    a.publish_interface = k_publish_interface; a.get_interface = k_get_interface;
    a.register_command = k_register_command; a.imgui_context = k_imgui_context;
    a.ui_begin = k_ui_begin; a.ui_end = k_ui_end; a.ui_text = k_ui_text; a.ui_button = k_ui_button;
    a.ui_checkbox = k_ui_checkbox; a.ui_slider_float = k_ui_slider_float; a.ui_input_text = k_ui_input_text;
    a.ui_same_line = k_ui_same_line; a.ui_separator = k_ui_separator; a.ui_want_input = k_ui_want_input;
    a.mod_stats = k_mod_stats; a.mod_state = k_mod_state;
    game::fill_table(a);
    a.config_get_bool = k_config_get_bool; a.config_get_int = k_config_get_int; a.config_get_float = k_config_get_float;
    a.config_get_string = k_config_get_string; a.config_set = k_config_set;
    a.ability_define = k_ability_define; a.ability_number = k_ability_number; a.ability_name = k_ability_name;
    a.ability_list = k_ability_list; a.ability_slot_owner = k_ability_slot_owner;
    // mod management (1.2): mods/manage.cpp, appended after the game API's last member
    a.mods_count = k_mods_count; a.mods_get = k_mods_get; a.mods_package_hash = k_mods_package_hash;
    a.mods_option_count = k_mods_option_count; a.mods_option_get = k_mods_option_get;
    a.mods_option_check = k_mods_option_check; a.mods_option_set = k_mods_option_set;
    a.mods_rescan = k_mods_rescan;
    a.mods_queue = k_mods_queue; a.mods_queue_clear = k_mods_queue_clear;
    a.mods_queue_count = k_mods_queue_count; a.mods_queue_get = k_mods_queue_get;
    a.game_restart = k_game_restart;
    return a;
}

}  // namespace

// ----------------------------------------------------------------- side rules
// Single source of truth for the side each API member may be called from. Every member of KyubuApi is
// listed (tests check this). KY_SIDE_BOTH = either side. v1.0 has no server-only (authoritative)
// functions yet; they arrive with the world/entity API and go in this table as KY_SIDE_SERVER.
namespace {
constexpr ApiSideRule kSideRules[] = {
    {"log", KY_SIDE_BOTH}, {"phase", KY_SIDE_BOTH}, {"current_side", KY_SIDE_BOTH}, {"host_info", KY_SIDE_BOTH},
    {"mod_id", KY_SIDE_BOTH}, {"game_dir", KY_SIDE_BOTH}, {"mod_data_dir", KY_SIDE_BOTH},
    {"read_package_file", KY_SIDE_BOTH}, {"is_mod_loaded", KY_SIDE_BOTH}, {"buffer_alloc", KY_SIDE_BOTH},
    {"buffer_free", KY_SIDE_BOTH}, {"run_on_main_thread", KY_SIDE_BOTH},
    {"resolve_symbol", KY_SIDE_BOTH}, {"rva_to_addr", KY_SIDE_BOTH}, {"addr_to_rva", KY_SIDE_BOTH},
    {"mem_read", KY_SIDE_BOTH}, {"mem_write", KY_SIDE_BOTH}, {"game_alloc", KY_SIDE_BOTH}, {"game_free", KY_SIDE_BOTH},
    {"hook_function", KY_SIDE_BOTH}, {"hook_symbol", KY_SIDE_BOTH}, {"hook_mid", KY_SIDE_BOTH},
    {"hook_import", KY_SIDE_BOTH}, {"hook_vtable", KY_SIDE_BOTH}, {"patch_bytes", KY_SIDE_BOTH}, {"unhook", KY_SIDE_BOTH},
    {"subscribe", KY_SIDE_BOTH}, {"unsubscribe", KY_SIDE_BOTH}, {"emit", KY_SIDE_BOTH}, {"event_cancel", KY_SIDE_BOTH},
    {"asset_provider", KY_SIDE_BOTH}, {"asset_read", KY_SIDE_BOTH}, {"asset_served_by", KY_SIDE_BOTH},
    {"registry_create", KY_SIDE_BOTH}, {"registry_add", KY_SIDE_BOTH}, {"registry_remap", KY_SIDE_BOTH},
    {"registry_lookup", KY_SIDE_BOTH}, {"registry_name", KY_SIDE_BOTH}, {"registry_payload", KY_SIDE_BOTH},
    {"registry_count", KY_SIDE_BOTH}, {"registry_entry", KY_SIDE_BOTH}, {"registry_hash", KY_SIDE_BOTH},
    {"channel_register", KY_SIDE_BOTH}, {"channel_send", KY_SIDE_BOTH}, {"session_manifest", KY_SIDE_BOTH},
    {"publish_interface", KY_SIDE_BOTH}, {"get_interface", KY_SIDE_BOTH},
    {"register_command", KY_SIDE_CLIENT}, {"imgui_context", KY_SIDE_CLIENT}, {"ui_begin", KY_SIDE_CLIENT},
    {"ui_end", KY_SIDE_CLIENT}, {"ui_text", KY_SIDE_CLIENT}, {"ui_button", KY_SIDE_CLIENT},
    {"ui_checkbox", KY_SIDE_CLIENT}, {"ui_slider_float", KY_SIDE_CLIENT}, {"ui_input_text", KY_SIDE_CLIENT},
    {"ui_same_line", KY_SIDE_CLIENT}, {"ui_separator", KY_SIDE_CLIENT}, {"ui_want_input", KY_SIDE_CLIENT},
    {"mod_stats", KY_SIDE_BOTH}, {"mod_state", KY_SIDE_BOTH},
    // game API (1.1 on): every member also has a GAME_API line in loader/src/game/entries.inc
    {"game_available", KY_SIDE_BOTH}, {"game_enum", KY_SIDE_BOTH}, {"creature_get", KY_SIDE_BOTH},
    {"creature_player", KY_SIDE_CLIENT},   // the server side has no "local" player
    {"creature_spawn", KY_SIDE_SERVER},    // a new creature belongs to the server side
    {"creature_find", KY_SIDE_BOTH},       // a handle on the caller's side for any creature of its World
    {"creature_teleport", KY_SIDE_BOTH},   // the owner check decides which creatures a side may move
    {"body_define", KY_SIDE_BOTH}, {"body_name", KY_SIDE_BOTH}, {"creature_body_attach", KY_SIDE_BOTH}, {"creature_body_play", KY_SIDE_BOTH},
    {"creature_body_offset", KY_SIDE_BOTH}, {"creature_body_weight", KY_SIDE_BOTH}, {"creature_body_read", KY_SIDE_BOTH}, {"creature_body_point", KY_SIDE_BOTH},
    {"creature_set_yaw", KY_SIDE_BOTH},   // the owner check decides which creatures a side may turn
    {"creature_set_flags", KY_SIDE_BOTH},
    {"world_block", KY_SIDE_BOTH},   // each side reads its own World's blocks
    {"creature_hit", KY_SIDE_BOTH},  // the World decides: a joined client's hit goes to the server, as the game's own
    {"creature_state_get", KY_SIDE_BOTH}, {"creature_state_set", KY_SIDE_BOTH},   // the owner check decides who may write
    {"shot_take", KY_SIDE_SERVER}, {"projectile_push", KY_SIDE_SERVER},           // Server.exe's World flies what it pushes
    {"creature_give_item", KY_SIDE_CLIENT},   // the bags of the local player; the insert is mapped for Cube.exe only
    // mod options (1.1)
    {"config_get_bool", KY_SIDE_BOTH}, {"config_get_int", KY_SIDE_BOTH}, {"config_get_float", KY_SIDE_BOTH},
    {"config_get_string", KY_SIDE_BOTH}, {"config_set", KY_SIDE_BOTH},
    // abilities (1.1): definitions are data, the same on both sides
    {"ability_define", KY_SIDE_BOTH}, {"ability_number", KY_SIDE_BOTH}, {"ability_name", KY_SIDE_BOTH},
    {"ability_list", KY_SIDE_BOTH}, {"ability_slot_owner", KY_SIDE_BOTH},
    // the game API for abilities: the owner check decides which creatures a side may start or end (as creature_teleport's)
    {"ability_get", KY_SIDE_BOTH}, {"ability_cooldown", KY_SIDE_BOTH}, {"ability_cooldown_set", KY_SIDE_BOTH}, {"ability_start", KY_SIDE_BOTH}, {"ability_end", KY_SIDE_BOTH},
    // the ranks are the local player's, kept next to the character save: a client's
    {"ability_rank_get", KY_SIDE_CLIENT}, {"ability_rank_set", KY_SIDE_CLIENT},
    // the animation editor's calls: the preview clock is the local player's, so a client's
    {"animation_define_from_text", KY_SIDE_CLIENT}, {"animation_preview", KY_SIDE_CLIENT}, {"animation_preview_clear", KY_SIDE_CLIENT},
    // the clip binding is data, like ability_define: the same on both sides (a server side records it, draws nothing)
    {"ability_animation_bind", KY_SIDE_BOTH}, {"ability_animation_bind_for", KY_SIDE_BOTH},
    // the UI scene and the render device: Cube.exe's plasma engine, so the client's
    {"ui_node_find", KY_SIDE_CLIENT}, {"ui_node_get", KY_SIDE_CLIENT}, {"ui_node_child", KY_SIDE_CLIENT}, {"render_get", KY_SIDE_CLIENT},
    // the game's particles: the step output a client's controller plays, so the client's (the push is mapped for Cube.exe only)
    {"particle_spawn", KY_SIDE_CLIENT},
    // lights: the frame a client draws, so the client's (the sites are mapped for Cube.exe only)
    {"light_spawn", KY_SIDE_CLIENT}, {"light_set", KY_SIDE_CLIENT}, {"light_remove", KY_SIDE_CLIENT},
    // mod management (1.2): the loader's own data, no side
    {"mods_count", KY_SIDE_BOTH}, {"mods_get", KY_SIDE_BOTH}, {"mods_package_hash", KY_SIDE_BOTH},
    {"mods_option_count", KY_SIDE_BOTH}, {"mods_option_get", KY_SIDE_BOTH}, {"mods_option_check", KY_SIDE_BOTH},
    {"mods_option_set", KY_SIDE_BOTH}, {"mods_rescan", KY_SIDE_BOTH},
    {"mods_queue", KY_SIDE_BOTH}, {"mods_queue_clear", KY_SIDE_BOTH},
    {"mods_queue_count", KY_SIDE_BOTH}, {"mods_queue_get", KY_SIDE_BOTH},
    // restart (D3): the start menu is the client's
    {"game_restart", KY_SIDE_CLIENT},
};
}  // namespace

const ApiSideRule* api_side_rules(size_t* count) {
    if (count) *count = sizeof kSideRules / sizeof kSideRules[0];
    return kSideRules;
}

KyubuSide api_side_of(const char* fn) {
    for (auto& r : kSideRules) if (strcmp(r.name, fn) == 0) return r.sides;
    return KY_SIDE_NONE;
}

bool api_side_allowed(const char* fn, KyubuSide caller) {
    if (caller != KY_SIDE_CLIENT && caller != KY_SIDE_SERVER) return true;   // not inside a sided callback
    return (api_side_of(fn) & caller) != 0;
}

bool api_version_supported(uint32_t v) {
    return (v >> 16) == KYUBU_API_VERSION_MAJOR && (v & 0xFFFF) <= KYUBU_API_VERSION_MINOR;
}

const KyubuApi* api_table() {
    static KyubuApi table = make_table();
    return &table;
}

// Runs queued run_on_main_thread tasks (called from frame.begin / server tick).
void api_run_main_thread_tasks() {
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> g(g_tasks_mu);
        tasks.swap(g_tasks);
    }
    for (auto& t : tasks) ModManager::get().invoke(t.mod, t.side, "run_on_main_thread", [&] { t.fn(t.user); });
}

}  // namespace kyubu
