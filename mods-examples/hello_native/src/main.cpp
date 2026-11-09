// hello_native: a small native mod showing the main parts of the Kyubu C++ SDK.
#include <cstdio>

#include "kyubu.hpp"

namespace {

kyubu::Mod* g_client = nullptr;
int g_frames = 0;
int g_asset_reads = 0;

// Database::get is a thiscall: bool(Database* this, const std::string& key, void** out, uint32_t* len).
// Declared as __fastcall with an unused EDX parameter, which has the same ABI and also compiles with MSVC
// (MSVC does not allow __thiscall on free functions). See docs/hooks.md.
using DbGetFn = bool(__fastcall*)(void*, void*, const void*, void**, uint32_t*);
DbGetFn g_db_get_next = nullptr;
bool __fastcall db_get_hook(void* self, void* edx, const void* key, void** out, uint32_t* len) {
    ++g_asset_reads;
    return g_db_get_next(self, edx, key, out, len);
}

struct HelloIface {
    int (*frames)();
};
HelloIface g_iface{[] { return g_frames; }};

void KYUBU_CALL cmd_hello(const char* args, KyubuPrintFn print, void* ctx, void*) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "hello from native code! args='%s' frames=%d asset reads=%d", args, g_frames, g_asset_reads);
    print(ctx, buf);
}

}  // namespace

struct HelloNative {
    explicit HelloNative(kyubu::Mod& m) : mod(m) {
        mod.info("init on the %s side", m.side() == KY_SIDE_CLIENT ? "client" : "server");
        // Registries are shared by both sides of a process; registering the same entry twice is harmless,
        // and registering on every side keeps client and dedicated-server registries identical.
        mod.on("lifecycle.register", [this](KyubuEvent&) { mod.add_model("hello_native:crate", "hello-native-crate.cub"); });
        mod.on("lifecycle.game_ready", [this](KyubuEvent&) {
            mod.info("game ready; hello_native:crate is model slot %u", mod.model_slot("hello_native:crate"));
        });
        mod.on("lifecycle.world_enter", [this](KyubuEvent& ev) {
            mod.info("world entered: %s", kyubu::Mod::data<KyubuWorldData>(ev)->world);
        });
        if (m.side() == KY_SIDE_CLIENT) {
            g_client = &m;
            mod.on("frame.begin", [](KyubuEvent&) { ++g_frames; });
            mod.hook("Database::get", &db_get_hook, &g_db_get_next);
            m.api()->register_command(m.handle(), "hello_native:hello", "say hello", cmd_hello, nullptr);
            m.api()->publish_interface(m.handle(), "hello_native:counter", 1, &g_iface);
        }
    }
    ~HelloNative() { mod.info("shutdown after %d frames", g_frames); }
    kyubu::Mod& mod;
};

KYUBU_MOD("hello_native", KY_SIDE_CLIENT | KY_SIDE_SERVER, HelloNative)
