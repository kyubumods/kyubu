// Entry point of the proxy DLL: identifies the host in DllMain, detours the executable's
// entry point, and runs the full boot on the main thread once the loader lock is released.
#include "crash.h"

#include <shellapi.h>

#include "guard.h"
#include "headless.h"
#include "host.h"
#include "loader.h"
#include "log.h"
#include "manage.h"
#include "runtime.h"
#include "ui.h"

namespace kyubu {

Loader& L() {
    static Loader* l = new Loader();
    return *l;
}

bool Loader::has_arg(const wchar_t* a) const {
    for (auto& s : args) if (_wcsicmp(s.c_str(), a) == 0) return true;
    return false;
}

std::wstring Loader::arg_value(const wchar_t* a) const {
    size_t n = wcslen(a);
    for (size_t i = 0; i < args.size(); ++i) {
        if (_wcsicmp(args[i].c_str(), a) == 0 && i + 1 < args.size()) return args[i + 1];
        if (args[i].size() > n && _wcsnicmp(args[i].c_str(), a, n) == 0 && args[i][n] == L'=') return args[i].substr(n + 1);
    }
    return {};
}

namespace {

uint8_t g_entry_saved[5];
uintptr_t g_entry = 0;

void parse_args() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 0; i < argc; ++i) L().args.emplace_back(argv[i]);
    LocalFree(argv);
}

void detect_safe_mode() {
    Loader& l = L();
    if (l.has_arg(L"--kyubu-safe")) { l.safe_mode = true; l.safe_reason = "--kyubu-safe on the command line"; }
    else if (GetAsyncKeyState(VK_SHIFT) & 0x8000) { l.safe_mode = true; l.safe_reason = "Shift held at startup"; }
    else if (file_exists(join_path(l.kyubu_dir, "SAFE"))) { l.safe_mode = true; l.safe_reason = "kyubu/SAFE exists"; }
}

}  // namespace

void boot_main() {
    Loader& l = L();
    guard_init();
    parse_args();
    l.game_dir = l.host.game_dir;
    l.kyubu_dir = join_path(l.game_dir, "kyubu");
    l.mods_dir = join_path(l.game_dir, "mods");
    l.config_path = join_path(l.kyubu_dir, "config.toml");
    l.config = Config::load(l.config_path);
    l.dev_mode = l.config.dev_mode || l.has_arg(L"--kyubu-dev");
    l.test_mode = l.has_arg(L"--kyubu-test") || !l.arg_value(L"--kyubu-test").empty();
    l.test_suite = narrow(l.arg_value(L"--kyubu-test"));

    // --kyubu-headless (add-native-replay): dev mode only.
    const auto hl = headless::resolve_flag(l.has_arg(L"--kyubu-headless"), l.dev_mode);
    headless::set_enabled(hl == headless::FlagResult::On && l.host.host == KY_HOST_CLIENT);

    // A process started by game_restart waits for the old one to end before it takes the log (mod-management D3).
    const RestartWait restart = restart_wait_for_parent();

    Log& log = Log::get();
    const std::string& lv = l.config.log_level;
    KyubuLogLevel min = lv == "debug" ? KY_LOG_DEBUG : lv == "warn" ? KY_LOG_WARN : lv == "error" ? KY_LOG_ERROR : KY_LOG_INFO;
    log.set_min_level(l.dev_mode ? KY_LOG_DEBUG : min);
    log.open(join_path(l.kyubu_dir, "logs"));
    const char* host_name = l.host.host == KY_HOST_CLIENT ? "Cube.exe" : l.host.host == KY_HOST_SERVER ? "Server.exe" : "unknown";
    KINFO("Kyubu %s (API %d.%d) starting in %s (%s)", KYUBU_VERSION_STRING, KYUBU_API_VERSION_MAJOR, KYUBU_API_VERSION_MINOR, host_name,
          l.host.exe_path.c_str());
    KINFO("Command line: %s", narrow(GetCommandLineW()).c_str());
    if (hl == headless::FlagResult::Refused) KWARN("--kyubu-headless refused: it needs dev mode (--kyubu-dev or loader.dev_mode = true)");
    else if (hl == headless::FlagResult::On && !headless::enabled()) KWARN("--kyubu-headless ignored: only Cube.exe has a window");
    else if (headless::enabled()) KINFO("headless: on (off-screen no-activate window, synthetic input, real input dropped)");
    if (restart.restarted)
        KINFO("Restarted by game_restart: the previous process %s after %lu ms%s", restart.ended ? "ended" : "had not ended",
              (unsigned long)restart.waited_ms, restart.error ? fmt(" (wait failed, error %lu)", (unsigned long)restart.error).c_str() : "");
    for (auto& e : l.config.errors) KERROR("config.toml: %s", e.c_str());

    if (!verify_text_hash(l.host)) {
        KWARN("Unsupported game build (timestamp 0x%08X, size 0x%X, .text sha256 %s). "
              "Running in passthrough mode: no hooks, no mods.",
              l.host.timestamp, l.host.image_size, l.host.text_sha256.c_str());
        l.passthrough = true;
        return;
    }
    l.passthrough = false;
    KINFO("Host build %s verified (image base 0x%08X)", l.host.build_id.c_str(), (unsigned)l.host.base);

    crash_install(l.kyubu_dir, {});
    detect_safe_mode();
    if (l.safe_mode) {
        KWARN("SAFE MODE (%s): all mods are disabled for this session", l.safe_reason.c_str());
    } else {
        std::string disable = crash_handle_pending(l.kyubu_dir, l.config.on_mod_crash);
        if (!disable.empty()) {
            Config::set_mod_enabled(l.config_path, disable, false);
            l.config.mod_enabled[disable] = false;
            KWARN("Mod '%s' disabled after the previous crash", disable.c_str());
        }
    }
    // The queue ModManager wrote last session (D2): applied before the mod scan, and skipped in safe mode, which
    // keeps the file for the next normal start. A boot with no pending file only purges the old backups.
    const PendingResult pending = apply_pending();
    const std::string pending_error = pending_notice(pending);
    if (!pending_error.empty() && l.host.host == KY_HOST_CLIENT) Ui::get().notice(pending_error, 20);
    runtime_boot();   // everything after M0: symbols, hooks, mods, events...
}

