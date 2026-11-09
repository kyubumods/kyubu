#include "luart.h"

#include <cmath>
#include <map>
#include <mutex>

#include "abilities.h"
#include "ability_step.h"
#include "api.h"
#include "call.h"
#include "channels.h"
#include "console.h"
#include "events.h"
#include "handles.h"
#include "hooks.h"
#include "json.hpp"
#include "lights.h"
#include "loader.h"
#include "log.h"
#include "lua.hpp"
#include "modconfig.h"
#include "modmanager.h"
#include "overlay.h"
#include "prelude_lua.h"   // generated: static const char kPreludeLua[]
#include "registry.h"
#include "symbols.h"
#include "ui.h"

namespace kyubu {

void add_module_dir(lua_State* L, const std::string& dir) {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    std::string path = join_path(dir, "?.lua") + ";" + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
    lua_pop(L, 1);
    lua_pushstring(L, path.c_str());
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
}

namespace {

const KyubuApi* A() { return api_table(); }
Loader& L_() { return kyubu::L(); }

// luaL_tolstring replacement for Lua 5.1 / LuaJIT: pushes tostring(v) and returns it.
const char* to_display(lua_State* L, int idx) {
    idx = idx < 0 ? lua_gettop(L) + idx + 1 : idx;
    lua_getglobal(L, "tostring");
    lua_pushvalue(L, idx);
    lua_call(L, 1, 1);
    const char* s = lua_tostring(L, -1);
    return s ? s : "";
}

struct LuaVM {
    lua_State* L = nullptr;
    KyubuSide side = KY_SIDE_NONE;
    std::recursive_mutex mu;
    int make_api_ref = LUA_NOREF;
};

struct LuaMod {
    Mod* mod = nullptr;
    KyubuSide side = KY_SIDE_NONE;
    LuaVM* vm = nullptr;
    int env_ref = LUA_NOREF;
    int api_ref = LUA_NOREF;
    std::vector<KyubuId> subs, hooks, providers;
    std::vector<std::string> commands, channels, ifaces;
};

struct LuaCb {
    LuaVM* vm;
    LuaMod* lm;
    int ref;
    std::string what;
    int errors = 0;
    uint64_t window = 0;
    bool disabled = false;
    KyubuId id = 0;
};

LuaVM* g_vm[3] = {nullptr, nullptr, nullptr};
std::map<std::pair<Mod*, int>, LuaMod*> g_mods;
std::map<std::string, uint64_t> g_script_mtimes;   // "modid|path" -> mtime

LuaVM* vm_of(lua_State* L) {
    for (int s = 1; s <= 2; ++s) if (g_vm[s] && g_vm[s]->L == L) return g_vm[s];
    return nullptr;
}

LuaMod* lua_mod(KyubuModHandle h) {
    KyubuSide side;
    Mod* m = ModManager::get().from_handle(h, &side);
    auto it = g_mods.find({m, (int)side});
    return it == g_mods.end() ? nullptr : it->second;
}

KyubuModHandle check_handle(lua_State* L, int idx) {
    if (!lua_islightuserdata(L, idx)) luaL_error(L, "invalid mod handle");
    return (KyubuModHandle)lua_touserdata(L, idx);
}

const char* result_text(KyubuResult r) {
    switch (r) {
        case KY_E_INVALID_ARG: return "invalid argument";
        case KY_E_PHASE: return "not allowed in this lifecycle phase";
        case KY_E_SIDE: return "not allowed from this side";
        case KY_E_NOT_FOUND: return "not found";
        case KY_E_UNAVAILABLE: return "unavailable on this build";
        case KY_E_CONFLICT: return "conflicts with another mod";
        case KY_E_UNSAFE: return "unsafe hook site";
        case KY_E_ACCESS: return "invalid address";
        case KY_E_NAMESPACE: return "outside the mod's namespace";
        case KY_E_DUPLICATE: return "already registered";
        case KY_E_TOO_LARGE: return "too large";
        case KY_E_VERSION: return "version requirement not met";
        case KY_E_NO_REMOTE: return "remote does not support channels";
        case KY_E_FAULTED: return "mod faulted";
        case KY_E_DEV_ONLY: return "requires dev mode";
        case KY_E_TRUST: return "not permitted at this trust level";
        case KY_E_BUFFER: return "buffer too small";
        case KY_E_GONE: return "the creature is gone";
        case KY_E_REFUSED: return "refused by the game state";
        default: return "error";
    }
}

int push_ok(lua_State* L, KyubuResult r) {
    if (r == KY_OK) { lua_pushboolean(L, 1); return 1; }
    lua_pushnil(L);
    lua_pushstring(L, result_text(r));
    return 2;
}

// Calls registry function `cb->ref` with `nargs` args on the stack. Caller holds the VM lock.
bool call_ref(LuaCb* cb, int nargs, int nres) {
    lua_State* L = cb->vm->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
    lua_insert(L, -(nargs + 1));
    lua_getglobal(L, "debug");
    lua_getfield(L, -1, "traceback");
    lua_remove(L, -2);
    int errfunc = lua_gettop(L) - nargs - 1;
    lua_insert(L, errfunc);
    int rc = lua_pcall(L, nargs, nres, errfunc);
    lua_remove(L, errfunc);
    if (rc == 0) return true;
    std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error object)";
    lua_pop(L, 1);
    Mod* m = cb->lm ? cb->lm->mod : nullptr;
    Log::get().write(KY_LOG_ERROR, m ? m->id : "lua", cb->vm->side, "Lua error in " + cb->what + ": " + err);
    uint64_t now = GetTickCount64();
    if (now - cb->window > 1000) { cb->window = now; cb->errors = 0; }
    if (++cb->errors > 20 && !cb->disabled) {
        cb->disabled = true;
        Log::get().write(KY_LOG_ERROR, m ? m->id : "lua", cb->vm->side,
                         "handler " + cb->what + " disabled after more than 20 errors within 1 second");
        if (cb->id && cb->lm) Events::get().unsubscribe(cb->lm->mod, cb->id);
    }
    return false;
}

LuaCb* new_cb(lua_State* L, int fn_index, LuaMod* lm, const std::string& what) {
    luaL_checktype(L, fn_index, LUA_TFUNCTION);
    lua_pushvalue(L, fn_index);
    auto cb = new LuaCb{vm_of(L), lm, luaL_ref(L, LUA_REGISTRYINDEX), what};
    return cb;   // intentionally never freed: native code may still hold the pointer
}

// ------------------------------------------------------------------ event payloads -> Lua tables
void set_num(lua_State* L, const char* k, double v) { lua_pushnumber(L, v); lua_setfield(L, -2, k); }
void set_str(lua_State* L, const char* k, const char* v) { if (v) { lua_pushstring(L, v); lua_setfield(L, -2, k); } }
void set_bool(lua_State* L, const char* k, bool v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); }

int ev_cancel(lua_State* L) {
    lua_getfield(L, 1, "__ev");
    auto ev = (KyubuEvent*)lua_touserdata(L, -1);
    if (ev && ev->cancellable) ev->cancelled = 1;
    return 0;
}

int ev_set_data(lua_State* L) {
    lua_getfield(L, 1, "__ev");
    auto ev = (KyubuEvent*)lua_touserdata(L, -1);
    size_t n;
    const char* s = luaL_checklstring(L, 2, &n);
    if (!ev || strcmp(ev->name, "net.send_raw") != 0) return luaL_error(L, "set_data is only valid for net.send_raw");
    auto d = (KyubuNetData*)ev->data;
    if (n > d->len) return luaL_error(L, "set_data cannot grow the buffer beyond %d bytes", (int)d->len);
    memcpy(d->mutable_data, s, n);
    *d->mutable_len = (uint32_t)n;
    return 0;
}

void push_event(lua_State* L, KyubuEvent* ev) {
    lua_newtable(L);
    set_str(L, "name", ev->name);
    set_str(L, "side", side_name(ev->side));
    set_bool(L, "cancellable", ev->cancellable != 0);
    set_bool(L, "network_thread", ev->network_thread != 0);
    lua_pushlightuserdata(L, ev);
    lua_setfield(L, -2, "__ev");
    lua_pushlightuserdata(L, ev->data);
    lua_setfield(L, -2, "raw");
    lua_pushcfunction(L, ev_cancel);
    lua_setfield(L, -2, "cancel");
    std::string n = ev->name;
    void* d = ev->data;
    if (!d) return;
    if (n == "frame.begin" || n == "frame.end" || n == "sim.tick") {
        auto f = (KyubuFrameData*)d;
        set_num(L, "frame", f->frame);
        set_num(L, "dt", f->dt);
    } else if (n == "sim.world_step") {
        auto s = (KyubuWorldStepData*)d;
        set_num(L, "world", (double)(uintptr_t)s->world);
        set_num(L, "dt", s->dt);
        set_num(L, "step_out", (double)(uintptr_t)s->step_out);
        set_num(L, "effects", (double)(uintptr_t)s->effects);
    } else if (starts_with(n, "render.")) {
        auto r = (KyubuRenderData*)d;
        lua_pushlightuserdata(L, r->device);
        lua_setfield(L, -2, "device");
        set_num(L, "width", r->width);
        set_num(L, "height", r->height);
    } else if (n == "input.key") {
        auto k = (KyubuKeyData*)d;
        set_num(L, "vk", k->vk);
        set_bool(L, "down", k->down);
        set_bool(L, "is_repeat", k->repeat);
    } else if (n == "input.char") {
        auto c = (KyubuCharData*)d;
        set_num(L, "codepoint", c->codepoint);
        set_str(L, "char", narrow(std::wstring(1, (wchar_t)c->codepoint)).c_str());
    } else if (n == "input.mouse_button") {
        auto b = (KyubuMouseButtonData*)d;
        set_num(L, "button", b->button);
        set_bool(L, "down", b->down);
        set_num(L, "x", b->x);
        set_num(L, "y", b->y);
    } else if (n == "input.mouse_move") {
        auto m = (KyubuMouseMoveData*)d;
        set_num(L, "x", m->x); set_num(L, "y", m->y); set_num(L, "dx", m->dx); set_num(L, "dy", m->dy);
    } else if (n == "input.mouse_wheel") {
        set_num(L, "delta", ((KyubuMouseWheelData*)d)->delta);
    } else if (n == "asset.request") {
        auto a = (KyubuAssetRequestData*)d;
        set_str(L, "db", a->db);
        set_str(L, "key", a->key);
    } else if (n == "asset.db_open") {
        set_str(L, "path", ((KyubuDbOpenData*)d)->path);
    } else if (n == "net.recv_raw" || n == "net.send_raw") {
        auto nd = (KyubuNetData*)d;
        set_num(L, "socket", (double)nd->socket);
        lua_pushlstring(L, (const char*)nd->data, nd->len);
        lua_setfield(L, -2, "data");
        if (n == "net.send_raw") { lua_pushcfunction(L, ev_set_data); lua_setfield(L, -2, "set_data"); }
    } else if (n == "net.connect" || n == "net.disconnect") {
        auto c = (KyubuNetConnData*)d;
        set_num(L, "socket", (double)c->socket);
        set_str(L, "address", c->address);
        set_num(L, "port", c->port);
    } else if (starts_with(n, "lifecycle.world_")) {
        set_str(L, "world", ((KyubuWorldData*)d)->world);
    } else if (n == "config.changed") {
        set_str(L, "mod", ((KyubuConfigChangedData*)d)->mod);
        set_str(L, "key", ((KyubuConfigChangedData*)d)->key);
    }
}

// Custom Lua events carry their Lua value in this registry slot while being emitted.
const char* kEmitSlot = "kyubu.emit_value";

void KYUBU_CALL event_trampoline(KyubuEvent* ev, void* user) {
    auto cb = (LuaCb*)user;
    if (cb->disabled) return;
    std::lock_guard<std::recursive_mutex> g(cb->vm->mu);
    lua_State* L = cb->vm->L;
    int top = lua_gettop(L);
    push_event(L, ev);
    lua_getfield(L, LUA_REGISTRYINDEX, kEmitSlot);
    lua_setfield(L, -2, "value");
    call_ref(cb, 1, 0);
    lua_settop(L, top);
}

// ------------------------------------------------------------------ hook lock stubs
// Every Lua function/import/vtable hook is entered through a stub that takes the VM lock and
// releases it when the handler returns, whatever the calling convention.
struct StubCtx { LuaVM* vm; void* target; };
thread_local std::vector<void*> t_ret_stack;

extern "C" void __cdecl kyubu_lua_hook_enter(StubCtx* ctx, uint32_t /*ecx*/, uint32_t /*edx*/, void* ret) {
    t_ret_stack.push_back(ret);
    t_ret_stack.push_back(ctx);
    ctx->vm->mu.lock();
}

extern "C" void* __cdecl kyubu_lua_hook_leave() {
    auto ctx = (StubCtx*)t_ret_stack.back();
    t_ret_stack.pop_back();
    void* ret = t_ret_stack.back();
    t_ret_stack.pop_back();
    ctx->vm->mu.unlock();
    return ret;
}

uint8_t* make_lock_stub(LuaVM* vm, void* target) {
    static uint8_t* pool = nullptr;
    static size_t used = 4096;
    if (used + 64 > 4096) {
        pool = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        used = 0;
    }
    auto ctx = new StubCtx{vm, target};
    uint8_t* p = pool + used;
    std::vector<uint8_t> c;
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) c.push_back(uint8_t(v >> (8 * i))); };
    auto rel = [&](void* to) { put32((uint32_t)((uintptr_t)to - ((uintptr_t)p + c.size() + 4))); };
    // ECX/EDX may carry arguments (thiscall/fastcall): keep them across the C call.
    c.push_back(0x52);                                             // push edx
    c.push_back(0x51);                                             // push ecx
    c.push_back(0x68); put32((uint32_t)(uintptr_t)ctx);          // push ctx
    c.push_back(0xE8); rel((void*)&kyubu_lua_hook_enter);          // call enter(ctx, ecx, edx, ret)
    c.insert(c.end(), {0x83, 0xC4, 0x04});                         // add esp,4
    c.push_back(0x59);                                             // pop ecx
    c.push_back(0x5A);                                             // pop edx
    size_t cont_fix = c.size() + 3;
    c.insert(c.end(), {0xC7, 0x04, 0x24}); put32(0);               // mov dword [esp], cont
    c.insert(c.end(), {0xFF, 0x25}); put32((uint32_t)(uintptr_t)&ctx->target);   // jmp [ctx->target]
    size_t cont = c.size();
    c.push_back(0x50);                                             // push eax
    c.push_back(0x52);                                             // push edx
    c.push_back(0xE8); rel((void*)&kyubu_lua_hook_leave);          // call leave -> eax = return address
    c.insert(c.end(), {0x89, 0xC1});                               // mov ecx,eax
    c.push_back(0x5A);                                             // pop edx
    c.push_back(0x58);                                             // pop eax
    c.insert(c.end(), {0xFF, 0xE1});                               // jmp ecx
    uint32_t cont_addr = (uint32_t)(uintptr_t)(p + cont);
    memcpy(&c[cont_fix], &cont_addr, 4);
    memcpy(p, c.data(), c.size());
    FlushInstructionCache(GetCurrentProcess(), p, c.size());
    used += (c.size() + 15) & ~size_t(15);
    return p;
}

// ------------------------------------------------------------------ C primitives (the K table)
int k_log(lua_State* L) {
    A()->log(check_handle(L, 1), (KyubuLogLevel)luaL_checkinteger(L, 2), luaL_checkstring(L, 3));
    return 0;
}

int k_phase(lua_State* L) {
    static const char* names[] = {"boot", "preinit", "register", "post_register", "game_ready", "shutdown"};
    lua_pushstring(L, names[A()->phase()]);
    return 1;
}

