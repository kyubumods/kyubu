// Boot orchestration after the host build is verified: symbols, hooks, mods, lifecycle phases.
#include "runtime.h"

#include "abilities.h"
#include "animations.h"
#include "api.h"
#include "channels.h"
#include "console.h"
#include "crash.h"
#include "events.h"
#include "game.h"
#include "game_hooks.h"
#include "hooks.h"
#include "join_gate.h"
#include "loader.h"
#include "log.h"
#include "luart.h"
#include "modconfig.h"
#include "models.h"
#include "modmanager.h"
#include "overlay.h"
#include "registry.h"
#include "symbols.h"
#include "ui.h"

namespace kyubu {

void api_run_main_thread_tasks();   // api.cpp

namespace {
uint32_t g_frame = 0;
bool g_game_ready = false;
LARGE_INTEGER g_last_frame{};

void set_phase(KyubuPhase p, const char* event) {
    L().phase = p;
    KINFO("phase: %s", event);
    Events::get().emit_active(std::string("lifecycle.") + event);
}
}  // namespace

void runtime_boot() {
    Loader& l = L();
    auto& syms = SymbolTable::get();
    syms.set_image(l.host.base);
    int n = syms.load_dir(join_path(l.kyubu_dir, "symbols"), l.host.build_id, "kyubu");
    KINFO("symbol map: %d symbol(s) for %s", n, l.host.build_id.c_str());
    if (n == 0) KWARN("no symbol map found in kyubu/symbols for this build: game events and assets will be unavailable");
    syms.verify_all();

    auto& hooks = Hooks::get();
    if (!hooks.init()) { KERROR("hook engine failed to initialise; running without mods"); return; }
    hooks.owner_order = [](const std::string& o) { return ModManager::get().order_of(o); };
    hooks.invoke_wrapper = [](const std::string& owner, KyubuSide side, const std::function<void()>& fn) {
        ModManager::get().invoke(owner == "kyubu" ? nullptr : ModManager::get().find(owner), side, "mid hook", fn);
    };
    crash_set_hooks({[](uintptr_t a) { return ModManager::get().mod_for_address(a); },
                     []() -> std::string {
                         Mod* m = current_mod_tls();
                         return m ? m->id : std::string();
                     }});

    Registries::get().init_builtin();
    register_builtin_commands();

    ModManager::get().discover_and_resolve();

    // Loader hooks that produce events and serve assets.
    install_common_hooks();
    Assets::get().install_hooks();
    if (l.host.host == KY_HOST_CLIENT) {
        Models::get().install_hooks();
        install_client_hooks();
    } else {
        l.mode = KY_MODE_DEDICATED;
        install_server_hooks();
    }
    game::boot();   // the game API's entries, checked against this build

    // PreInit: native entry points, then Lua scripts.
    l.phase = KY_PHASE_PREINIT;
    ModManager::get().load_native();
    {   // the join gate: the gate mods that loaded on this host decide the protocol value (D4)
        std::vector<game::GateMod> gate;
        for (Mod* m : ModManager::get().ordered())
            if (m->active() && m->manifest.gate) gate.push_back({m->id, m->manifest.version.str()});
        game::join_gate_set(gate);
    }
    LuaRuntime::get().load_all();
    Assets::get().build_index();
    Animations::get().scan();
    set_phase(KY_PHASE_PREINIT, "preinit");

    set_phase(KY_PHASE_REGISTER, "register");

    Registries::get().freeze([](const std::string& owner) { return ModManager::get().order_of(owner); });
    ability_slots_resolve(l.config.ability_placeholder);
    if (l.host.host == KY_HOST_CLIENT) ability_set_notice([](const std::string& text, double s) { Ui::get().notice(text, s); });
    set_phase(KY_PHASE_POSTREGISTER, "post_register");

    int loaded = 0;
    for (Mod* m : ModManager::get().ordered()) if (m->active()) ++loaded;
    KINFO("boot complete: %d mod(s) active", loaded);
}

void runtime_on_game_ready() {
    if (g_game_ready) return;
    g_game_ready = true;
    Assets::get().log_summary();
    std::vector<std::string> bad;
    int ok = 0;
    for (auto& m : ModManager::get().all()) {
        if (m->state == ModState::Loaded) ++ok;
        else if ((m->state == ModState::Failed || m->state == ModState::Faulted) &&
                 std::find(bad.begin(), bad.end(), m->id) == bad.end()) bad.push_back(m->id);
    }
    if (L().host.host == KY_HOST_CLIENT) {
        if (!bad.empty()) {
            std::string list;
            for (auto& b : bad) list += (list.empty() ? "" : ", ") + b;
            Ui::get().notice("Kyubu: some mods did not load: " + list + " - see " + Log::get().path(), 20);
        } else if (!L().safe_mode) {
            Ui::get().notice(fmt("Kyubu: %d mod(s) loaded", ok), 6);
        }
    }
    set_phase(KY_PHASE_GAMEREADY, "game_ready");
}

void runtime_on_frame_begin() {
    Loader& l = L();
    if (!g_game_ready) runtime_on_game_ready();
    LARGE_INTEGER now, fq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&fq);
    double dt = g_last_frame.QuadPart ? double(now.QuadPart - g_last_frame.QuadPart) * 1000.0 / double(fq.QuadPart) : 0.0;
    g_last_frame = now;
    ModManager::get().frame_stats_roll();
    api_run_main_thread_tasks();
    config_pump();
    Channels::get().pump(KY_SIDE_CLIENT);
    if (l.mode == KY_MODE_MENU_OR_SINGLEPLAYER) Channels::get().pump(KY_SIDE_SERVER);
    if (l.dev_mode) {
        for (auto& [db, key] : Assets::get().poll_changes()) {
            if (db == "data1.db") Models::get().reload_key(key);
            KINFO("hot reload: asset %s/%s changed", db.c_str(), key.c_str());
        }
        LuaRuntime::get().poll_changes();
        Animations::get().poll_changes();
        console_inbox_poll();
    }
    KyubuFrameData f{++g_frame, dt};
    Events::get().emit("frame.begin", KY_SIDE_CLIENT, &f, sizeof f);
}

