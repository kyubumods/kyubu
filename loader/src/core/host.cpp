#include "host.h"

#include <atomic>

#include "cubeworld/types.h"
#include "game_hooks.h"
#include "guard.h"
#include "log.h"
#include "loader.h"
#include "sha256.h"
#include "symbols.h"

namespace kyubu {

const std::vector<KnownBuild>& known_builds() {
    // The 2013-07-20 builds. The loader is compiled against their layouts (types.h: the
    // GameController has one more 8-byte member than the 07-02 build), so it runs on nothing else.
    static const std::vector<KnownBuild> builds = {
        {"cube-0x51EA955E", KY_HOST_CLIENT, "Cube.exe", 0x51EA955E, 0x3BC000, 0x28E1E0,
         "95ceb4cd764615977fb9874174944bfdb2608d578dd737bc280c7adee93f632e"},
        {"server-0x51EA94D5", KY_HOST_SERVER, "Server.exe", 0x51EA94D5, 0x1A8000, 0x14B44B,
         "613ad48e070e3aabb17fe55161f981e2d3af22be59489a347fbe949a3a5de486"},
    };
    return builds;
}

const KnownBuild* find_known_build(uint32_t timestamp, uint32_t image_size) {
    for (auto& b : known_builds())
        if (b.timestamp == timestamp && b.image_size == image_size) return &b;
    return nullptr;
}

IMAGE_NT_HEADERS* nt_headers(uintptr_t base) {
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

bool section_range(uintptr_t base, const char* name, uintptr_t* start, uint32_t* size) {
    auto nt = nt_headers(base);
    if (!nt) return false;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (strncmp((const char*)sec->Name, name, 8) == 0) {
            *start = base + sec->VirtualAddress;
            *size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

bool is_executable_address(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

HostInfo identify_host_header() {
    HostInfo h;
    h.base = (uintptr_t)GetModuleHandleW(nullptr);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    h.exe_path = narrow(path);
    h.game_dir = dir_of(h.exe_path);
    auto nt = nt_headers(h.base);
    if (!nt) return h;
    h.timestamp = nt->FileHeader.TimeDateStamp;
    h.image_size = nt->OptionalHeader.SizeOfImage;
    h.entry_rva = nt->OptionalHeader.AddressOfEntryPoint;
    if (auto kb = find_known_build(h.timestamp, h.image_size)) {
        h.header_match = kb->entry_rva == h.entry_rva;
        h.host = kb->host;
        h.build_id = kb->build_id;
    } else {
        // Still tell client from server by name so passthrough logs make sense.
        std::string n = lower(file_name(h.exe_path));
        h.host = n == "server.exe" ? KY_HOST_SERVER : (n == "cube.exe" ? KY_HOST_CLIENT : KY_HOST_UNKNOWN);
    }
    return h;
}

bool verify_text_hash(HostInfo& h) {
    auto data = read_file(h.exe_path);
    if (!data || data->size() < 0x400) return false;
    auto base = (uintptr_t)data->data();
    auto nt = nt_headers(base);
    if (!nt) return false;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (strncmp((const char*)sec->Name, ".text", 8) != 0) continue;
        uint32_t len = sec->SizeOfRawData < sec->Misc.VirtualSize ? sec->SizeOfRawData : sec->Misc.VirtualSize;
        if (sec->PointerToRawData + len > data->size()) return false;
        h.text_sha256 = Sha256::hex_of(data->data() + sec->PointerToRawData, len);
        break;
    }
    auto kb = find_known_build(h.timestamp, h.image_size);
    h.verified = h.header_match && kb && h.text_sha256 == kb->text_sha256;
    if (!h.verified) h.build_id.clear();
    return h.verified;
}

namespace {

const char kRestartEnv[] = "KYUBU_RESTART_PARENT";
constexpr DWORD kRestartWaitMs = 10000;
constexpr DWORD kRestartForceMs = 8000;   // the game did not quit by itself: leave through ExitProcess (its hook runs Shutdown)
std::atomic<bool> g_restarting{false};

KyubuResult restart_refused(KyubuResult r, const std::string& reason, std::string* why) {
    if (why) *why = reason;
    KWARN("game_restart: %s", reason.c_str());
    return r;
}

DWORD WINAPI restart_force_thread(LPVOID) {
    Sleep(kRestartForceMs);
    KWARN("game_restart: the game did not quit in %lu ms; exiting", kRestartForceMs);
    ExitProcess(0);
}

}  // namespace

KyubuResult restart_game(std::string* why) {
    Loader& l = L();
    if (l.host.host != KY_HOST_CLIENT) return restart_refused(KY_E_SIDE, "Server.exe has no start menu to restart from", why);
    if (l.phase.load() != KY_PHASE_GAMEREADY || l.mode != KY_MODE_MENU_OR_SINGLEPLAYER)
        return restart_refused(KY_E_PHASE, "the game restarts only at the start menu, after the loader is ready", why);
    if (world_loaded()) return restart_refused(KY_E_PHASE, "a world is loaded; the game restarts only at the start menu", why);
    if (g_restarting.exchange(true)) return KY_OK;   // already on its way out

    // The quit flag first: nothing is started when the game cannot be told to quit.
    const uintptr_t global = verified_symbol("cube::game_controller");
    uint32_t gc = 0;
    if (!global || !safe_read(global, &gc, sizeof gc) || !gc) {
        g_restarting = false;
        return restart_refused(KY_E_UNAVAILABLE, "the game controller is not known, so the game cannot be told to quit", why);
    }

    wchar_t exe[MAX_PATH];
    wchar_t dir[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH) || !GetCurrentDirectoryW(MAX_PATH, dir)) {
        g_restarting = false;
        return restart_refused(KY_E_INTERNAL, "cannot read the executable path or the working folder", why);
    }
    std::wstring cmd = GetCommandLineW();   // CreateProcess may write into the buffer
    HANDLE inheritable = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &inheritable, SYNCHRONIZE, TRUE, 0)) {
        g_restarting = false;
        return restart_refused(KY_E_INTERNAL, fmt("DuplicateHandle failed (error %lu)", GetLastError()), why);
    }
    SetEnvironmentVariableA(kRestartEnv, fmt("%lu", (unsigned long)(uintptr_t)inheritable).c_str());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, dir, &si, &pi);
    const DWORD err = ok ? 0 : GetLastError();
    SetEnvironmentVariableA(kRestartEnv, nullptr);
    CloseHandle(inheritable);
    if (!ok) {
        g_restarting = false;
        return restart_refused(KY_E_INTERNAL, fmt("CreateProcess failed (error %lu)", err), why);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    KINFO("game_restart: started process %lu with the same arguments; quitting", (unsigned long)pi.dwProcessId);

    const uint8_t one = 1;
    if (!safe_write(gc + offsetof(cube_GameController, quit_requested), &one, 1)) ExitProcess(0);
    if (HANDLE t = CreateThread(nullptr, 0, restart_force_thread, nullptr, 0, nullptr)) CloseHandle(t);
    return KY_OK;
}

RestartWait restart_wait_for_parent() {
    RestartWait r;
    char value[32];
    const DWORD n = GetEnvironmentVariableA(kRestartEnv, value, sizeof value);
    if (n == 0 || n >= sizeof value) return r;
    r.restarted = true;
    SetEnvironmentVariableA(kRestartEnv, nullptr);   // not for our own children
    HANDLE parent = (HANDLE)(uintptr_t)strtoul(value, nullptr, 10);
    const DWORD start = GetTickCount();
    const DWORD w = WaitForSingleObject(parent, kRestartWaitMs);
    r.waited_ms = GetTickCount() - start;
    r.ended = w == WAIT_OBJECT_0;
    if (w == WAIT_FAILED) r.error = GetLastError();
    if (w != WAIT_FAILED) CloseHandle(parent);
    return r;
}

}  // namespace kyubu