int k_host(lua_State* L) {
    KyubuHostInfo h{};
    h.size = sizeof h;
    A()->host_info(&h);
    lua_newtable(L);
    set_str(L, "host", h.host == KY_HOST_SERVER ? "server" : "client");
    set_str(L, "mode", h.mode == KY_MODE_REMOTE_CLIENT ? "remote_client" : h.mode == KY_MODE_DEDICATED ? "dedicated" : "local");
    set_str(L, "build", h.build_id);
    set_num(L, "image_base", (double)h.image_base);
    set_bool(L, "dev_mode", h.dev_mode);
    set_bool(L, "safe_mode", h.safe_mode);
    return 1;
}

int k_current_side(lua_State* L) {
    KyubuSide s = A()->current_side();
    if (s == KY_SIDE_NONE) {
        LuaVM* vm = vm_of(L);
        s = vm ? vm->side : KY_SIDE_NONE;
    }
    if (s == KY_SIDE_NONE) lua_pushnil(L); else lua_pushstring(L, side_name(s));
    return 1;
}

int k_game_dir(lua_State* L) { lua_pushstring(L, L_().game_dir.c_str()); return 1; }

int k_data_dir(lua_State* L) {
    char buf[1024];
    A()->mod_data_dir(check_handle(L, 1), buf, sizeof buf);
    lua_pushstring(L, buf);
    return 1;
}

int k_read_package_file(lua_State* L) {
    Mod* m = ModManager::get().from_handle(check_handle(L, 1));
    if (!m || !m->pkg) return 0;
    auto d = m->pkg->read(luaL_checkstring(L, 2));
    if (!d) return 0;
    lua_pushlstring(L, (const char*)d->data(), d->size());
    return 1;
}

int k_is_mod_loaded(lua_State* L) {
    char v[64];
    if (A()->is_mod_loaded(luaL_checkstring(L, 1), v, sizeof v) != KY_OK) return 0;
    lua_pushstring(L, v);
    return 1;
}

int k_manifest(lua_State* L) {
    lua_pushstring(L, session_manifest_json().c_str());
    return 1;
}

int k_mods(lua_State* L) {
    lua_newtable(L);
    int i = 1;
    for (auto& m : ModManager::get().all()) {
        lua_newtable(L);
        set_str(L, "id", m->id.c_str());
        set_str(L, "version", m->manifest.version.str().c_str());
        set_str(L, "state", mod_state_name(m->state));
        set_str(L, "reason", m->reason.c_str());
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int k_game_alloc(lua_State* L) {
    void* p = A()->game_alloc((uint32_t)luaL_checkinteger(L, 1));
    if (!p) return 0;
    lua_pushnumber(L, (double)(uintptr_t)p);
    return 1;
}
int k_game_free(lua_State* L) {
    A()->game_free((void*)(uintptr_t)luaL_checknumber(L, 1));
    return 0;
}
int k_mod_state(lua_State* L) {
    char b[32];
    if (A()->mod_state(luaL_checkstring(L, 1), b, sizeof b) != KY_OK) return 0;
    lua_pushstring(L, b);
    return 1;
}
// Names of every KyubuApi member with its side, from the loader's side-rule table (for parity checks).
int k_api_functions(lua_State* L) {
    size_t n = 0;
    const ApiSideRule* r = api_side_rules(&n);
    lua_newtable(L);
    for (size_t i = 0; i < n; ++i) {
        lua_pushstring(L, r[i].sides == KY_SIDE_CLIENT ? "client" : r[i].sides == KY_SIDE_SERVER ? "server" : "both");
        lua_setfield(L, -2, r[i].name);
    }
    return 1;
}

// kyubu.game.available(name): true, or false and the reason (full trust only; see prelude.lua).
int k_game_available(lua_State* L) {
    char why[512] = {};
    KyubuResult r = A()->game_available(luaL_checkstring(L, 1), why, sizeof why);
    lua_pushboolean(L, r == KY_OK);
    if (r == KY_OK) return 1;
    lua_pushstring(L, why[0] ? why : result_text(r));
    return 2;
}

// kyubu.game.enum(name): { {value = 0, name = "..."}, ... } in value order, or nil and the reason (full trust only).
int k_game_enum(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    char buf[512] = {};
    int64_t value = 0;
    lua_newtable(L);
    for (uint32_t i = 0;; ++i) {
        KyubuResult r = A()->game_enum(name, i, &value, buf, sizeof buf);
        if (r == KY_E_NOT_FOUND && i > 0) return 1;   // past the last value
        if (r != KY_OK) {
            lua_pushnil(L);
            lua_pushstring(L, buf[0] ? buf : result_text(r));
            return 2;
        }
        lua_createtable(L, 0, 2);
        lua_pushnumber(L, (double)value);
        lua_setfield(L, -2, "value");
        lua_pushstring(L, buf);
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, (int)i + 1);
    }
}

// ---- creatures (openspec add-entity-spawner, D6): one userdata per handle per VM, found again through a weak table
// in the registry keyed by the handle's 8 bytes, so that == and table keys work. The VM is shared by its side's mods,
// so an object remembers the mod that got it last from the API, and its methods call the game API as that mod.
const char* const kCreatureMeta = "kyubu.creature";
const char* const kCreatureObjects = "kyubu.creatures";

struct LuaCreature {
    KyubuCreature h;
    KyubuModHandle mod;
};

// The reason of a failed game-API call, for Lua: the guard's own words when it gave some.
const char* game_reason(KyubuResult r) {
    const std::string& why = game::last_refusal();
    return r != KY_E_GONE && !why.empty() ? why.c_str() : result_text(r);
}

int push_game_error(lua_State* L, KyubuResult r) {
    lua_pushnil(L);
    lua_pushstring(L, game_reason(r));
    return 2;
}

void push_creature(lua_State* L, KyubuModHandle mod, KyubuCreature h) {
    lua_getfield(L, LUA_REGISTRYINDEX, kCreatureObjects);
    lua_pushlstring(L, (const char*)&h, sizeof h);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) {
        ((LuaCreature*)lua_touserdata(L, -1))->mod = mod;
        lua_remove(L, -2);
        return;
    }
    lua_pop(L, 1);
    auto c = (LuaCreature*)lua_newuserdata(L, sizeof(LuaCreature));
    c->h = h;
    c->mod = mod;
    luaL_getmetatable(L, kCreatureMeta);
    lua_setmetatable(L, -2);
    lua_pushlstring(L, (const char*)&h, sizeof h);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4);
    lua_remove(L, -2);
}

LuaCreature* check_creature(lua_State* L, int idx, const char* fn) {
    auto c = (LuaCreature*)lua_touserdata(L, idx);
    bool ok = c && lua_getmetatable(L, idx);
    if (ok) {
        luaL_getmetatable(L, kCreatureMeta);
        ok = lua_rawequal(L, -1, -2) != 0;
        lua_pop(L, 2);
    }
    if (!ok) luaL_error(L, "%s: expected a creature, got %s", fn, luaL_typename(L, idx));
    return c;
}

// creature_get's result as a table: { id, type, hostile, level, hp, pos = {x =, y =, z =}, yaw }. Positions stay
// below 2^40 units, so they fit a Lua number exactly.
int read_creature(lua_State* L, KyubuModHandle mod, KyubuCreature h) {
    KyubuCreatureInfo info = {};
    info.size = sizeof info;
    KyubuResult r = A()->creature_get(mod, h, &info);
    if (r != KY_OK) return push_game_error(L, r);
    lua_createtable(L, 0, 7);
    lua_pushnumber(L, (lua_Number)info.id);
    lua_setfield(L, -2, "id");
    lua_pushnumber(L, info.entity_type);
    lua_setfield(L, -2, "type");
    lua_pushnumber(L, info.hostile_type);
    lua_setfield(L, -2, "hostile");
    lua_pushnumber(L, info.level);
    lua_setfield(L, -2, "level");
    lua_pushnumber(L, info.hp);
    lua_setfield(L, -2, "hp");
    lua_createtable(L, 0, 3);
    static const char* const axes[] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        lua_pushnumber(L, (lua_Number)info.pos[i]);
        lua_setfield(L, -2, axes[i]);
    }
    lua_setfield(L, -2, "pos");
    lua_pushnumber(L, info.body_yaw);
    lua_setfield(L, -2, "yaw");
    lua_createtable(L, 0, 2);
    lua_pushboolean(L, (info.flags & KY_CREATURE_UNSTOPPABLE) != 0);
    lua_setfield(L, -2, "unstoppable");
    lua_pushboolean(L, (info.flags & KY_CREATURE_INVULNERABLE) != 0);
    lua_setfield(L, -2, "invulnerable");
    lua_pushboolean(L, (info.flags & KY_CREATURE_ROOTED) != 0);
    lua_setfield(L, -2, "rooted");
    lua_setfield(L, -2, "flags");
    return 1;
}

// The failure of a ui call, for Lua: nil and the reason. KY_E_GONE is the creature's own word elsewhere, so a node says its own.
int push_ui_error(lua_State* L, KyubuResult r) {
    if (r != KY_E_GONE) return push_game_error(L, r);
    lua_pushnil(L);
    lua_pushstring(L, "no node of the UI tree has that address");
    return 2;
}

// A KyubuUiNode as a table: node and parent (addresses, 0 for the root), name, children, depth and colour {r, g, b, a}.
int push_ui_node(lua_State* L, const KyubuUiNode& n) {
    lua_createtable(L, 0, 6);
    lua_pushnumber(L, (lua_Number)n.node);
    lua_setfield(L, -2, "node");
    lua_pushnumber(L, (lua_Number)n.parent);
    lua_setfield(L, -2, "parent");
    lua_pushstring(L, n.name);
    lua_setfield(L, -2, "name");
    lua_pushnumber(L, n.children);
    lua_setfield(L, -2, "children");
    lua_pushnumber(L, n.depth);
    lua_setfield(L, -2, "depth");
    lua_createtable(L, 4, 0);
    for (int i = 0; i < 4; ++i) {
        lua_pushnumber(L, n.colour[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "colour");
    return 1;
}

// An address argument at `idx` (a number from 0 to 2^32 - 1; the game is 32-bit), or the root when it is nil or absent.
uintptr_t check_ui_address(lua_State* L, int idx, const char* fn) {
    if (lua_isnoneornil(L, idx)) return 0;
    const lua_Number n = luaL_checknumber(L, idx);
    if (!(n >= 0 && n <= 4294967295.0 && n == std::floor(n)))
        luaL_error(L, "%s: node must be an address from a node table (a whole number from 0 to 2^32 - 1), got %f", fn, n);
    return (uintptr_t)n;
}

// kyubu.game.ui_node_find(name, nth): the nth (0 first) node of the scene tree with that name, or nil and the reason.
int k_ui_node_find(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const lua_Number nth = luaL_optnumber(L, 3, 0);
    if (!(nth >= 0 && nth <= 4294967295.0 && nth == std::floor(nth)))
        return luaL_error(L, "kyubu.game.ui_node_find: nth must be a whole number from 0, got %f", nth);
    KyubuUiNode info = {};
    info.size = sizeof info;
    const KyubuResult r = A()->ui_node_find(mod, name, (uint32_t)nth, &info);
    if (r != KY_OK) return push_ui_error(L, r);
    return push_ui_node(L, info);
}

// kyubu.game.ui_node_get(node): the node at that address (nil or no argument: the root), or nil and the reason.
int k_ui_node_get(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const uintptr_t node = check_ui_address(L, 2, "kyubu.game.ui_node_get");
    KyubuUiNode info = {};
    info.size = sizeof info;
    const KyubuResult r = A()->ui_node_get(mod, node, &info);
    if (r != KY_OK) return push_ui_error(L, r);
    return push_ui_node(L, info);
}

// kyubu.game.ui_node_child(node, index): the index-th (0 first) child of the node, or nil and the reason.
int k_ui_node_child(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const uintptr_t node = check_ui_address(L, 2, "kyubu.game.ui_node_child");
    const lua_Number index = luaL_checknumber(L, 3);
    if (!(index >= 0 && index <= 4294967295.0 && index == std::floor(index)))
        return luaL_error(L, "kyubu.game.ui_node_child: index must be a whole number from 0, got %f", index);
    KyubuUiNode info = {};
    info.size = sizeof info;
    const KyubuResult r = A()->ui_node_child(mod, node, (uint32_t)index, &info);
    if (r != KY_OK) return push_ui_error(L, r);
    return push_ui_node(L, info);
}

// kyubu.game.render_get(): { device = address, width =, height = } of the game's Direct3D device, or nil and the reason.
int k_render_get(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    KyubuRenderData data = {};
    const KyubuResult r = A()->render_get(mod, &data);
    if (r != KY_OK) return push_ui_error(L, r);
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, (lua_Number)(uintptr_t)data.device);
    lua_setfield(L, -2, "device");
    lua_pushnumber(L, data.width);
    lua_setfield(L, -2, "width");
    lua_pushnumber(L, data.height);
    lua_setfield(L, -2, "height");
    return 1;
}

// A number of particle_spawn's table (at index t): the field's value, or `def` when the key is absent (NAN: the key is required).
// Anything else raises, naming the key (luaL_error takes only C strings).
lua_Number particle_number(lua_State* L, int t, const char* key, lua_Number def) {
    lua_getfield(L, t, key);
    if (lua_isnil(L, -1) && !std::isnan(def)) {
        lua_pop(L, 1);
        return def;
    }
    if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "kyubu.game.particle_spawn: '%s' must be a number", key);
    const lua_Number v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

// A triple of particle_spawn's table: {a, b, c}, or {x = , y = , z = } for `pos` and `accel`, or absent (then `def`; NULL def: required).
// `color` is the array form, and its fourth number is read by the caller.
void particle_triple(lua_State* L, int t, const char* key, const double* def, double out[3]) {
    lua_getfield(L, t, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (!def) luaL_error(L, "kyubu.game.particle_spawn: '%s' is required", key);
        for (int i = 0; i < 3; ++i) out[i] = def[i];
        return;
    }
    if (lua_type(L, -1) != LUA_TTABLE) luaL_error(L, "kyubu.game.particle_spawn: '%s' must be a table of three numbers", key);
    const int tt = lua_gettop(L);
    static const char* const names[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        lua_rawgeti(L, tt, i + 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, tt, names[i]);
        }
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "kyubu.game.particle_spawn: '%s' must hold three numbers (an array, or x, y and z)", key);
        out[i] = lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

// kyubu.game.particle_spawn{ pos = {x, y, z}, [type = 1], [count = 8], [scale = 0.25], [spreading = 3], [accel = {0, 0, 10}],
// [color = {r, g, b, a}] }: true, or nil and the reason (full trust only).
int k_particle_spawn(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    KyubuParticle p = {};
    p.size = sizeof p;
    double pos[3], accel[3];
    static const double kAccel[3] = {0, 0, 10};
    particle_triple(L, 2, "pos", nullptr, pos);
    particle_triple(L, 2, "accel", kAccel, accel);
    for (int i = 0; i < 3; ++i) {
        if (!(std::fabs(pos[i]) < 9.0e18)) luaL_error(L, "kyubu.game.particle_spawn: 'pos' is out of range");
        p.pos[i] = (int64_t)std::llround(pos[i]);
        p.accel[i] = (float)accel[i];
    }
    static const double kWhite[3] = {1, 1, 1};
    double rgb[3];
    particle_triple(L, 2, "color", kWhite, rgb);   // {r, g, b} or {r, g, b, a}: the alpha is read below
    for (int i = 0; i < 3; ++i) p.color[i] = (float)rgb[i];
    p.color[3] = 1.0f;
    lua_getfield(L, 2, "color");
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_rawgeti(L, -1, 4);
        if (lua_type(L, -1) == LUA_TNUMBER) p.color[3] = (float)lua_tonumber(L, -1);
        else {
            lua_pop(L, 1);
            lua_getfield(L, -1, "a");
            if (lua_type(L, -1) == LUA_TNUMBER) p.color[3] = (float)lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    p.scale = (float)particle_number(L, 2, "scale", 0.25);
    p.spreading = (float)particle_number(L, 2, "spreading", 3.0);
    const lua_Number type = particle_number(L, 2, "type", KY_PARTICLE_BOMB);
    const lua_Number count = particle_number(L, 2, "count", 8);
    if (!(type >= 0 && type <= 4294967295.0 && type == std::floor(type)))
        luaL_error(L, "kyubu.game.particle_spawn: 'type' must be a whole number");
    if (!(count >= 0 && count <= 4294967295.0 && count == std::floor(count)))
        luaL_error(L, "kyubu.game.particle_spawn: 'count' must be a whole number");
    p.type = (uint32_t)type;
    p.count = (uint32_t)count;
    const KyubuResult r = A()->particle_spawn(mod, &p);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// A light's table (at index t) for light_spawn and light_set: { pos = {x, y, z} (world units), radius, [color = {r, g, b}],
// [life_ms = 0], [fade_ms = 0] }. A bad field raises, naming the function and the key.
void light_from_table(lua_State* L, int t, const char* fn, KyubuLight* l) {
    luaL_checktype(L, t, LUA_TTABLE);
    *l = KyubuLight{};
    l->size = sizeof *l;
    auto number = [&](const char* key, lua_Number def) {
        lua_getfield(L, t, key);
        if (lua_isnil(L, -1) && !std::isnan(def)) {
            lua_pop(L, 1);
            return def;
        }
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "kyubu.game.%s: '%s' must be a number", fn, key);
        const lua_Number v = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return v;
    };
    auto triple = [&](const char* key, const double* def, double out[3]) {
        lua_getfield(L, t, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            if (!def) luaL_error(L, "kyubu.game.%s: '%s' is required", fn, key);
            for (int i = 0; i < 3; ++i) out[i] = def[i];
            return;
        }
        if (lua_type(L, -1) != LUA_TTABLE) luaL_error(L, "kyubu.game.%s: '%s' must be a table of three numbers", fn, key);
        const int tt = lua_gettop(L);
        static const char* const xyz[3] = {"x", "y", "z"};
        static const char* const rgb[3] = {"r", "g", "b"};
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(L, tt, i + 1);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_getfield(L, tt, def ? rgb[i] : xyz[i]);
            }
            if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "kyubu.game.%s: '%s' must hold three numbers", fn, key);
            out[i] = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    };
    double pos[3], color[3];
    static const double kWarm[3] = {1.0, 0.8, 0.6};
    triple("pos", nullptr, pos);
    triple("color", kWarm, color);
    for (int i = 0; i < 3; ++i) {
        if (!(std::fabs(pos[i]) < 9.0e18)) luaL_error(L, "kyubu.game.%s: 'pos' is out of range", fn);
        l->pos[i] = (int64_t)std::llround(pos[i]);
        l->color[i] = (float)color[i];
    }
    l->radius = (float)number("radius", NAN);
    const lua_Number life = number("life_ms", 0), fade = number("fade_ms", 0);
    if (!(life >= 0 && life <= 4294967295.0 && life == std::floor(life)) || !(fade >= 0 && fade <= 4294967295.0 && fade == std::floor(fade)))
        luaL_error(L, "kyubu.game.%s: 'life_ms' and 'fade_ms' must be whole numbers of 0 or more", fn);
    l->life_ms = (uint32_t)life;
    l->fade_ms = (uint32_t)fade;
}