void shutdown_main() { runtime_shutdown(); }

}  // namespace kyubu

using namespace kyubu;

namespace {
bool g_vanilla_missing = false;
wchar_t g_vanilla_path[MAX_PATH];
}  // namespace

extern "C" void __cdecl kyubu_entry_detour() {
    // Restore the original entry bytes first so the game starts normally whatever happens.
    DWORD old;
    VirtualProtect((void*)g_entry, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)g_entry, g_entry_saved, 5);
    VirtualProtect((void*)g_entry, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)g_entry, 5);

    if (g_vanilla_missing) {
        // Every zlib export forwards to the original DLL; without it the game cannot work. Stop before any game code runs.
        std::wstring msg = std::wstring(L"Kyubu cannot start the game: the original zlib library is missing.\n\nExpected file:\n") +
                           g_vanilla_path +
                           L"\n\nReinstall Kyubu, or rename the original zlib1.dll to zlib1_vanilla.dll "
                           L"(a copy is kept in kyubu\\backup\\zlib1.dll).";
        MessageBoxW(nullptr, msg.c_str(), L"Kyubu", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        ExitProcess(1);
    }
    if (!L().host.header_match) { reinterpret_cast<void(__cdecl*)()>(g_entry)(); return; }   // passthrough

    DWORD fault = guarded("kyubu", [] { boot_main(); });
    if (fault) {
        KERROR("Kyubu boot failed with exception 0x%08lX (%s); continuing without mods", fault, exception_name(fault));
        L().passthrough = true;
    }
    reinterpret_cast<void(__cdecl*)()>(g_entry)();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        L().self_module = inst;
        // The original zlib is the forwarding target of every export. Windows refuses to start the game
        // without it; Wine binds the imports to stubs instead, so check here and stop at the entry point.
        GetModuleFileNameW(inst, g_vanilla_path, MAX_PATH);
        if (wchar_t* slash = wcsrchr(g_vanilla_path, L'\\')) wcscpy(slash + 1, L"zlib1_vanilla.dll");
        g_vanilla_missing = !GetModuleHandleW(L"zlib1_vanilla.dll") &&
                            GetFileAttributesW(g_vanilla_path) == INVALID_FILE_ATTRIBUTES;
        L().host = identify_host_header();
        if (!L().host.header_match && !g_vanilla_missing) return TRUE;   // passthrough: forwarders only
        if (!L().host.entry_rva) return TRUE;
        g_entry = L().host.base + L().host.entry_rva;
        memcpy(g_entry_saved, (void*)g_entry, 5);
        uint8_t jmp[5] = {0xE9};
        int32_t rel = (int32_t)((uintptr_t)&kyubu_entry_detour - (g_entry + 5));
        memcpy(jmp + 1, &rel, 4);
        DWORD old;
        if (VirtualProtect((void*)g_entry, 5, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy((void*)g_entry, jmp, 5);
            VirtualProtect((void*)g_entry, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void*)g_entry, 5);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        // Nothing: shutdown runs from the ExitProcess hook while the process is still intact.
    }
    return TRUE;
}