void runtime_on_frame_end() {
    KyubuFrameData f{g_frame, 0};
    Events::get().emit("frame.end", KY_SIDE_CLIENT, &f, sizeof f);
}

void runtime_on_server_tick(double dt_ms) {
    static uint32_t tick = 0;
    if (L().host.host == KY_HOST_SERVER) config_pump();   // Cube.exe pumps in frame.begin
    if (L().host.host == KY_HOST_SERVER) Channels::get().pump(KY_SIDE_SERVER);   // joined clients' messages (D10)
    if (L().host.host == KY_HOST_SERVER && L().dev_mode) console_inbox_poll();
    KyubuFrameData f{++tick, dt_ms};
    Events::get().emit("sim.tick", KY_SIDE_SERVER, &f, sizeof f);
}

void runtime_shutdown() {
    Loader& l = L();
    if (l.passthrough || l.phase == KY_PHASE_SHUTDOWN) return;
    set_phase(KY_PHASE_SHUTDOWN, "shutdown");
    LuaRuntime::get().shutdown();
    for (Mod* m : ModManager::get().ordered()) {
        for (size_t i = 0; i < m->shutdown_fns.size(); ++i) {
            auto fn = m->shutdown_fns[i];
            KyubuSide s = m->shutdown_sides[i];
            ModManager::get().invoke(m, s, "kyubu_mod_shutdown", [&] { fn(s); });
        }
    }
    KINFO("Kyubu shut down cleanly");
}

std::string runtime_reload_mod(const std::string& id) {
    Mod* m = ModManager::get().find(id);
    if (!m) return "no mod '" + id + "'";
    return LuaRuntime::get().reload(m);
}

std::string runtime_lua_eval(KyubuSide side, const std::string& code) { return LuaRuntime::get().eval(side, code); }

}  // namespace kyubu