uint32_t light_id(lua_State* L, int i, const char* fn) {
    const lua_Number v = luaL_checknumber(L, i);
    if (!(v >= 1 && v <= 4294967295.0 && v == std::floor(v))) luaL_error(L, "kyubu.game.%s: the id must be a whole number of 1 or more", fn);
    return (uint32_t)v;
}

// kyubu.game.light_spawn{ pos =, radius =, [color], [life_ms], [fade_ms] }: the light's id, or nil and the reason.
int k_light_spawn(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    KyubuLight l;
    light_from_table(L, 2, "light_spawn", &l);
    uint32_t id = 0;
    const KyubuResult r = A()->light_spawn(mod, &l, &id);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushnumber(L, id);
    return 1;
}

// kyubu.game.light_set(id, { the fields of light_spawn }): true, or nil and the reason.
int k_light_set(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const uint32_t id = light_id(L, 2, "light_set");
    KyubuLight l;
    light_from_table(L, 3, "light_set", &l);
    const KyubuResult r = A()->light_set(mod, id, &l);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// kyubu.game.light_remove(id): true, or nil and the reason.
int k_light_remove(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const KyubuResult r = A()->light_remove(mod, light_id(L, 2, "light_remove"));
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// kyubu.game.world_block(x, y, z): the block's type byte (the kind is type % 32) and its red, green and blue, or nil and
// "unknown" for a place in no loaded zone, or nil and the reason. x, y and z are blocks; each is rounded down.
int k_world_block(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    int32_t v[3];
    for (int i = 0; i < 3; ++i) {
        const lua_Number n = std::floor(luaL_checknumber(L, 2 + i));
        if (!(n >= -2147483648.0 && n <= 2147483647.0))
            return luaL_error(L, "kyubu.game.world_block: argument %d is out of range", 1 + i);
        v[i] = (int32_t)n;
    }
    KyubuBlock b = {};
    const KyubuResult r = A()->world_block(mod, v[0], v[1], v[2], &b);
    if (r == KY_E_NOT_FOUND) {
        lua_pushnil(L);
        lua_pushliteral(L, "unknown");
        return 2;
    }
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushinteger(L, b.type);
    lua_pushinteger(L, b.color[0]);
    lua_pushinteger(L, b.color[1]);
    lua_pushinteger(L, b.color[2]);
    return 4;
}

// kyubu.game.hit(attacker or nil, target, {damage =, stun_ms =, knock = {x, y, z}, magic =}): a hit through the game's hitCreature,
// inside a World step (sim.world_step). True when it went through, false when it was void, dodged or blocked; nil and the reason
// when refused. damage is required; stun_ms 0, knock {0, 0, 0} and magic false when absent.
lua_Number hit_number(lua_State* L, int t, const char* key, bool required) {
    lua_getfield(L, t, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (required) luaL_error(L, "kyubu.game.hit: '%s' is required", key);
        return 0;
    }
    if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "kyubu.game.hit: '%s' must be a number", key);
    const lua_Number v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

int k_creature_hit(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const KyubuCreature attacker = lua_isnil(L, 2) ? 0 : check_creature(L, 2, "kyubu.game.hit")->h;
    const KyubuCreature target = check_creature(L, 3, "kyubu.game.hit")->h;
    luaL_checktype(L, 4, LUA_TTABLE);
    KyubuHit hit = {};
    hit.size = sizeof hit;
    hit.damage = (float)hit_number(L, 4, "damage", true);
    const lua_Number stun = hit_number(L, 4, "stun_ms", false);
    if (!(stun >= 0 && stun <= KY_HIT_STUN_MAX)) return luaL_error(L, "kyubu.game.hit: 'stun_ms' must be 0 to %d", KY_HIT_STUN_MAX);
    hit.stun_ms = (int32_t)std::lround(stun);
    lua_getfield(L, 4, "knock");
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TTABLE) return luaL_error(L, "kyubu.game.hit: 'knock' must be a table of three numbers");
        static const char* const names[3] = {"x", "y", "z"};
        const int kt = lua_gettop(L);
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(L, kt, i + 1);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_getfield(L, kt, names[i]);
            }
            if (lua_type(L, -1) != LUA_TNUMBER) return luaL_error(L, "kyubu.game.hit: 'knock' must be a table of three numbers");
            hit.knock[i] = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 4, "magic");
    if (lua_toboolean(L, -1)) hit.flags |= KY_HIT_MAGIC;
    lua_pop(L, 1);
    int landed = 0;
    const KyubuResult r = A()->creature_hit(mod, attacker, target, &hit, &landed);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, landed);
    return 1;
}

// kyubu.game.creature_player(): the client side's own creature, or nil and the reason (full trust only).
int k_creature_player(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    KyubuCreature h = 0;
    KyubuResult r = A()->creature_player(mod, &h);
    if (r != KY_OK) return push_game_error(L, r);
    push_creature(L, mod, h);
    return 1;
}

// kyubu.game.creature_find(id): the creature with that game id as an object of this side, or nil and the reason.
int k_creature_find(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const lua_Number n = luaL_checknumber(L, 2);
    if (!(n == std::floor(n) && std::fabs(n) < 9007199254740992.0))
        return luaL_error(L, "kyubu.game.creature_find: id must be a whole number, got %f", n);
    KyubuCreature h = 0;
    KyubuResult r = A()->creature_find(mod, (int64_t)n, &h);
    if (r != KY_OK) return push_game_error(L, r);
    push_creature(L, mod, h);
    return 1;
}

// A field of creature_give_item's table (at index t): an integer from lo to hi, or `def` when the key is absent (a required key
// has def = NAN). Anything else raises, naming the key. Only C strings here: luaL_error does not return.
lua_Number item_integer(lua_State* L, int t, const char* key, lua_Number lo, lua_Number hi, lua_Number def) {
    lua_getfield(L, t, key);
    if (lua_isnil(L, -1) && !std::isnan(def)) {
        lua_pop(L, 1);
        return def;
    }
    const lua_Number v = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : NAN;
    if (!(v >= lo && v <= hi && v == std::floor(v)))
        luaL_error(L, "kyubu.game.creature_give_item: '%s' must be an integer from %.0f to %.0f", key, lo, hi);
    lua_pop(L, 1);
    return v;
}

// Reads creature_give_item's item table at index t into out (size set).
void read_item(lua_State* L, int t, KyubuItem* out) {
    luaL_checktype(L, t, LUA_TTABLE);
    *out = {};
    out->size = sizeof *out;
    out->type = (uint8_t)item_integer(L, t, "type", 1, KY_ITEM_TYPE_MAX, NAN);
    out->sub_type = (uint8_t)item_integer(L, t, "sub_type", 0, 255, 0);
    out->rarity = (uint8_t)item_integer(L, t, "rarity", 0, 4, 0);
    out->material = (uint8_t)item_integer(L, t, "material", 0, 255, 0);
    out->modifier = (uint32_t)item_integer(L, t, "modifier", 0, 4294967295.0, 0);
    out->level = (int32_t)item_integer(L, t, "level", 1, 32767, 1);
}

// kyubu.game.creature_give_item(c, item), also c:give_item(item): true, or nil and the reason.
int k_creature_give_item(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    KyubuCreature h = check_creature(L, 2, "kyubu.game.creature_give_item")->h;
    KyubuItem item;
    read_item(L, 3, &item);
    const KyubuResult r = A()->creature_give_item(mod, h, &item);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}
int creature_give_item_method(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "creature:give_item");
    KyubuItem item;
    read_item(L, 2, &item);
    const KyubuResult r = A()->creature_give_item(c->mod, c->h, &item);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// kyubu.game.creature_get(c), also c:get(): the creature's state, or nil and the reason (full trust only).
int k_creature_get(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    return read_creature(L, mod, check_creature(L, 2, "kyubu.game.creature_get")->h);
}
int creature_get_method(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "creature:get");
    return read_creature(L, c->mod, c->h);
}

// Moves a creature this side owns to x, y, z (whole units; the numbers start at stack index `first`): true, or nil and the reason.
int teleport_creature(lua_State* L, KyubuModHandle mod, KyubuCreature h, int first) {
    int64_t pos[3];
    for (int i = 0; i < 3; ++i) {
        const lua_Number n = luaL_checknumber(L, first + i);
        if (!(n == std::floor(n) && std::fabs(n) < 9007199254740992.0))
            return luaL_error(L, "creature teleport: x, y and z must be whole numbers of units, got %f", n);
        pos[i] = (int64_t)n;
    }
    const KyubuResult r = A()->creature_teleport(mod, h, pos);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}
// kyubu.game.creature_teleport(c, x, y, z), also c:teleport(x, y, z).
int k_creature_teleport(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    return teleport_creature(L, mod, check_creature(L, 2, "kyubu.game.creature_teleport")->h, 3);
}
int creature_teleport_method(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "creature:teleport");
    return teleport_creature(L, c->mod, c->h, 2);
}

// Turns a creature this side owns to the yaw at stack index `idx` (degrees, counter-clockwise from north): true, or nil and the reason.
int set_creature_yaw(lua_State* L, KyubuModHandle mod, KyubuCreature h, int idx) {
    const KyubuResult r = A()->creature_set_yaw(mod, h, (float)luaL_checknumber(L, idx));
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}
// kyubu.game.creature_set_yaw(c, yaw), also c:set_yaw(yaw).
int k_creature_set_yaw(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    return set_creature_yaw(L, mod, check_creature(L, 2, "kyubu.game.creature_set_yaw")->h, 3);
}
int creature_set_yaw_method(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "creature:set_yaw");
    return set_creature_yaw(L, c->mod, c->h, 2);
}

// Sets a creature's flags from the table at stack index `idx` ({unstoppable = true, invulnerable = true}; a missing key is false): true, or
// nil and the reason.
int set_creature_flags(lua_State* L, KyubuModHandle mod, KyubuCreature h, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    uint32_t flags = 0;
    static const struct { const char* key; uint32_t bit; } kKeys[] = {{"unstoppable", KY_CREATURE_UNSTOPPABLE}, {"invulnerable", KY_CREATURE_INVULNERABLE}, {"rooted", KY_CREATURE_ROOTED}};
    for (const auto& k : kKeys) {
        lua_getfield(L, idx, k.key);
        if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TBOOLEAN)
            return luaL_error(L, "creature_set_flags: '%s' must be a boolean", k.key);
        if (lua_toboolean(L, -1)) flags |= k.bit;
        lua_pop(L, 1);
    }
    const KyubuResult r = A()->creature_set_flags(mod, h, flags);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}
// kyubu.game.creature_set_flags(c, flags), also c:set_flags(flags).
int k_creature_set_flags(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    return set_creature_flags(L, mod, check_creature(L, 2, "kyubu.game.creature_set_flags")->h, 3);
}
int creature_set_flags_method(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "creature:set_flags");
    return set_creature_flags(L, c->mod, c->h, 2);
}

// ---- creature state (add-skills-unleashed-multiplayer 10.3) ----
const char* const kXyz[] = {"x", "y", "z"};

void push_vec3(lua_State* L, const float* v, const char* name) {
    lua_createtable(L, 0, 3);
    for (int i = 0; i < 3; ++i) {
        lua_pushnumber(L, v[i]);
        lua_setfield(L, -2, kXyz[i]);
    }
    lua_setfield(L, -2, name);
}

// creature_state_get's result as a table: { pos = {x, y, z} (units), velocity, knock, aim = {x, y, z}, box = {x, y, z}, mode,
// mode_ms, stun_ms, slowed_ms, charge }.
int read_creature_state(lua_State* L, KyubuModHandle mod, KyubuCreature h) {
    KyubuCreatureState s = {};
    s.size = sizeof s;
    const KyubuResult r = A()->creature_state_get(mod, h, &s);
    if (r != KY_OK) return push_game_error(L, r);
    lua_createtable(L, 0, 10);
    lua_createtable(L, 0, 3);
    for (int i = 0; i < 3; ++i) {
        lua_pushnumber(L, (lua_Number)s.pos[i]);
        lua_setfield(L, -2, kXyz[i]);
    }
    lua_setfield(L, -2, "pos");
    push_vec3(L, s.velocity, "velocity");
    push_vec3(L, s.knock, "knock");
    push_vec3(L, s.aim, "aim");
    push_vec3(L, s.box, "box");
    const struct { const char* key; lua_Number v; } nums[] = {{"mode", (lua_Number)s.mode}, {"mode_ms", (lua_Number)s.mode_ms},
        {"stun_ms", (lua_Number)s.stun_ms}, {"slowed_ms", (lua_Number)s.slowed_ms}, {"charge", (lua_Number)s.charge}};
    for (const auto& n : nums) {
        lua_pushnumber(L, n.v);
        lua_setfield(L, -2, n.key);
    }
    return 1;
}

