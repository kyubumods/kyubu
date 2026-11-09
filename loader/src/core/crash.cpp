#include "crash.h"

#include <dbghelp.h>

#include "guard.h"
#include "hooks.h"
#include "json.hpp"
#include "log.h"

namespace kyubu {

namespace {

std::string g_dir;
CrashHooks g_hooks;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;
volatile LONG g_in_crash = 0;

std::string find_implicated(EXCEPTION_POINTERS* ep, std::string* how) {
    auto try_addr = [&](uintptr_t a, const char* what) -> std::string {
        if (!g_hooks.mod_for_address) return {};
        std::string m = g_hooks.mod_for_address(a);
        if (!m.empty() && how) *how = what;
        return m;
    };
    uintptr_t ip = ep ? (uintptr_t)ep->ExceptionRecord->ExceptionAddress : 0;
    if (auto m = try_addr(ip, "faulting address is inside the mod"); !m.empty()) return m;
    if (g_hooks.active_mod) {
        std::string m = g_hooks.active_mod();
        if (!m.empty()) { if (how) *how = "a callback of the mod was running"; return m; }
    }
    if (const char* o = current_guard_owner(); o && strcmp(o, "kyubu") != 0) {
        if (how) *how = "a guarded callback of the mod was running";
        return o;
    }
    if (ep) {
        auto sp = (const uint32_t*)ep->ContextRecord->Esp;
        for (int i = 0; i < 2048; ++i) {
            uint32_t v;
            if (!safe_read((uintptr_t)(sp + i), &v, 4)) break;
            if (auto m = try_addr(v, "a return address on the stack belongs to the mod"); !m.empty()) return m;
        }
    }
    return {};
}

void stack_trace(EXCEPTION_POINTERS* ep, std::string& out) {
    if (!ep) return;
    CONTEXT* c = ep->ContextRecord;
    out += "Stack (frame chain):\r\n";
    out += "  " + describe_address(c->Eip) + "\r\n";
    uint32_t ebp = c->Ebp;
    for (int i = 0; i < 64 && ebp; ++i) {
        uint32_t frame[2];
        if (!safe_read(ebp, frame, 8)) break;
        if (!frame[1]) break;
        out += "  " + describe_address(frame[1]) + "\r\n";
        if (frame[0] <= ebp) break;
        ebp = frame[0];
    }
    out += "Stack (scan for code addresses near ESP):\r\n";
    int found = 0;
    for (int i = 0; i < 1024 && found < 40; ++i) {
        uint32_t v;
        if (!safe_read(c->Esp + i * 4, &v, 4)) break;
        if (module_of(v) && describe_address(v).find('+') != std::string::npos) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((void*)(uintptr_t)v, &mbi, sizeof mbi) && (mbi.Protect & 0xF0)) {
                out += fmt("  [esp+%04X] ", i * 4) + describe_address(v) + "\r\n";
                ++found;
            }
        }
    }
}

void write_minidump(const std::string& path, EXCEPTION_POINTERS* ep) {
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) return;
    using Fn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                             PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto fn = (Fn)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (!fn) return;
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    fn(GetCurrentProcess(), GetCurrentProcessId(), f, (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory),
       ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
}

std::string json_escape(const std::string& in) {
    std::string o;
    for (char c : in) {
        if (c == '\\' || c == '"') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

LONG WINAPI top_filter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_in_crash, 1) == 0) crash_write_report(ep, "unhandled exception");
    if (g_prev_filter) return g_prev_filter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

