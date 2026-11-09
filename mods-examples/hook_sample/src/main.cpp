// hook_sample: the example from docs/hooks.md.
// Builds with MinGW (i686) and MSVC (Win32). Every hook here is removed automatically when the game exits.
#include <cstdint>
#include <cstdio>

#include "kyubu.hpp"

namespace {

kyubu::Mod* g_mod = nullptr;

// 1. A __thiscall member function, hooked by symbol name.
//    bool Database::get(const std::string& key, void** out, uint32_t* len)
//    MSVC cannot declare free __thiscall functions, so the handler is a __fastcall with an unused EDX
//    parameter: ECX = this, EDX = unused, the rest on the stack, callee pops - the same ABI as __thiscall.
using DbGetFn = bool(__fastcall*)(void* self, void* edx, const void* key, void** out, uint32_t* len);
DbGetFn g_db_get_next = nullptr;
unsigned g_db_reads = 0;

bool __fastcall db_get_hook(void* self, void* edx, const void* key, void** out, uint32_t* len) {
    ++g_db_reads;
    return g_db_get_next(self, edx, key, out, len);   // always call next unless you replace the function
}

// 2. An imported Windows function, hooked in Cube.exe's import table.
using SleepFn = void(__stdcall*)(uint32_t ms);
SleepFn g_sleep_next = nullptr;
unsigned g_sleeps = 0;

void __stdcall sleep_hook(uint32_t ms) {
    ++g_sleeps;
    g_sleep_next(ms);
}

// 3. A mid-function hook: runs at one instruction with the CPU registers in `ctx`.
//    ModelLoader::resizeSite is the `push 0xA09` that sizes the model table (see docs/registries.md).
void KYUBU_CALL model_table_site(KyubuCpuContext* ctx, void*) {
    g_mod->info("model table sized at 0x%08X (esp=0x%08X)", ctx->eip, ctx->esp);
}

void KYUBU_CALL cmd_stats(const char*, KyubuPrintFn print, void* pctx, void*) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "Database::get calls: %u, Sleep calls: %u", g_db_reads, g_sleeps);
    print(pctx, buf);
}

}  // namespace

struct HookSample {
    explicit HookSample(kyubu::Mod& m) : mod(m) {
        g_mod = &m;
        const KyubuApi* api = m.api();

        if (!mod.hook("Database::get", &db_get_hook, &g_db_get_next))
            mod.warn("Database::get is not available on this build");

        KyubuId id = 0;
        if (api->hook_import(m.handle(), "kernel32.dll", "Sleep", (void*)&sleep_hook, 0, (void**)&g_sleep_next, &id) != KY_OK)
            mod.warn("could not hook Sleep");

        // A mod-defined symbol from symbols/hook_sample.toml resolves like a loader symbol.
        if (uintptr_t a = mod.symbol("hook_sample:ModelLoader::loadAll"))
            mod.info("hook_sample:ModelLoader::loadAll resolved to 0x%08X", (unsigned)a);

        uintptr_t site = mod.symbol("ModelLoader::resizeSite");
        if (site && api->hook_mid(m.handle(), site, model_table_site, nullptr, &id) != KY_OK)
            mod.warn("mid hook refused (see the log for the reason)");

        api->register_command(m.handle(), "hook_sample:stats", "show hook call counts", cmd_stats, nullptr);
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("hook_sample", KY_SIDE_CLIENT, HookSample)