// Reads the {x, y, z} table at field `key` of the table at `idx` into v; false when the key is absent. A part left out keeps v's.
bool get_vec3(lua_State* L, int idx, const char* key, double* v) {
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    if (!lua_istable(L, -1)) luaL_error(L, "creature_state_set: '%s' must be a table {x =, y =, z =}", key);
    for (int i = 0; i < 3; ++i) {
        lua_getfield(L, -1, kXyz[i]);
        if (!lua_isnil(L, -1)) v[i] = luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return true;
}

// Writes the fields the table at `idx` has (the keys of read_creature_state); the others keep their value: true, or nil and the reason.
int write_creature_state(lua_State* L, KyubuModHandle mod, KyubuCreature h, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    KyubuCreatureState s = {};
    s.size = sizeof s;
    const KyubuResult got = A()->creature_state_get(mod, h, &s);   // the parts of a vector left out keep the creature's
    if (got != KY_OK) return push_game_error(L, got);
    uint32_t f = 0;
    double v[3] = {(double)s.pos[0], (double)s.pos[1], (double)s.pos[2]};
    if (get_vec3(L, idx, "pos", v)) {
        f |= KY_STATE_POS;
        for (int i = 0; i < 3; ++i) s.pos[i] = (int64_t)v[i];
    }
    const struct { const char* key; float* p; uint32_t bit; } vecs[] = {
        {"velocity", s.velocity, KY_STATE_VELOCITY}, {"knock", s.knock, KY_STATE_KNOCK}, {"aim", s.aim, KY_STATE_AIM}};
    for (const auto& e : vecs) {
        double w[3] = {e.p[0], e.p[1], e.p[2]};
        if (!get_vec3(L, idx, e.key, w)) continue;
        f |= e.bit;
        for (int i = 0; i < 3; ++i) e.p[i] = (float)w[i];
    }
    const struct { const char* key; uint32_t bit; } nums[] = {{"mode", KY_STATE_MODE}, {"mode_ms", KY_STATE_MODE_MS},
        {"stun_ms", KY_STATE_STUN}, {"slowed_ms", KY_STATE_SLOWED}, {"charge", KY_STATE_CHARGE}};
    for (const auto& n : nums) {
        lua_getfield(L, idx, n.key);
        if (!lua_isnil(L, -1)) {
            const lua_Number x = luaL_checknumber(L, -1);
            f |= n.bit;
            switch (n.bit) {
                case KY_STATE_MODE: s.mode = (uint32_t)x; break;
                case KY_STATE_MODE_MS: s.mode_ms = (int32_t)x; break;
                case KY_STATE_STUN: s.stun_ms = (int32_t)x; break;
                case KY_STATE_SLOWED: s.slowed_ms = (int32_t)x; break;
                default: s.charge = (float)x; break;
            }
        }
        lua_pop(L, 1);
    }
    s.fields = f;
    const KyubuResult r = A()->creature_state_set(mod, h, &s);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}
// kyubu.game.creature_state_get(c), also c:state(); kyubu.game.creature_state_set(c, t), also c:set_state(t).
int k_creature_state(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    return read_creature_state(L, mod, check_creature(L, 2, "kyubu.game.creature_state_get")->h);
}
int creature_state_method(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "creature:state");
    return read_creature_state(L, c->mod, c->h);
}
int k_creature_set_state(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    return write_creature_state(L, mod, check_creature(L, 2, "kyubu.game.creature_state_set")->h, 3);
}
// kyubu.game.shots(): the projectile packets the joined clients sent since the last call, oldest first, as a list of
// { connection =, data = the 0x70 bytes as a string }; an empty list when none. nil and the reason when refused. Server.exe only.
int k_shot_take(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    constexpr uint32_t kBatch = 32;
    KyubuShot buf[kBatch];
    lua_newtable(L);
    int n = 0;
    for (;;) {
        for (auto& s : buf) s.size = sizeof s;
        uint32_t got = 0;
        const KyubuResult r = A()->shot_take(mod, buf, kBatch, &got);
        if (r != KY_OK) {
            lua_pop(L, 1);
            return push_game_error(L, r);
        }
        for (uint32_t i = 0; i < got; ++i) {
            lua_createtable(L, 0, 2);
            lua_pushinteger(L, buf[i].connection);
            lua_setfield(L, -2, "connection");
            lua_pushlstring(L, (const char*)buf[i].data, KY_SHOT_DATA_SIZE);
            lua_setfield(L, -2, "data");
            lua_rawseti(L, -2, ++n);
        }
        if (got < kBatch) break;
    }
    return 1;
}

// kyubu.game.projectile_push(data): data is a cube::Projectile as a 0x70-byte string (a shot's data, edited). True, or nil and the
// reason. Inside a World step, Server.exe only.
int k_projectile_push(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    const KyubuResult r = A()->projectile_push(mod, (const uint8_t*)data, (uint32_t)len);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

int creature_set_state_method(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "creature:set_state");
    return write_creature_state(L, c->mod, c->h, 2);
}

// ---- creature bodies (1.4) ----
const char* const kPartNames[] = {"root", "body", "head", "hand_r", "hand_l", "tail"};

int body_part_arg(lua_State* L, int idx, const char* fn) {
    const char* s = luaL_checkstring(L, idx);
    for (int i = 0; i < 6; ++i)
        if (!strcmp(s, kPartNames[i])) return i;
    return luaL_error(L, "%s: part must be root, body, head, hand_r, hand_l or tail, got '%s'", fn, s);
}

// kyubu.game.body_define{name =, rig = "<text>", pieces = {node = slot, ...}, clips = {name = "<text>", ...}}: the body id, or nil and the reason.
int k_body_define(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, "name");
    const std::string name = luaL_optstring(L, -1, "body");
    lua_pop(L, 1);
    lua_getfield(L, 2, "rig");
    if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "body_define: rig must be the text of a .rig.toml");
    const std::string rig = lua_tostring(L, -1);
    lua_pop(L, 1);
    std::vector<std::string> keep;   // owns the strings the C structs point to
    keep.reserve(256);
    std::vector<KyubuBodyPiece> pieces;
    std::vector<KyubuBodyClip> clips;
    lua_getfield(L, 2, "pieces");
    if (lua_istable(L, -1)) {
        for (lua_pushnil(L); lua_next(L, -2);) {
            if (lua_type(L, -2) != LUA_TSTRING || !lua_isnumber(L, -1)) return luaL_error(L, "body_define: pieces is {node name = model slot}");
            keep.push_back(lua_tostring(L, -2));
            pieces.push_back({nullptr, (uint32_t)lua_tonumber(L, -1)});
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    const size_t first_clip = keep.size();
    lua_getfield(L, 2, "clips");
    if (lua_istable(L, -1)) {
        for (lua_pushnil(L); lua_next(L, -2);) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "body_define: clips is {clip name = .anim.toml text}");
            keep.push_back(lua_tostring(L, -2));
            keep.push_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (keep.size() > 250) return luaL_error(L, "body_define: too many pieces and clips");
    for (size_t i = 0; i < pieces.size(); ++i) pieces[i].node = keep[i].c_str();
    for (size_t i = first_clip; i + 1 < keep.size(); i += 2) clips.push_back({keep[i].c_str(), keep[i + 1].c_str()});
    KyubuBodyDesc d = {};
    d.size = sizeof d;
    d.name = name.c_str();
    d.rig_text = rig.c_str();
    d.piece_count = (uint32_t)pieces.size();
    d.pieces = pieces.data();
    d.clip_count = (uint32_t)clips.size();
    d.clips = clips.data();
    KyubuBody b = 0;
    char why[512] = {};
    const KyubuResult r = A()->body_define(mod, &d, &b, why, sizeof why);
    if (r != KY_OK) {
        lua_pushnil(L);
        lua_pushstring(L, why[0] ? why : game_reason(r));
        return 2;
    }
    lua_pushnumber(L, b);
    return 1;
}

// kyubu.game.body_name(body): {pieces = {...}, clips = {...}}, or nil and the reason.
int k_body_name(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const KyubuBody b = (KyubuBody)luaL_checknumber(L, 2);
    lua_newtable(L);
    for (uint32_t kind = 0; kind < 2; ++kind) {
        lua_newtable(L);
        char buf[96];
        for (uint32_t i = 0;; ++i) {
            const KyubuResult r = A()->body_name(mod, b, kind, i, buf, sizeof buf);
            if (r == KY_E_NOT_FOUND && i > 0) break;
            if (r == KY_E_NOT_FOUND && kind == 0 && i == 0) {   // no piece: an empty list, unless the body does not exist
                char probe[8];
                if (A()->body_name(mod, b, 1, 0, probe, sizeof probe) == KY_E_NOT_FOUND && A()->body_name(mod, b, 0, 0, probe, sizeof probe) == KY_E_NOT_FOUND) {
                    lua_pop(L, 2);
                    lua_pushnil(L);
                    lua_pushstring(L, "no such body");
                    return 2;
                }
                break;
            }
            if (r == KY_E_NOT_FOUND) break;
            if (r != KY_OK) {
                lua_pop(L, 2);
                return push_game_error(L, r);
            }
            lua_pushstring(L, buf);
            lua_rawseti(L, -2, (int)i + 1);
        }
        lua_setfield(L, -2, kind == 0 ? "pieces" : "clips");
    }
    return 1;
}

// kyubu.game.creature_body_attach(c, body, scale): body nil detaches.
int k_creature_body_attach(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    LuaCreature* c = check_creature(L, 2, "kyubu.game.creature_body_attach");
    const KyubuBody b = lua_isnoneornil(L, 3) ? 0 : (KyubuBody)luaL_checknumber(L, 3);
    const float scale = b ? (float)luaL_checknumber(L, 4) : 1.0f;
    const KyubuResult r = A()->creature_body_attach(mod, c->h, b, scale);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// kyubu.game.creature_body_play(c, clip, {start =, loop =, fade =})
int k_creature_body_play(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    LuaCreature* c = check_creature(L, 2, "kyubu.game.creature_body_play");
    const char* clip = luaL_checkstring(L, 3);
    float start = 0, fade = 0;
    uint32_t flags = 0;
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "start");
        if (!lua_isnil(L, -1)) start = (float)luaL_checknumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 4, "fade");
        if (!lua_isnil(L, -1)) fade = (float)luaL_checknumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 4, "loop");
        if (lua_toboolean(L, -1)) flags |= KY_BODY_LOOP;
        lua_pop(L, 1);
    }
    const KyubuResult r = A()->creature_body_play(mod, c->h, clip, start, flags, fade);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// kyubu.game.creature_body_offset(c, part, channel, {x, y, z}, group); creature_body_offset(c) clears every offset.
int k_creature_body_offset(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    LuaCreature* c = check_creature(L, 2, "kyubu.game.creature_body_offset");
    KyubuResult r;
    if (lua_isnoneornil(L, 3)) {
        r = A()->creature_body_offset(mod, c->h, 0, 0, nullptr, 0);
    } else {
        const int part = body_part_arg(L, 3, "creature_body_offset");
        const char* ch = luaL_checkstring(L, 4);
        if (strcmp(ch, "pos") && strcmp(ch, "rot")) return luaL_error(L, "creature_body_offset: channel must be 'pos' or 'rot'");
        luaL_checktype(L, 5, LUA_TTABLE);
        float v[3];
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(L, 5, i + 1);
            v[i] = (float)luaL_checknumber(L, -1);
            lua_pop(L, 1);
        }
        const uint32_t group = (uint32_t)luaL_optnumber(L, 6, 0);
        r = A()->creature_body_offset(mod, c->h, (uint32_t)part, strcmp(ch, "pos") ? 1u : 0u, v, group);
    }
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

int k_creature_body_weight(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    LuaCreature* c = check_creature(L, 2, "kyubu.game.creature_body_weight");
    const KyubuResult r = A()->creature_body_weight(mod, c->h, (uint32_t)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// kyubu.game.creature_body_read(c): {clip =, time =, fade_left =, loop =, markers = {"name", ...}}
int k_creature_body_read(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    LuaCreature* c = check_creature(L, 2, "kyubu.game.creature_body_read");
    KyubuBodyState st = {};
    st.size = sizeof st;
    KyubuBodyMarker markers[32];
    uint32_t n = 0;
    const KyubuResult r = A()->creature_body_read(mod, c->h, &st, markers, 32, &n);
    if (r != KY_OK) return push_game_error(L, r);
    lua_newtable(L);
    lua_pushnumber(L, st.clip);
    lua_setfield(L, -2, "clip");
    lua_pushnumber(L, st.time_ms);
    lua_setfield(L, -2, "time");
    lua_pushnumber(L, st.fade_left_ms);
    lua_setfield(L, -2, "fade_left");
    lua_pushboolean(L, (st.flags & KY_BODY_LOOP) != 0);
    lua_setfield(L, -2, "loop");
    lua_newtable(L);
    for (uint32_t i = 0; i < n; ++i) {
        lua_pushstring(L, markers[i].name);
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "markers");
    return 1;
}

// kyubu.game.creature_body_point(c, piece, {x, y, z}, clip, t_ms): x, y, z in blocks.
int k_creature_body_point(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    LuaCreature* c = check_creature(L, 2, "kyubu.game.creature_body_point");
    const char* piece = luaL_checkstring(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    float p[3], out[3];
    for (int i = 0; i < 3; ++i) {
        lua_rawgeti(L, 4, i + 1);
        p[i] = (float)luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
    const char* clip = lua_isnoneornil(L, 5) ? nullptr : luaL_checkstring(L, 5);
    const float t = (float)luaL_optnumber(L, 6, 0);
    const KyubuResult r = A()->creature_body_point(mod, c->h, piece, p, clip, t, out);
    if (r != KY_OK) return push_game_error(L, r);
    for (int i = 0; i < 3; ++i) lua_pushnumber(L, out[i]);
    return 3;
}

// An ability argument at idx: its number (0 to 255) or its id ("demo:burst"). Returns false, with the reason in `why`, for an id
// that has no number. Only C strings here: luaL_error does not return.
bool ability_arg(lua_State* L, int idx, const char* fn, uint32_t* out, char (&why)[160]) {
    if (lua_type(L, idx) == LUA_TSTRING) {
        const char* id = lua_tostring(L, idx);
        const KyubuResult r = A()->ability_number(id, out);
        if (r != KY_OK) snprintf(why, sizeof why, "no ability \"%s\" (%s)", id, result_text(r));
        return r == KY_OK;
    }
    const lua_Number n = luaL_checknumber(L, idx);
    if (!(n >= 0 && n <= 255 && n == std::floor(n)))
        luaL_error(L, "%s: the ability must be a whole number from 0 to 255, or its id, got %f", fn, n);
    *out = (uint32_t)n;
    return true;
}

// kyubu.game.ability_get(c), also c:ability(): { number =, id =, elapsed_ms = } for the ability the creature is in (number 0 and
// an empty id for none), or nil and the reason.
int ability_read(lua_State* L, KyubuModHandle mod, KyubuCreature h) {
    KyubuAbilityInfo info = {};
    info.size = sizeof info;
    const KyubuResult r = A()->ability_get(mod, h, &info);
    if (r != KY_OK) return push_game_error(L, r);
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, info.number);
    lua_setfield(L, -2, "number");
    lua_pushstring(L, info.id);
    lua_setfield(L, -2, "id");
    lua_pushnumber(L, info.elapsed_ms);
    lua_setfield(L, -2, "elapsed_ms");
    return 1;
}
int k_ability_get(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    return ability_read(L, mod, check_creature(L, 2, "kyubu.game.ability_get")->h);
}
int creature_ability_method(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "creature:ability");
    return ability_read(L, c->mod, c->h);
}

// kyubu.game.ability_cooldown(c, ability), also c:cooldown(ability): the milliseconds of cooldown left, or nil and the reason.
int ability_cooldown_of(lua_State* L, KyubuModHandle mod, KyubuCreature h, int arg, const char* fn) {
    uint32_t number = 0, ms = 0;
    char why[160] = {};
    if (!ability_arg(L, arg, fn, &number, why)) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    const KyubuResult r = A()->ability_cooldown(mod, h, number, &ms);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushnumber(L, ms);
    return 1;
}
int k_ability_cooldown(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    return ability_cooldown_of(L, mod, check_creature(L, 2, "kyubu.game.ability_cooldown")->h, 3, "kyubu.game.ability_cooldown");
}
int creature_cooldown_method(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "creature:cooldown");
    return ability_cooldown_of(L, c->mod, c->h, 2, "creature:cooldown");
}

// kyubu.game.ability_cooldown_set(c, ability, ms), also c:set_cooldown(ability, ms): true, or nil and the reason.
int ability_cooldown_set_of(lua_State* L, KyubuModHandle mod, KyubuCreature h, int arg, const char* fn) {
    uint32_t number = 0;
    char why[160] = {};
    if (!ability_arg(L, arg, fn, &number, why)) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    const lua_Number ms = luaL_checknumber(L, arg + 1);
    if (!(ms >= 0 && ms <= KY_ABILITY_COOLDOWN_MAX) || ms != (lua_Number)(uint32_t)ms) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: ms must be a whole number from 0 to %d", fn, (int)KY_ABILITY_COOLDOWN_MAX);
        return 2;
    }
    const KyubuResult r = A()->ability_cooldown_set(mod, h, number, (uint32_t)ms);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}