std::string crash_write_report(EXCEPTION_POINTERS* ep, const char* reason) {
    std::string stamp = now_stamp();
    std::string logs = join_path(g_dir, "logs");
    make_dirs(logs);
    std::string path = join_path(logs, "crash-" + stamp + ".txt");
    std::string how;
    std::string mod = find_implicated(ep, &how);

    std::string r = "Kyubu crash report\r\n==================\r\n";
    r += "Reason: " + std::string(reason) + "\r\n";
    if (ep) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        r += fmt("Exception: 0x%08lX (%s)\r\n", code, exception_name(code));
        r += "Faulting address: " + describe_address((uintptr_t)ep->ExceptionRecord->ExceptionAddress) + "\r\n";
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
            r += fmt("Access: %s 0x%08lX\r\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                     (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
        CONTEXT* c = ep->ContextRecord;
        r += fmt("Registers: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX EIP=%08lX\r\n",
                 c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip);
    }
    r += "Thread: " + std::to_string(GetCurrentThreadId()) + "\r\n";
    // A 32-bit game runs out of address space (2 GB) or video memory before it runs out of RAM, and its resource
    // creations then return null; this line tells such a crash from a bug.
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem))
        r += fmt("Memory: address space %lu of %lu MB free, physical %lu of %lu MB free\r\n",
                 (unsigned long)(mem.ullAvailVirtual >> 20), (unsigned long)(mem.ullTotalVirtual >> 20),
                 (unsigned long)(mem.ullAvailPhys >> 20), (unsigned long)(mem.ullTotalPhys >> 20));
    r += "Implicated mod: " + (mod.empty() ? std::string("(none)") : mod + " - " + how) + "\r\n\r\n";
    stack_trace(ep, r);
    r += "\r\nLast log lines:\r\n";
    for (auto& l : Log::get().tail(200)) r += "  " + l.text + "\r\n";
    write_text(path, r);

    // The pending marker goes first: writing the minidump is the step most likely to fail in a broken process.
    std::string pending = "{\n  \"report\": \"" + json_escape(path) + "\",\n  \"mod\": \"" + json_escape(mod) +
                          "\",\n  \"time\": \"" + stamp + "\"\n}\n";
    write_text(join_path(g_dir, "crash-pending.json"), pending);

    std::string dump = join_path(logs, "crash-" + stamp + ".dmp");
    write_minidump(dump, ep);
    Log::get().write(KY_LOG_ERROR, "kyubu", KY_SIDE_NONE,
                     "Crash report written: " + path + (mod.empty() ? "" : " (implicated mod: " + mod + ")"));
    return path;
}

namespace {
using SetUefFn = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetUefFn g_set_uef_real = nullptr;
// Keeps Kyubu's filter on top: filters installed later by the game or its CRT are chained behind it.
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI set_uef_hook(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    if (f == top_filter) return g_set_uef_real(f);
    LPTOP_LEVEL_EXCEPTION_FILTER old = g_prev_filter;
    g_prev_filter = f;
    return old;
}
}  // namespace

void crash_install(const std::string& kyubu_dir, CrashHooks hooks) {
    g_dir = kyubu_dir;
    g_hooks = std::move(hooks);
    g_prev_filter = SetUnhandledExceptionFilter(top_filter);
    // Protect the filter: hook SetUnhandledExceptionFilter so later callers chain behind us.
    if (Hooks::get().init()) {
        auto target = (uintptr_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetUnhandledExceptionFilter");
        std::string err;
        if (target) Hooks::get().hook_function("kyubu", target, (void*)&set_uef_hook, kLoaderPriority, (void**)&g_set_uef_real, nullptr, &err);
    }
}

void crash_set_hooks(CrashHooks hooks) { g_hooks = std::move(hooks); }

std::string crash_handle_pending(const std::string& kyubu_dir, const std::string& policy) {
    std::string p = join_path(kyubu_dir, "crash-pending.json");
    auto data = read_file(p);
    if (!data) return {};
    DeleteFileW(widen(p).c_str());
    auto j = nlohmann::json::parse(data->begin(), data->end(), nullptr, false);
    if (j.is_discarded()) return {};
    std::string mod = j.value("mod", "");
    std::string report = j.value("report", "");
    KWARN("The previous session crashed. Report: %s", report.c_str());
    if (mod.empty() || policy == "ignore") return {};
    bool disable = policy == "disable";
    if (policy == "ask") {
        std::wstring msg = L"The last Cube World session crashed, and the mod \"" + widen(mod) +
                           L"\" was implicated.\n\nReport: " + widen(report) +
                           L"\n\nDisable this mod before loading mods?";
        disable = MessageBoxW(nullptr, msg.c_str(), L"Kyubu - crash recovery",
                              MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND) == IDYES;
    }
    return disable ? mod : std::string();
}

}  // namespace kyubu