int k_ability_cooldown_set(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    return ability_cooldown_set_of(L, mod, check_creature(L, 2, "kyubu.game.ability_cooldown_set")->h, 3,
                                   "kyubu.game.ability_cooldown_set");
}
int creature_set_cooldown_method(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "creature:set_cooldown");
    return ability_cooldown_set_of(L, c->mod, c->h, 2, "creature:set_cooldown");
}

// kyubu.game.ability_start(c, ability): true, or nil and the reason (a refusal says why: on cooldown, not enough mp, busy).
int k_ability_start(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    KyubuCreature h = check_creature(L, 2, "kyubu.game.ability_start")->h;
    uint32_t number = 0;
    char why[160] = {};
    if (!ability_arg(L, 3, "kyubu.game.ability_start", &number, why)) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    const KyubuResult r = A()->ability_start(mod, h, number);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// kyubu.game.ability_end(c): true, or nil and the reason.
int k_ability_end(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const KyubuResult r = A()->ability_end(mod, check_creature(L, 2, "kyubu.game.ability_end")->h);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// kyubu.game.ability_rank_get(ability): the local player's rank in a mod ability, or nil and the reason.
int k_ability_rank_get(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    uint32_t number = 0, rank = 0;
    char why[160] = {};
    if (!ability_arg(L, 2, "kyubu.game.ability_rank_get", &number, why)) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    const KyubuResult r = A()->ability_rank_get(mod, number, &rank);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushnumber(L, rank);
    return 1;
}

// kyubu.game.ability_rank_set(ability, rank): the rank stored (clamped to 0 up to the ability's maximum), or nil and the reason.
int k_ability_rank_set(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    uint32_t number = 0, stored = 0;
    char why[160] = {};
    if (!ability_arg(L, 2, "kyubu.game.ability_rank_set", &number, why)) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    const lua_Number n = luaL_checknumber(L, 3);
    if (!(n >= 0 && n <= 65535 && n == std::floor(n)))
        luaL_error(L, "kyubu.game.ability_rank_set: the rank must be a whole number from 0 to 65535, got %f", n);
    const KyubuResult r = A()->ability_rank_set(mod, number, (uint32_t)n, &stored);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushnumber(L, stored);
    return 1;
}

// kyubu.game.animation_define_from_text(name, text): true, or nil and the errors (one per line). The editor's calls: dev mode and
// full trust (docs/game-api.md).
int k_animation_define_from_text(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const char* name = luaL_checkstring(L, 2);
    size_t n = 0;
    const char* text = luaL_checklstring(L, 3, &n);
    if (strlen(text) != n) luaL_error(L, "kyubu.game.animation_define_from_text: the text has a NUL byte");
    char err[4096] = {};
    const KyubuResult r = A()->animation_define_from_text(mod, name, text, err, sizeof err);
    if (r == KY_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, err[0] ? err : game_reason(r));
    return 2;
}

// kyubu.game.animation_preview(clip_id, t_ms): true, or nil and the reason.
int k_animation_preview(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const char* id = luaL_checkstring(L, 2);
    const lua_Number t = luaL_checknumber(L, 3);
    const KyubuResult r = A()->animation_preview(mod, id, t);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// kyubu.game.animation_preview_clear(): true, or nil and the reason.
int k_animation_preview_clear(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const KyubuResult r = A()->animation_preview_clear(mod);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// K.ability_animation_bind(handle, ability_id, clip_id): true, or nil and the reason (kyubu.animation.bind). Any trust level.
int k_ability_animation_bind(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const char* ability = luaL_checkstring(L, 2);
    const char* clip = luaL_checkstring(L, 3);
    const KyubuResult r = A()->ability_animation_bind(mod, ability, clip);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// K.ability_animation_bind_for(handle, ability_id, creature_id, clip_id or nil): true, or nil and the reason
// (kyubu.animation.bind_for). Any trust level.
int k_ability_animation_bind_for(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const char* ability = luaL_checkstring(L, 2);
    const lua_Number id = luaL_checknumber(L, 3);
    if (!(id >= 1) || id != std::floor(id) || id > 9007199254740992.0) return luaL_error(L, "kyubu.animation.bind_for: the creature id must be a whole number from 1");
    const char* clip = lua_isnoneornil(L, 4) ? nullptr : luaL_checkstring(L, 4);
    const KyubuResult r = A()->ability_animation_bind_for(mod, ability, (uint64_t)id, clip);
    if (r != KY_OK) return push_game_error(L, r);
    lua_pushboolean(L, 1);
    return 1;
}

// Raises "kyubu.game.creature_spawn: <what>, got <the value at idx, or its type>". Only C strings here: luaL_error
// does not return, so nothing that needs a destructor may be live.
int spawn_field_error(lua_State* L, int idx, const char* what) {
    if (lua_type(L, idx) == LUA_TNUMBER)
        return luaL_error(L, "kyubu.game.creature_spawn: %s, got %f", what, lua_tonumber(L, idx));
    if (lua_type(L, idx) == LUA_TSTRING)
        return luaL_error(L, "kyubu.game.creature_spawn: %s, got \"%s\"", what, lua_tostring(L, idx));
    return luaL_error(L, "kyubu.game.creature_spawn: %s, got %s", what, luaL_typename(L, idx));
}

// A field of creature_spawn's table (at index t) that must be an integer from lo to hi; anything else raises, naming
// the key.
lua_Number spawn_integer(lua_State* L, int t, const char* key, lua_Number lo, lua_Number hi) {
    lua_getfield(L, t, key);
    const lua_Number v = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : NAN;
    if (!(v >= lo && v <= hi && v == std::floor(v))) {
        char what[96];
        snprintf(what, sizeof what, "'%s' must be an integer from %.0f to %.0f", key, lo, hi);
        spawn_field_error(L, -1, what);
    }
    lua_pop(L, 1);
    return v;
}

// The point at the top of the stack ({x, y, z} or {x =, y =, z =}, world units) into out; `what` names it in the error
// ("pos", "path[2]"). Raises on anything else. The point stays on the stack.
void spawn_point(lua_State* L, const char* what, int64_t out[3]) {
    if (!lua_istable(L, -1)) {
        char msg[80];
        snprintf(msg, sizeof msg, "'%s' must be a table {x, y, z} or {x =, y =, z =}", what);
        spawn_field_error(L, -1, msg);
    }
    static const char* const axes[] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        lua_getfield(L, -1, axes[i]);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_rawgeti(L, -1, i + 1);
        }
        const lua_Number v = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : NAN;
        if (!(std::fabs(v) < 4611686018427387904.0)) {   // 2^62
            char msg[80];
            snprintf(msg, sizeof msg, "%s.%s must be a number of world units", what, axes[i]);
            spawn_field_error(L, -1, msg);
        }
        out[i] = (int64_t)std::floor(v);   // world units: a fraction is dropped
        lua_pop(L, 1);
    }
}

// kyubu.game.creature_spawn{ type =, hostile =, level =, pos = {x, y, z} or {x =, y =, z =}, yaw =, behavior =
// "still" | "monster" | "friendly" | "companion" | "guard" | "default", owner = creature (a companion), path =
// { {x, y, z}, ... } (a guard) }: the new creature, or nil and the reason (server side, full trust only). A field of
// the wrong type raises; a value the loader refuses (not an entity type, hostile 0, level 0, a companion without an
// owner, a guard without a path) gives nil and the reason.
int k_creature_spawn(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    KyubuSpawn s = {};
    s.size = sizeof s;
    s.entity_type = (uint32_t)spawn_integer(L, 2, "type", 0, 4294967295.0);
    s.hostile_type = (uint8_t)spawn_integer(L, 2, "hostile", 0, 255);
    s.level = (int32_t)spawn_integer(L, 2, "level", -2147483648.0, 2147483647.0);
    lua_getfield(L, 2, "pos");
    spawn_point(L, "pos", s.pos);
    lua_pop(L, 1);
    lua_getfield(L, 2, "yaw");
    if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TNUMBER) spawn_field_error(L, -1, "'yaw' must be a number of degrees");
    s.body_yaw = (float)lua_tonumber(L, -1);   // nil: 0, facing north
    lua_pop(L, 1);
    lua_getfield(L, 2, "behavior");
    s.behavior = KY_SPAWN_STILL;
    if (!lua_isnil(L, -1)) {
        static const struct { const char* name; uint8_t kind; } kinds[] = {
            {"still", KY_SPAWN_STILL}, {"monster", KY_SPAWN_MONSTER}, {"friendly", KY_SPAWN_FRIENDLY},
            {"companion", KY_SPAWN_COMPANION}, {"guard", KY_SPAWN_GUARD}, {"default", KY_SPAWN_DEFAULT}};
        const char* kind = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        bool known = false;
        for (auto& k : kinds)
            if (strcmp(kind, k.name) == 0) s.behavior = k.kind, known = true;
        if (!known)
            spawn_field_error(L, -1, "'behavior' must be \"still\", \"monster\", \"friendly\", \"companion\", \"guard\" or \"default\"");
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "owner");
    if (!lua_isnil(L, -1)) s.owner = check_creature(L, -1, "kyubu.game.creature_spawn: 'owner'")->h;
    lua_pop(L, 1);
    int64_t path[KY_SPAWN_PATH_MAX][3];   // plain memory: luaL_error below does not return
    lua_getfield(L, 2, "path");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1)) spawn_field_error(L, -1, "'path' must be a list of points {x, y, z}");
        const int n = (int)lua_objlen(L, -1);
        if (n > (int)KY_SPAWN_PATH_MAX) {
            char what[64];
            snprintf(what, sizeof what, "'path' must hold at most %u points", KY_SPAWN_PATH_MAX);
            spawn_field_error(L, -1, what);
        }
        for (int i = 0; i < n; ++i) {
            char what[24];
            snprintf(what, sizeof what, "path[%d]", i + 1);
            lua_rawgeti(L, -1, i + 1);
            spawn_point(L, what, path[i]);
            lua_pop(L, 1);
        }
        s.path_count = (uint32_t)n;
        s.path = path;
    }
    lua_pop(L, 1);
    KyubuCreature h = 0;
    KyubuResult r = A()->creature_spawn(mod, &s, &h);
    if (r != KY_OK) return push_game_error(L, r);
    push_creature(L, mod, h);
    return 1;
}

// c:valid(): whether the creature still exists, without raising (any thread: it only looks at the handle tables).
int creature_valid(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "creature:valid");
    LuaVM* v = vm_of(L);
    lua_pushboolean(L, v && game::resolve_handle(v->side, c->h) != nullptr);
    return 1;
}

// tostring(c): "creature 0x<handle> (id <game id>)", or "(gone)".
int creature_tostring(lua_State* L) {
    LuaCreature* c = check_creature(L, 1, "tostring");
    std::string s = fmt("creature 0x%016llX", (unsigned long long)c->h);
    LuaVM* v = vm_of(L);
    KyubuCreatureInfo info = {};
    info.size = sizeof info;
    if (!v || !game::resolve_handle(v->side, c->h)) s += " (gone)";
    else if (A()->creature_get(c->mod, c->h, &info) == KY_OK) s += fmt(" (id %lld)", (long long)info.id);
    lua_pushstring(L, s.c_str());
    return 1;
}

// The objects' weak table and metatable, made with the VM.
void open_creatures(lua_State* L) {
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, kCreatureObjects);
    luaL_newmetatable(L, kCreatureMeta);
    static const luaL_Reg methods[] = {{"valid", creature_valid}, {"get", creature_get_method}, {"teleport", creature_teleport_method}, {"set_yaw", creature_set_yaw_method}, {"set_flags", creature_set_flags_method}, {"state", creature_state_method}, {"set_state", creature_set_state_method}, {"give_item", creature_give_item_method}, {"ability", creature_ability_method}, {"cooldown", creature_cooldown_method}, {"set_cooldown", creature_set_cooldown_method}, {nullptr, nullptr}};
    lua_newtable(L);
    luaL_register(L, nullptr, methods);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, creature_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushliteral(L, "creature");   // getmetatable(c) gives this, and setmetatable(c, ...) fails
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

// kyubu.config.get(key) / .set(key, value): the calling mod's own options, at every trust level (docs/config.md).
Mod* config_mod(lua_State* L) {
    Mod* m = ModManager::get().from_handle(check_handle(L, 1));
    if (!m) luaL_error(L, "invalid mod handle");
    return m;
}
int k_config_get(lua_State* L) {
    Mod* m = config_mod(L);
    OptValue v;
    std::string err;
    if (config_get(m, luaL_checkstring(L, 2), std::nullopt, &v, &err) != KY_OK) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    if (auto b = std::get_if<bool>(&v)) lua_pushboolean(L, *b);
    else if (auto i = std::get_if<int64_t>(&v)) lua_pushnumber(L, (lua_Number)*i);
    else if (auto f = std::get_if<double>(&v)) lua_pushnumber(L, *f);
    else lua_pushstring(L, std::get<std::string>(v).c_str());
    return 1;
}
int k_config_set(lua_State* L) {
    Mod* m = config_mod(L);
    std::string key = luaL_checkstring(L, 2);
    OptValue v;
    switch (lua_type(L, 3)) {
        case LUA_TBOOLEAN: v = lua_toboolean(L, 3) != 0; break;
        case LUA_TNUMBER: {
            double d = lua_tonumber(L, 3);
            OptValue cur;
            bool is_int = config_get(m, key, OptType::Int, &cur, nullptr) == KY_OK;
            if (is_int && d == std::floor(d) && std::fabs(d) < 9.007199254740992e15) v = (int64_t)d;
            else v = d;
            break;
        }
        case LUA_TSTRING: v = std::string(lua_tostring(L, 3)); break;
        default: return luaL_argerror(L, 3, "expected a boolean, number or string");
    }
    std::string err;
    if (config_set(m, key, v, &err) != KY_OK) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int k_stats(lua_State* L) {
    double ms = 0;
    uint32_t hooks = 0;
    if (A()->mod_stats(luaL_checkstring(L, 1), &ms, &hooks) != KY_OK) return 0;
    lua_pushnumber(L, ms);
    lua_pushinteger(L, hooks);
    return 2;
}

void KYUBU_CALL task_trampoline(void* user) {
    auto cb = (LuaCb*)user;
    std::lock_guard<std::recursive_mutex> g(cb->vm->mu);
    int top = lua_gettop(cb->vm->L);
    call_ref(cb, 0, 0);
    lua_settop(cb->vm->L, top);
}

int k_run_on_main_thread(lua_State* L) {
    auto h = check_handle(L, 1);
    auto cb = new_cb(L, 2, lua_mod(h), "run_on_main_thread");
    return push_ok(L, A()->run_on_main_thread(h, task_trampoline, cb));
}

KyubuSide side_arg(lua_State* L, int idx, KyubuModHandle h) {
    if (lua_isnoneornil(L, idx)) {
        KyubuSide s = KY_SIDE_NONE;
        ModManager::get().from_handle(h, &s);
        return s;
    }
    std::string s = luaL_checkstring(L, idx);
    if (s == "client") return KY_SIDE_CLIENT;
    if (s == "server") return KY_SIDE_SERVER;
    if (s == "both") return KY_SIDE_BOTH;
    luaL_error(L, "side must be 'client', 'server' or 'both'");
    return KY_SIDE_NONE;
}

int k_subscribe(lua_State* L) {
    auto h = check_handle(L, 1);
    std::string name = luaL_checkstring(L, 2);
    LuaMod* lm = lua_mod(h);
    auto cb = new_cb(L, 3, lm, name);
    int prio = (int)luaL_optinteger(L, 4, 0);
    KyubuSide side = side_arg(L, 5, h);
    KyubuId id = 0;
    KyubuResult r = A()->subscribe(h, name.c_str(), side, prio, event_trampoline, cb, &id);
    if (r != KY_OK) return push_ok(L, r);
    cb->id = id;
    if (lm) lm->subs.push_back(id);
    lua_pushinteger(L, id);
    return 1;
}

int k_unsubscribe(lua_State* L) {
    return push_ok(L, A()->unsubscribe(check_handle(L, 1), (KyubuId)luaL_checkinteger(L, 2)));
}

int k_emit(lua_State* L) {
    auto h = check_handle(L, 1);
    const char* name = luaL_checkstring(L, 2);
    lua_pushvalue(L, 3);
    lua_setfield(L, LUA_REGISTRYINDEX, kEmitSlot);
    int cancelled = 0;
    KyubuResult r = A()->emit(h, name, KY_SIDE_NONE, nullptr, 0, lua_toboolean(L, 4), &cancelled);
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kEmitSlot);
    if (r != KY_OK) return push_ok(L, r);
    lua_pushboolean(L, cancelled);
    return 1;
}

int k_resolve_symbol(lua_State* L) {
    uintptr_t a = 0;
    std::string why;
    KyubuResult r = SymbolTable::get().resolve(luaL_checkstring(L, 1), &a, &why);
    if (r != KY_OK) { lua_pushnil(L); lua_pushstring(L, why.empty() ? result_text(r) : why.c_str()); return 2; }
    lua_pushnumber(L, (double)a);
    return 1;
}

int k_rva_to_addr(lua_State* L) { lua_pushnumber(L, (double)A()->rva_to_addr((uint32_t)luaL_checknumber(L, 1))); return 1; }
int k_addr_to_rva(lua_State* L) { lua_pushnumber(L, (double)A()->addr_to_rva((uintptr_t)luaL_checknumber(L, 1))); return 1; }

int k_mem_read(lua_State* L) {
    uintptr_t a = (uintptr_t)luaL_checknumber(L, 1);
    size_t n = (size_t)luaL_checkinteger(L, 2);
    std::vector<char> buf(n);
    if (A()->mem_read(a, buf.data(), (uint32_t)n) != KY_OK) return push_ok(L, KY_E_ACCESS);
    lua_pushlstring(L, buf.data(), n);
    return 1;
}

int k_mem_write(lua_State* L) {
    size_t n;
    const char* s = luaL_checklstring(L, 3, &n);
    return push_ok(L, A()->mem_write(check_handle(L, 1), (uintptr_t)luaL_checknumber(L, 2), s, (uint32_t)n));
}

int k_patch_bytes(lua_State* L) {
    auto h = check_handle(L, 1);
    size_t n;
    const char* s = luaL_checklstring(L, 3, &n);
    KyubuId id = 0;
    KyubuResult r = A()->patch_bytes(h, (uintptr_t)luaL_checknumber(L, 2), s, (uint32_t)n, &id);
    if (r != KY_OK) return push_ok(L, r);
    if (auto lm = lua_mod(h)) lm->hooks.push_back(id);
    lua_pushinteger(L, id);
    return 1;
}

int k_unhook(lua_State* L) { return push_ok(L, A()->unhook(check_handle(L, 1), (KyubuId)luaL_checkinteger(L, 2))); }

int k_hook_raw(lua_State* L) {
    auto h = check_handle(L, 1);
    std::string kind = luaL_checkstring(L, 2);
    void* cb = (void*)(uintptr_t)luaL_checknumber(L, 3);
    int prio = (int)luaL_checkinteger(L, 4);
    void** cell = (void**)(uintptr_t)luaL_checknumber(L, 5);
    LuaVM* vm = vm_of(L);
    void* stub = make_lock_stub(vm, cb);
    KyubuId id = 0;
    KyubuResult r;
    if (kind == "function") r = A()->hook_function(h, (uintptr_t)luaL_checknumber(L, 6), stub, prio, cell, &id);
    else if (kind == "import") r = A()->hook_import(h, luaL_checkstring(L, 6), luaL_checkstring(L, 7), stub, prio, cell, &id);
    else if (kind == "vtable") {
        void* obj = lua_islightuserdata(L, 6) ? lua_touserdata(L, 6) : (void*)(uintptr_t)luaL_checknumber(L, 6);
        r = A()->hook_vtable(h, obj, (uint32_t)luaL_checkinteger(L, 7), stub, prio, cell, &id);
    } else return luaL_error(L, "unknown hook kind %s", kind.c_str());
    if (r != KY_OK) return push_ok(L, r);
    if (auto lm = lua_mod(h)) lm->hooks.push_back(id);
    lua_pushinteger(L, id);
    return 1;
}

int k_hook_mid(lua_State* L) {
    auto h = check_handle(L, 1);
    uintptr_t addr = (uintptr_t)luaL_checknumber(L, 2);
    LuaMod* lm = lua_mod(h);
    auto cb = new_cb(L, 3, lm, "mid hook");
    KyubuId id = 0;
    auto fn = [](KyubuCpuContext* ctx, void* user) {
        auto c = (LuaCb*)user;
        if (c->disabled) return;
        std::lock_guard<std::recursive_mutex> g(c->vm->mu);
        int top = lua_gettop(c->vm->L);
        lua_pushlightuserdata(c->vm->L, ctx);
        call_ref(c, 1, 0);
        lua_settop(c->vm->L, top);
    };
    KyubuResult r = A()->hook_mid(h, addr, fn, cb, &id);
    if (r != KY_OK) return push_ok(L, r);
    if (lm) lm->hooks.push_back(id);
    lua_pushinteger(L, id);
    return 1;
}

int k_hook_error(lua_State* L) {
    auto h = check_handle(L, 1);
    A()->log(h, KY_LOG_ERROR, (std::string("Lua error in hook handler: ") + luaL_checkstring(L, 2)).c_str());
    return 0;
}

std::string opt_payload(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return {};
    size_t n;
    const char* s = luaL_checklstring(L, idx, &n);
    return std::string(s, n);
}

int k_registry_create(lua_State* L) {
    return push_ok(L, A()->registry_create(check_handle(L, 1), luaL_checkstring(L, 2), (uint32_t)luaL_optinteger(L, 3, 1),
                                           lua_toboolean(L, 4)));
}
int k_registry_add(lua_State* L) {
    std::string p = opt_payload(L, 4);
    return push_ok(L, A()->registry_add(check_handle(L, 1), luaL_checkstring(L, 2), luaL_checkstring(L, 3), p.data(),
                                        (uint32_t)p.size(), nullptr));
}
int k_registry_remap(lua_State* L) {
    std::string p = opt_payload(L, 4);
    return push_ok(L, A()->registry_remap(check_handle(L, 1), luaL_checkstring(L, 2), luaL_checkstring(L, 3), p.data(),
                                          (uint32_t)p.size()));
}
int k_registry_lookup(lua_State* L) {
    uint32_t n = 0;
    KyubuResult r = A()->registry_lookup(luaL_checkstring(L, 1), luaL_checkstring(L, 2), &n);
    if (r != KY_OK) return push_ok(L, r);
    lua_pushinteger(L, n);
    return 1;
}
int k_registry_name(lua_State* L) {
    char buf[256];
    KyubuResult r = A()->registry_name(luaL_checkstring(L, 1), (uint32_t)luaL_checkinteger(L, 2), buf, sizeof buf);
    if (r != KY_OK) return push_ok(L, r);
    lua_pushstring(L, buf);
    return 1;
}
int k_registry_payload(lua_State* L) {
    std::vector<uint8_t> p;
    KyubuResult r = Registries::get().payload(luaL_checkstring(L, 1), luaL_checkstring(L, 2), &p);
    if (r != KY_OK) return push_ok(L, r);
    lua_pushlstring(L, (const char*)p.data(), p.size());
    return 1;
}
int k_registry_count(lua_State* L) {
    uint32_t n = 0;
    KyubuResult r = A()->registry_count(luaL_checkstring(L, 1), &n);
    if (r != KY_OK) return push_ok(L, r);
    lua_pushinteger(L, n);
    return 1;
}
int k_registry_entry(lua_State* L) {
    char buf[256];
    uint32_t n = 0;
    KyubuResult r = A()->registry_entry(luaL_checkstring(L, 1), (uint32_t)luaL_checkinteger(L, 2), buf, sizeof buf, &n);
    if (r != KY_OK) return push_ok(L, r);
    lua_pushstring(L, buf);
    lua_pushinteger(L, n);
    return 2;
}

// ---- kyubu.ability.define: the prelude hands over a table with the cooldown function already evaluated into `cooldown`.
// A field of the wrong type raises, naming it (as creature_give_item does); a value out of range is the loader's check.
const char* ability_text(lua_State* L, int t, const char* key) {
    lua_getfield(L, t, key);
    const char* s = nullptr;
    if (lua_type(L, -1) == LUA_TSTRING) s = lua_tostring(L, -1);   // the table keeps the string alive
    else if (!lua_isnil(L, -1)) luaL_error(L, "kyubu.ability.define: '%s' must be a string", key);
    lua_pop(L, 1);
    return s;
}
lua_Number ability_number(lua_State* L, int t, const char* key, lua_Number def) {
    lua_getfield(L, t, key);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return def; }
    if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "kyubu.ability.define: '%s' must be a number", key);
    const lua_Number v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}
lua_Number ability_whole(lua_State* L, int t, const char* key, lua_Number def) {
    const lua_Number v = ability_number(L, t, key, def);
    if (v != std::floor(v)) luaL_error(L, "kyubu.ability.define: '%s' must be a whole number", key);
    return v;
}
// A number as the u32 or i32 the C struct holds; anything outside that range becomes a value every field refuses.
uint32_t to_u32(lua_Number v) { return v >= 0 && v <= 4294967295.0 ? (uint32_t)v : 0xFFFFFFFFu; }
int32_t to_i32(lua_Number v) { return v >= -2147483648.0 && v <= 2147483647.0 ? (int32_t)v : 0x7FFFFFFF; }

// The callbacks of an ability that a Lua mod defined (design D6): the step runner calls the native trampolines below with
// its context, and they call the mod's functions with a ctx table. `state` is a table that lives from on_start to on_end
// for each creature that runs the ability. A Lua error is logged by call_ref with the mod and the ability, and the
// activation ends `cancelled`.
struct LuaAbility {
    LuaCb* cb[3] = {nullptr, nullptr, nullptr};   // on_start, on_step, on_end
    std::map<KyubuCreature, int> states;          // registry refs of ctx.state
};

// ctx:cancel(): the design's `ctx:end()` is not Lua ('end' is a keyword).
int ability_ctx_cancel(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "creature");
    auto c = (LuaCreature*)lua_touserdata(L, -1);
    if (c) game::ability_mark_cancel(c->h);
    return 0;
}

// ctx:marker(): the names of the clip markers crossed since the previous step, an array in the order crossed (empty when none, and in
// on_start). The array is the closure's upvalue, built from the native context's list.
int ability_ctx_marker(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

void lua_ability_call(void* user, int which, const KyubuAbilityCtx* ctx) {
    auto ab = (LuaAbility*)user;
    LuaCb* cb = ab->cb[which];
    const bool end = which == 2;
    LuaCb* any = cb ? cb : ab->cb[0] ? ab->cb[0] : ab->cb[1] ? ab->cb[1] : ab->cb[2];   // a table with only on_end has cb[2] alone
    std::lock_guard<std::recursive_mutex> g(any->vm->mu);
    lua_State* L = any->vm->L;
    const int top = lua_gettop(L);
    auto it = ab->states.find(ctx->creature);
    if (cb && !cb->disabled) {
        lua_newtable(L);
        const int t = lua_gettop(L);
        KyubuModHandle mod = cb->lm && cb->lm->mod ? cb->lm->mod->handle(cb->vm->side) : nullptr;
        push_creature(L, mod, ctx->creature);
        lua_setfield(L, t, "creature");
        set_str(L, "id", ctx->id);
        set_num(L, "number", ctx->number);
        set_num(L, "elapsed_ms", ctx->elapsed_ms);
        set_num(L, "dt_ms", ctx->dt_ms);
        set_num(L, "steps", ctx->steps);
        set_num(L, "rank", ctx->rank);
        set_num(L, "restarts", ctx->size >= offsetof(KyubuAbilityCtx, restarts) + sizeof(ctx->restarts) ? ctx->restarts : 0);
        const bool watch = ctx->size >= offsetof(KyubuAbilityCtx, watch) + sizeof(ctx->watch) && ctx->watch;
        set_str(L, "side", watch ? "watch" : ctx->side == KY_SIDE_SERVER ? "server" : "client");
        if (end) set_str(L, "reason", game::end_reason_name((game::EndReason)ctx->reason));
        if (it == ab->states.end()) {
            lua_newtable(L);
            it = ab->states.emplace(ctx->creature, luaL_ref(L, LUA_REGISTRYINDEX)).first;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
        lua_setfield(L, t, "state");
        lua_pushcfunction(L, ability_ctx_cancel);
        lua_setfield(L, t, "cancel");
        const uint32_t n = ctx->size >= sizeof(KyubuAbilityCtx) && ctx->markers ? ctx->marker_count : 0;
        lua_createtable(L, (int)n, 0);
        for (uint32_t i = 0; i < n; ++i) {
            lua_pushstring(L, ctx->markers[i]);
            lua_rawseti(L, -2, (int)i + 1);
        }
        lua_pushcclosure(L, ability_ctx_marker, 1);
        lua_setfield(L, t, "marker");
        if (!call_ref(cb, 1, 0)) game::ability_callback_failed();
    }
    if (end && it != ab->states.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, it->second);
        ab->states.erase(it);
    }
    lua_settop(L, top);
}

void KYUBU_CALL lua_ability_start(void* user, const KyubuAbilityCtx* ctx) { lua_ability_call(user, 0, ctx); }
void KYUBU_CALL lua_ability_step(void* user, const KyubuAbilityCtx* ctx) { lua_ability_call(user, 1, ctx); }
void KYUBU_CALL lua_ability_end(void* user, const KyubuAbilityCtx* ctx) { lua_ability_call(user, 2, ctx); }

// K.ability_define(handle, id, def): true, or nil and the reason.
int k_ability_define(lua_State* L) {
    KyubuModHandle mod = check_handle(L, 1);
    const char* id = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    KyubuAbilityDef d = {};
    d.size = sizeof d;
    d.name = ability_text(L, 3, "name");
    d.description = ability_text(L, 3, "description");
    d.icon = ability_text(L, 3, "icon");
    d.icon_file = ability_text(L, 3, "icon_file");
    d.duration_ms = to_u32(ability_whole(L, 3, "duration_ms", 500));
    d.mp_cost = (float)ability_number(L, 3, "mp_cost", 0);
    d.max_rank = to_u32(ability_whole(L, 3, "max_rank", 1));
    d.restarts = to_u32(ability_whole(L, 3, "restarts", 0));
    uint32_t cooldown[kAbilityRankMax + 1];
    lua_getfield(L, 3, "cooldown");
    if (lua_type(L, -1) == LUA_TTABLE) {
        const size_t n = lua_objlen(L, -1);
        if (n > kAbilityRankMax + 1 || (d.max_rank >= 1 && d.max_rank <= kAbilityRankMax && n != (size_t)d.max_rank + 1))
            luaL_error(L, "kyubu.ability.define: 'cooldown_ms' must have max_rank + 1 values (ranks 0 to max_rank)");
        for (size_t i = 0; i < n; ++i) {
            lua_rawgeti(L, -1, (int)i + 1);
            if (lua_type(L, -1) != LUA_TNUMBER || lua_tonumber(L, -1) != std::floor(lua_tonumber(L, -1)))
                luaL_error(L, "kyubu.ability.define: 'cooldown_ms' must give whole numbers");
            cooldown[i] = to_u32(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        if (n == (size_t)d.max_rank + 1) d.cooldown_ms = cooldown;
    } else if (!lua_isnil(L, -1)) {
        luaL_error(L, "kyubu.ability.define: 'cooldown_ms' must be a number or a function");
    }
    lua_pop(L, 1);
    lua_getfield(L, 3, "claim");
    if (lua_type(L, -1) == LUA_TTABLE) {
        const int c = lua_gettop(L);
        // a claim table is a claim: only slot 4 (the default) passes, so 0 and others reach the check as a bad "slot"
        const int32_t slot = to_i32(ability_whole(L, c, "slot", KY_ABILITY_SLOT_FOURTH));
        d.slot = slot == KY_ABILITY_SLOT_FOURTH ? slot : 99;
        lua_getfield(L, c, "class");
        if (lua_type(L, -1) == LUA_TSTRING) { const int v = ability_class_value(lua_tostring(L, -1)); d.class_type = v == -2 ? 99 : v; }
        else if (lua_type(L, -1) == LUA_TNUMBER) d.class_type = to_i32(ability_whole(L, c, "class", 0));
        lua_pop(L, 1);
        lua_getfield(L, c, "specialization");
        if (lua_isnil(L, -1)) d.specialization = KY_ABILITY_SPEC_ANY;
        else if (lua_type(L, -1) == LUA_TNUMBER) d.specialization = to_i32(ability_whole(L, c, "specialization", 0));
        else if (lua_type(L, -1) == LUA_TSTRING) { const int v = ability_specialization_value(d.class_type, lua_tostring(L, -1)); d.specialization = v == -2 ? 99 : v; }
        else luaL_error(L, "kyubu.ability.define: claim 'specialization' must be a name or a number");
        lua_pop(L, 1);
    } else if (!lua_isnil(L, -1)) {
        luaL_error(L, "kyubu.ability.define: 'claim' must be a table");
    }
    lua_pop(L, 1);
    // The callbacks: all three trampolines are set when the table has any function, so that on_end also drops the ctx.state
    // of an activation whose mod gave no on_end. `watch` (6.2) is a table of the same three names: the bystander's callbacks,
    // with a LuaAbility of their own, so that a watched run's ctx.state is not the creature's own run's.
    static const char* const kCallbackNames[3] = {"on_start", "on_step", "on_end"};
    auto read_callbacks = [&](int table, const std::string& prefix) -> LuaAbility* {
        LuaAbility* ab = nullptr;   // like LuaCb: never freed, native code keeps the pointer
        for (int i = 0; i < 3; ++i) {
            lua_getfield(L, table, kCallbackNames[i]);
            if (lua_type(L, -1) == LUA_TFUNCTION) {
                if (!ab) ab = new LuaAbility;
                ab->cb[i] = new_cb(L, -1, lua_mod(mod), "ability " + std::string(id) + " " + prefix + kCallbackNames[i]);
            } else if (!lua_isnil(L, -1)) {
                luaL_error(L, "kyubu.ability.define: '%s%s' must be a function", prefix.c_str(), kCallbackNames[i]);
            }
            lua_pop(L, 1);
        }
        return ab;
    };
    if (LuaAbility* ab = read_callbacks(3, "")) {
        d.on_start = lua_ability_start;
        d.on_step = lua_ability_step;
        d.on_end = lua_ability_end;
        d.user = ab;
    }
    lua_getfield(L, 3, "watch");
    if (lua_type(L, -1) == LUA_TTABLE) {
        if (LuaAbility* ab = read_callbacks(lua_gettop(L), "watch.")) {
            d.watch_start = lua_ability_start;
            d.watch_step = lua_ability_step;
            d.watch_end = lua_ability_end;
            d.watch_user = ab;
        }
    } else if (!lua_isnil(L, -1)) {
        luaL_error(L, "kyubu.ability.define: 'watch' must be a table");
    }
    lua_pop(L, 1);
    return push_ok(L, A()->ability_define(mod, id, &d));
}

// K.ability_number(id): the number, or nil and the reason.
int k_ability_number(lua_State* L) {
    uint32_t n = 0;
    KyubuResult r = A()->ability_number(luaL_checkstring(L, 1), &n);
    if (r != KY_OK) return push_ok(L, r);
    lua_pushinteger(L, n);
    return 1;
}
// K.ability_name(number): the id, or nil and the reason.
int k_ability_name(lua_State* L) {
    char buf[256];
    KyubuResult r = A()->ability_name((uint32_t)luaL_checkinteger(L, 1), buf, sizeof buf);
    if (r != KY_OK) return push_ok(L, r);
    lua_pushstring(L, buf);
    return 1;
}
// K.ability_list(): an array of { id =, number = } in number order, or nil and the reason (before the freeze).
int k_ability_list(lua_State* L) {
    char buf[256];
    uint32_t count = 0, n = 0;
    KyubuResult r = A()->ability_list(0, buf, sizeof buf, &n, &count);
    if (r != KY_OK) return push_ok(L, r);
    lua_createtable(L, (int)count, 0);
    for (uint32_t i = 0; i < count && A()->ability_list(i, buf, sizeof buf, &n, nullptr) == KY_OK; ++i) {
        lua_createtable(L, 0, 2);
        lua_pushstring(L, buf);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, n);
        lua_setfield(L, -2, "number");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}
// K.ability_slot_owner(class, specialization): the id and the number of the ability that holds slot 4, or nil and the reason
// ("not found": nobody). A class is a number or a name ("warrior"), a specialization 0 or 1 or a name ("guardian"); an
// unknown name is a value the native check refuses.
int k_ability_slot_owner(lua_State* L) {
    int32_t cls, spec;
    if (lua_type(L, 1) == LUA_TSTRING) { const int v = ability_class_value(lua_tostring(L, 1)); cls = v == -2 ? 99 : v; }
    else cls = to_i32(luaL_checknumber(L, 1));
    if (lua_type(L, 2) == LUA_TSTRING) { const int v = ability_specialization_value(cls, lua_tostring(L, 2)); spec = v == -2 ? 99 : v; }
    else spec = to_i32(luaL_checknumber(L, 2));
    char buf[256];
    uint32_t n = 0;
    KyubuResult r = A()->ability_slot_owner(cls, spec, buf, sizeof buf, &n);
    if (r != KY_OK) return push_ok(L, r);
    lua_pushstring(L, buf);
    lua_pushinteger(L, n);
    return 2;
}

int k_registry_hash(lua_State* L) {
    char buf[80];
    KyubuResult r = A()->registry_hash(luaL_checkstring(L, 1), buf, sizeof buf);
    if (r != KY_OK) return push_ok(L, r);
    lua_pushstring(L, buf);
    return 1;
}
int k_json_text(lua_State* L) {
    nlohmann::json j = {{"en", luaL_checkstring(L, 1)}, {"de", luaL_checkstring(L, 2)}};
    lua_pushstring(L, j.dump().c_str());
    return 1;
}
int k_text_get(lua_State* L) {
    std::string id = luaL_checkstring(L, 1);
    std::string lang = luaL_optstring(L, 2, "");
    if (lang.empty()) {
        // options.cfg: "language 0" = English, 1 = German
        lang = "en";
        if (auto d = read_file(join_path(L_().game_dir, "options.cfg"))) {
            std::string s(d->begin(), d->end());
            auto p = s.find("language");
            if (p != std::string::npos && s.find('1', p) == p + 9) lang = "de";
        }
    }
    std::vector<uint8_t> p;
    if (Registries::get().payload("kyubu:text", id, &p) != KY_OK) return 0;
    auto j = nlohmann::json::parse(p.begin(), p.end(), nullptr, false);
    if (j.is_discarded() || !j.contains(lang)) return 0;
    lua_pushstring(L, j[lang].get<std::string>().c_str());
    return 1;
}

KyubuResult KYUBU_CALL provider_trampoline(const char* db, const char* key, void** out, uint32_t* out_len, void* user) {
    auto cb = (LuaCb*)user;
    if (cb->disabled) return KY_E_NOT_FOUND;
    std::lock_guard<std::recursive_mutex> g(cb->vm->mu);
    lua_State* L = cb->vm->L;
    int top = lua_gettop(L);
    lua_pushstring(L, db);
    lua_pushstring(L, key);
    KyubuResult r = KY_E_NOT_FOUND;
    if (call_ref(cb, 2, 1) && lua_type(L, -1) == LUA_TSTRING) {
        size_t n;
        const char* s = lua_tolstring(L, -1, &n);
        *out = api_table()->buffer_alloc((uint32_t)n);
        memcpy(*out, s, n);
        *out_len = (uint32_t)n;
        r = KY_OK;
    }
    lua_settop(L, top);
    return r;
}

int k_asset_provider(lua_State* L) {
    auto h = check_handle(L, 1);
    LuaMod* lm = lua_mod(h);
    auto cb = new_cb(L, 4, lm, "asset provider");
    KyubuId id = 0;
    KyubuResult r = A()->asset_provider(h, luaL_checkstring(L, 2), luaL_checkstring(L, 3), provider_trampoline, cb, &id);
    if (r != KY_OK) return push_ok(L, r);
    if (lm) lm->providers.push_back(id);
    lua_pushinteger(L, id);
    return 1;
}

int k_asset_read(lua_State* L) {
    auto r = Assets::get().resolve(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    if (!r) return 0;
    lua_pushlstring(L, (const char*)r->data.data(), r->data.size());
    lua_pushstring(L, r->layer.c_str());
    return 2;
}

int k_asset_served_by(lua_State* L) {
    lua_pushstring(L, Assets::get().served_by(luaL_checkstring(L, 1), luaL_checkstring(L, 2)).c_str());
    return 1;
}

void KYUBU_CALL channel_trampoline(uint32_t conn, const uint8_t* data, uint32_t len, void* user) {
    auto cb = (LuaCb*)user;
    if (cb->disabled) return;
    std::lock_guard<std::recursive_mutex> g(cb->vm->mu);
    lua_State* L = cb->vm->L;
    int top = lua_gettop(L);
    lua_pushinteger(L, conn);
    lua_pushlstring(L, (const char*)data, len);
    call_ref(cb, 2, 0);
    lua_settop(L, top);
}

int k_channel_register(lua_State* L) {
    auto h = check_handle(L, 1);
    std::string name = luaL_checkstring(L, 2);
    LuaMod* lm = lua_mod(h);
    auto cb = new_cb(L, 3, lm, "channel " + name);
    KyubuSide side = side_arg(L, 4, h);
    KyubuResult r = A()->channel_register(h, name.c_str(), side, channel_trampoline, cb);
    if (r == KY_OK && lm) lm->channels.push_back(name);
    return push_ok(L, r);
}

int k_channel_send(lua_State* L) {
    size_t n;
    const char* s = luaL_checklstring(L, 4, &n);
    return push_ok(L, A()->channel_send(check_handle(L, 1), luaL_checkstring(L, 2), (uint32_t)luaL_checknumber(L, 3), s, (uint32_t)n));
}

int k_publish_interface(lua_State* L) {
    auto h = check_handle(L, 1);
    std::string name = luaL_checkstring(L, 2);
    void* p = lua_islightuserdata(L, 4) ? lua_touserdata(L, 4) : (void*)(uintptr_t)luaL_checknumber(L, 4);
    KyubuResult r = A()->publish_interface(h, name.c_str(), (uint32_t)luaL_checkinteger(L, 3), p);
    if (r == KY_OK) if (auto lm = lua_mod(h)) lm->ifaces.push_back(name);
    return push_ok(L, r);
}

int k_get_interface(lua_State* L) {
    void* p = nullptr;
    uint32_t v = 0;
    KyubuResult r = A()->get_interface(luaL_checkstring(L, 1), (uint32_t)luaL_optinteger(L, 2, 0), &p, &v);
    if (r != KY_OK) return push_ok(L, r);
    lua_pushlightuserdata(L, p);
    lua_pushinteger(L, v);
    return 2;
}

struct CmdCtx { KyubuPrintFn print; void* ctx; };
int cmd_print(lua_State* L) {
    auto c = (CmdCtx*)lua_touserdata(L, lua_upvalueindex(1));
    std::string line;
    for (int i = 1; i <= lua_gettop(L); ++i) {
        if (i > 1) line += " ";
        line += to_display(L, i);
        lua_pop(L, 1);
    }
    c->print(c->ctx, line.c_str());
    return 0;
}

void KYUBU_CALL command_trampoline(const char* args, KyubuPrintFn print, void* pctx, void* user) {
    auto cb = (LuaCb*)user;
    std::lock_guard<std::recursive_mutex> g(cb->vm->mu);
    lua_State* L = cb->vm->L;
    int top = lua_gettop(L);
    CmdCtx c{print, pctx};
    lua_pushstring(L, args);
    lua_pushlightuserdata(L, &c);
    lua_pushcclosure(L, cmd_print, 1);
    call_ref(cb, 2, 0);
    lua_settop(L, top);
}

int k_register_command(lua_State* L) {
    auto h = check_handle(L, 1);
    std::string name = luaL_checkstring(L, 2);
    LuaMod* lm = lua_mod(h);
    auto cb = new_cb(L, 4, lm, "command " + name);
    KyubuResult r = A()->register_command(h, name.c_str(), luaL_optstring(L, 3, ""), command_trampoline, cb);
    if (r == KY_OK && lm) lm->commands.push_back(name);
    return push_ok(L, r);
}

// UI (valid inside render.overlay handlers)
int ui_begin(lua_State* L) {
    bool has_open = !lua_isnoneornil(L, 2);
    int open = has_open ? lua_toboolean(L, 2) : 1;
    int vis = A()->ui_begin(luaL_checkstring(L, 1), has_open ? &open : nullptr);
    lua_pushboolean(L, vis);
    lua_pushboolean(L, open);
    return 2;
}
int ui_end(lua_State*) { A()->ui_end(); return 0; }
int ui_text(lua_State* L) { A()->ui_text(luaL_checkstring(L, 1)); return 0; }
int ui_button(lua_State* L) { lua_pushboolean(L, A()->ui_button(luaL_checkstring(L, 1))); return 1; }
int ui_checkbox(lua_State* L) {
    int v = lua_toboolean(L, 2);
    int changed = A()->ui_checkbox(luaL_checkstring(L, 1), &v);
    lua_pushboolean(L, changed);
    lua_pushboolean(L, v);
    return 2;
}
int ui_slider_float(lua_State* L) {
    float v = (float)luaL_checknumber(L, 2);
    int changed = A()->ui_slider_float(luaL_checkstring(L, 1), &v, (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
    lua_pushboolean(L, changed);
    lua_pushnumber(L, v);
    return 2;
}
int ui_input_text(lua_State* L) {
    size_t cap = (size_t)luaL_optinteger(L, 3, 256);
    std::vector<char> buf(cap + 1, 0);
    strncpy(buf.data(), luaL_optstring(L, 2, ""), cap);
    int changed = A()->ui_input_text(luaL_checkstring(L, 1), buf.data(), (uint32_t)cap);
    lua_pushboolean(L, changed);
    lua_pushstring(L, buf.data());
    return 2;
}
int ui_same_line(lua_State*) { A()->ui_same_line(); return 0; }
int ui_separator(lua_State*) { A()->ui_separator(); return 0; }
int ui_want_input(lua_State* L) { lua_pushboolean(L, A()->ui_want_input()); return 1; }

void push_k_table(lua_State* L) {
    static const luaL_Reg fns[] = {
        {"log", k_log}, {"phase", k_phase}, {"host", k_host}, {"current_side", k_current_side},
        {"game_dir", k_game_dir}, {"data_dir", k_data_dir}, {"read_package_file", k_read_package_file},
        {"is_mod_loaded", k_is_mod_loaded}, {"manifest", k_manifest}, {"mods", k_mods}, {"stats", k_stats},
        {"game_alloc", k_game_alloc}, {"game_free", k_game_free}, {"mod_state", k_mod_state}, {"api_functions", k_api_functions},
        {"run_on_main_thread", k_run_on_main_thread}, {"subscribe", k_subscribe}, {"unsubscribe", k_unsubscribe},
        {"emit", k_emit}, {"resolve_symbol", k_resolve_symbol}, {"rva_to_addr", k_rva_to_addr},
        {"addr_to_rva", k_addr_to_rva}, {"mem_read", k_mem_read}, {"mem_write", k_mem_write},
        {"patch_bytes", k_patch_bytes}, {"unhook", k_unhook}, {"hook_raw", k_hook_raw}, {"hook_mid", k_hook_mid},
        {"hook_error", k_hook_error}, {"registry_create", k_registry_create}, {"registry_add", k_registry_add},
        {"registry_remap", k_registry_remap}, {"registry_lookup", k_registry_lookup}, {"registry_name", k_registry_name},
        {"registry_payload", k_registry_payload}, {"registry_count", k_registry_count}, {"registry_entry", k_registry_entry},
        {"registry_hash", k_registry_hash}, {"ability_define", k_ability_define}, {"ability_number", k_ability_number},
        {"ability_name", k_ability_name}, {"ability_list", k_ability_list}, {"ability_slot_owner", k_ability_slot_owner},
        {"json_text", k_json_text}, {"text_get", k_text_get},
        {"asset_provider", k_asset_provider}, {"asset_read", k_asset_read}, {"asset_served_by", k_asset_served_by},
        {"channel_register", k_channel_register}, {"channel_send", k_channel_send},
        {"publish_interface", k_publish_interface}, {"get_interface", k_get_interface},
        {"register_command", k_register_command}, {"game_available", k_game_available},
        {"game_enum", k_game_enum}, {"creature_get", k_creature_get}, {"creature_player", k_creature_player},
        {"creature_spawn", k_creature_spawn}, {"creature_find", k_creature_find}, {"creature_teleport", k_creature_teleport}, {"creature_set_yaw", k_creature_set_yaw}, {"creature_set_flags", k_creature_set_flags},
        {"body_define", k_body_define}, {"body_name", k_body_name}, {"creature_body_attach", k_creature_body_attach}, {"creature_body_play", k_creature_body_play},
        {"creature_body_offset", k_creature_body_offset}, {"creature_body_weight", k_creature_body_weight}, {"creature_body_read", k_creature_body_read}, {"creature_body_point", k_creature_body_point}, {"creature_give_item", k_creature_give_item},
        {"ability_get", k_ability_get}, {"ability_cooldown", k_ability_cooldown}, {"ability_cooldown_set", k_ability_cooldown_set}, {"ability_start", k_ability_start}, {"ability_end", k_ability_end},
        {"ability_rank_get", k_ability_rank_get}, {"ability_rank_set", k_ability_rank_set},
        {"animation_define_from_text", k_animation_define_from_text}, {"animation_preview", k_animation_preview},
        {"animation_preview_clear", k_animation_preview_clear}, {"ability_animation_bind", k_ability_animation_bind}, {"ability_animation_bind_for", k_ability_animation_bind_for},
        {"ui_node_find", k_ui_node_find}, {"ui_node_get", k_ui_node_get}, {"ui_node_child", k_ui_node_child}, {"render_get", k_render_get}, {"particle_spawn", k_particle_spawn}, {"light_spawn", k_light_spawn}, {"light_set", k_light_set}, {"light_remove", k_light_remove}, {"world_block", k_world_block}, {"creature_hit", k_creature_hit}, {"creature_state_get", k_creature_state}, {"creature_state_set", k_creature_set_state}, {"shot_take", k_shot_take}, {"projectile_push", k_projectile_push},
        {"config_get", k_config_get}, {"config_set", k_config_set}, {nullptr, nullptr}};
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    static const luaL_Reg ui[] = {
        {"begin", ui_begin}, {"finish", ui_end}, {"text", ui_text}, {"button", ui_button}, {"checkbox", ui_checkbox},
        {"slider_float", ui_slider_float}, {"input_text", ui_input_text}, {"same_line", ui_same_line},
        {"separator", ui_separator}, {"want_input", ui_want_input}, {nullptr, nullptr}};
    lua_newtable(L);
    luaL_register(L, nullptr, ui);
    lua_pushcfunction(L, ui_end);
    lua_setfield(L, -2, "end");
    lua_setfield(L, -2, "ui");
}

// ------------------------------------------------------------------ VM & environments
LuaVM* vm(KyubuSide side) {
    if (g_vm[side]) return g_vm[side];
    auto v = new LuaVM();
    v->side = side;
    v->L = luaL_newstate();
    luaL_openlibs(v->L);
    lua_State* L = v->L;
    open_creatures(L);
    add_module_dir(L, join_path(L_().kyubu_dir, "lua"));   // require("cubeworld") in full-trust mods
    // load the prelude: returns make_api(handle, id, side, trust)
    if (luaL_loadbuffer(L, kPreludeLua, strlen(kPreludeLua), "@kyubu/prelude.lua") != 0) {
        KERROR("Lua prelude failed to compile: %s", lua_tostring(L, -1));
        lua_close(L);
        delete v;
        return nullptr;
    }
    push_k_table(L);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        KERROR("Lua prelude failed: %s", lua_tostring(L, -1));
        lua_close(L);
        delete v;
        return nullptr;
    }
    v->make_api_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_vm[side] = v;
    KINFO("Lua VM created for the %s side (%s)", side_name(side), LUAJIT_VERSION);
    return v;
}

const char* kRestrictedRequire =
    "local name = ...\n"
    "if name == 'ffi' then error(\"FFI is unavailable at trust level 'restricted'\", 2) end\n"
    "error(\"require('\" .. tostring(name) .. \"') is unavailable at trust level 'restricted'\", 2)\n";

// Builds a fresh environment table for (mod, side) and leaves it on the stack.
bool make_env(LuaVM* v, LuaMod* lm) {
    lua_State* L = v->L;
    const std::string& trust = lm->mod->manifest.trust;
    lua_newtable(L);                    // env
    int env = lua_gettop(L);
    if (trust == "full") {
        lua_newtable(L);                // metatable {__index = _G}
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, env);
    } else {
        static const char* safe[] = {"assert", "error", "ipairs", "next", "pairs", "pcall", "select", "tonumber",
                                     "tostring", "type", "unpack", "xpcall", "rawget", "rawset", "rawequal",
                                     "setmetatable", "getmetatable", "string", "table", "math", "bit", "coroutine", nullptr};
        for (int i = 0; safe[i]; ++i) {
            lua_getglobal(L, safe[i]);
            lua_setfield(L, env, safe[i]);
        }
        luaL_loadstring(L, kRestrictedRequire);
        lua_setfield(L, env, "require");
    }
    lua_pushvalue(L, env);
    lua_setfield(L, env, "_G");
    // kyubu = make_api(handle, id, side, trust)
    lua_rawgeti(L, LUA_REGISTRYINDEX, v->make_api_ref);
    lua_pushlightuserdata(L, lm->mod->handle(lm->side));
    lua_pushstring(L, lm->mod->id.c_str());
    lua_pushstring(L, side_name(lm->side));
    lua_pushstring(L, trust.c_str());
    if (lua_pcall(L, 4, 1, 0) != 0) {
        Log::get().write(KY_LOG_ERROR, lm->mod->id, lm->side, std::string("cannot create Lua API: ") + lua_tostring(L, -1));
        lua_settop(L, env - 1);
        return false;
    }
    lua_pushvalue(L, -1);
    lm->api_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_setfield(L, env, "kyubu");
    // print -> kyubu.info
    luaL_loadstring(L, "local k = ... return function(...) k.info(...) end");
    lua_getfield(L, env, "kyubu");
    lua_call(L, 1, 1);
    lua_setfield(L, env, "print");
    return true;
}

bool run_script(LuaVM* v, LuaMod* lm, const std::string& rel) {
    lua_State* L = v->L;
    auto code = lm->mod->pkg->read(rel);
    if (!code) {
        Log::get().write(KY_LOG_ERROR, lm->mod->id, lm->side, "script not found: " + rel);
        return false;
    }
    g_script_mtimes[lm->mod->id + "|" + rel] = lm->mod->pkg->mtime(rel);
    std::string chunk = "@" + lm->mod->id + "/" + rel;
    if (luaL_loadbuffer(L, (const char*)code->data(), code->size(), chunk.c_str()) != 0) {
        Log::get().write(KY_LOG_ERROR, lm->mod->id, lm->side, std::string("script error: ") + lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, lm->env_ref);
    lua_setfenv(L, -2);
    LuaCb tmp{v, lm, LUA_NOREF, rel};
    // call the chunk through call_ref semantics (with traceback)
    tmp.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    bool ok = false;
    ModManager::get().invoke(lm->mod, lm->side, rel.c_str(), [&] { ok = call_ref(&tmp, 0, 0); });
    luaL_unref(L, LUA_REGISTRYINDEX, tmp.ref);
    return ok;
}

std::vector<std::string> scripts_for(Mod* m, KyubuSide side) {
    std::vector<std::string> v;
    auto& s = m->manifest.scripts;
    if (auto it = s.find("common"); it != s.end() && m->has_side(side)) v.push_back(it->second);
    if (auto it = s.find(side == KY_SIDE_CLIENT ? "client" : "server"); it != s.end()) v.push_back(it->second);
    return v;
}

bool load_mod_side(Mod* m, KyubuSide side) {
    auto scripts = scripts_for(m, side);
    if (scripts.empty()) return true;
    LuaVM* v = vm(side);
    if (!v) return false;
    std::lock_guard<std::recursive_mutex> g(v->mu);
    auto key = std::make_pair(m, (int)side);
    LuaMod* lm = g_mods[key];
    if (!lm) { lm = new LuaMod(); g_mods[key] = lm; }
    lm->mod = m;
    lm->side = side;
    lm->vm = v;
    if (!make_env(v, lm)) return false;
    lm->env_ref = luaL_ref(v->L, LUA_REGISTRYINDEX);
    bool ok = true;
    for (auto& rel : scripts) ok = run_script(v, lm, rel) && ok;
    if (ok) Log::get().write(KY_LOG_INFO, m->id, side, fmt("Lua scripts loaded (%u file(s), trust %s)", (unsigned)scripts.size(), m->manifest.trust.c_str()));
    return ok;
}

void unload_mod_side(LuaMod* lm) {
    LuaVM* v = lm->vm;
    std::lock_guard<std::recursive_mutex> g(v->mu);
    lua_State* L = v->L;
    if (lm->api_ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, lm->api_ref);
        lua_getfield(L, -1, "_unload");
        if (lua_pcall(L, 0, 0, 0) != 0) lua_pop(L, 1);
        lua_pop(L, 1);
    }
    for (auto id : lm->subs) Events::get().unsubscribe(lm->mod, id);
    for (auto id : lm->hooks) Hooks::get().remove(lm->mod->id, id);
    for (auto id : lm->providers) Assets::get().remove_provider(id);
    for (auto& c : lm->commands) Console::get().remove(c);
    for (auto& c : lm->channels) Channels::get().unreg(lm->side, c);
    for (auto& i : lm->ifaces) Interfaces::get().remove(i);
    lm->subs.clear(); lm->hooks.clear(); lm->providers.clear();
    lm->commands.clear(); lm->channels.clear(); lm->ifaces.clear();
    if (lm->env_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, lm->env_ref);
    if (lm->api_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, lm->api_ref);
    lm->env_ref = lm->api_ref = LUA_NOREF;
    lua_gc(L, LUA_GCCOLLECT, 0);
}

}  // namespace

LuaRuntime& LuaRuntime::get() {
    static LuaRuntime* r = new LuaRuntime();
    return *r;
}

void LuaRuntime::load_all() {
    uint32_t sides = ModManager::get().host_sides();
    for (Mod* m : ModManager::get().ordered()) {
        if (!m->active() || m->manifest.scripts.empty()) continue;
        for (KyubuSide s : {KY_SIDE_CLIENT, KY_SIDE_SERVER}) {
            if (!(sides & s)) continue;
            if (!load_mod_side(m, s)) {
                if (m->state == ModState::Loaded) {
                    m->state = ModState::Failed;
                    m->reason = "Lua script failed to load (see log)";
                }
            }
        }
    }
}

std::string LuaRuntime::reload(Mod* m) {
    if (!m) return "no such mod";
    if (!L_().dev_mode) return "reload requires dev mode";
    int n = 0;
    game::lights_drop_mod(m->id);   // the new scripts start their lights again
    for (KyubuSide s : {KY_SIDE_CLIENT, KY_SIDE_SERVER}) {
        auto it = g_mods.find({m, (int)s});
        if (it == g_mods.end()) continue;
        unload_mod_side(it->second);
        load_mod_side(m, s);
        ++n;
    }
    Assets::get().build_index();
    return n ? fmt("reloaded %s (%d side(s)); registry entries are kept", m->id.c_str(), n)
             : m->id + " has no Lua scripts; assets re-indexed";
}

std::string LuaRuntime::eval(KyubuSide side, const std::string& code) {
    LuaVM* v = vm(side);
    if (!v) return "no Lua VM";
    std::lock_guard<std::recursive_mutex> g(v->mu);
    lua_State* L = v->L;
    std::string out;
    std::string src = "return " + code;
    if (luaL_loadstring(L, src.c_str()) != 0) {
        lua_pop(L, 1);
        if (luaL_loadstring(L, code.c_str()) != 0) {
            out = lua_tostring(L, -1);
            lua_pop(L, 1);
            return out;
        }
    }
    int top = lua_gettop(L) - 1;
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        out = std::string("error: ") + lua_tostring(L, -1);
        lua_settop(L, top);
        return out;
    }
    for (int i = top + 1; i <= lua_gettop(L); ++i) {
        if (!out.empty()) out += "\t";
        out += to_display(L, i);
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    return out.empty() ? "ok" : out;
}

void LuaRuntime::poll_changes() {
    static uint64_t last = 0;
    uint64_t now = GetTickCount64();
    if (now - last < 500) return;
    last = now;
    std::vector<Mod*> changed;
    for (auto& [key, mt] : g_script_mtimes) {
        auto bar = key.find('|');
        Mod* m = ModManager::get().find(key.substr(0, bar));
        if (!m || !m->pkg || m->pkg->is_zip()) continue;
        if (m->pkg->mtime(key.substr(bar + 1)) != mt &&
            std::find(changed.begin(), changed.end(), m) == changed.end()) changed.push_back(m);
    }
    for (Mod* m : changed) KINFO("hot reload: %s", reload(m).c_str());
}

void LuaRuntime::shutdown() {
    for (auto& [k, lm] : g_mods) {
        std::lock_guard<std::recursive_mutex> g(lm->vm->mu);
        if (lm->api_ref == LUA_NOREF) continue;
        lua_rawgeti(lm->vm->L, LUA_REGISTRYINDEX, lm->api_ref);
        lua_getfield(lm->vm->L, -1, "_unload");
        if (lua_pcall(lm->vm->L, 0, 0, 0) != 0) lua_pop(lm->vm->L, 1);
        lua_pop(lm->vm->L, 1);
    }
}

size_t LuaRuntime::memory_kb(KyubuSide side) {
    if (!g_vm[side]) return 0;
    std::lock_guard<std::recursive_mutex> g(g_vm[side]->mu);
    return (size_t)lua_gc(g_vm[side]->L, LUA_GCCOUNT, 0);
}

bool LuaRuntime::has_vm(KyubuSide side) { return g_vm[side] != nullptr; }

}  // namespace kyubu
