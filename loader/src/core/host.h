#pragma once
#include "common.h"

namespace kyubu {

struct KnownBuild {
    const char* build_id;
    KyubuHost host;
    const char* exe_name;
    uint32_t timestamp;
    uint32_t image_size;
    uint32_t entry_rva;
    const char* text_sha256;   // SHA-256 of the .text raw data in the file on disk
};

struct HostInfo {
    KyubuHost host = KY_HOST_UNKNOWN;
    std::string build_id;         // empty when unsupported
    uintptr_t base = 0;
    uint32_t image_size = 0;
    uint32_t timestamp = 0;
    uint32_t entry_rva = 0;
    std::string exe_path;
    std::string game_dir;
    std::string text_sha256;      // filled by verify_text_hash()
    bool header_match = false;    // timestamp + size matched a known build
    bool verified = false;        // .text hash matched too
};

// Cheap identification from the in-memory PE header (safe inside DllMain).
HostInfo identify_host_header();
// Full identification: hashes .text of the executable file on disk. Call outside DllMain.
bool verify_text_hash(HostInfo& h);

const KnownBuild* find_known_build(uint32_t timestamp, uint32_t image_size);
const std::vector<KnownBuild>& known_builds();

// PE helpers on a mapped image
IMAGE_NT_HEADERS* nt_headers(uintptr_t base);
bool section_range(uintptr_t base, const char* name, uintptr_t* start, uint32_t* size);
bool is_executable_address(uintptr_t addr);   // inside an executable section of any loaded module

// Restart (mod-management D3). restart_game starts the same executable with the same command line and working
// folder, hands the child an inheritable handle of this process (environment KYUBU_RESTART_PARENT), then asks the
// game to quit through its own quit flag, so Shutdown runs on the game thread as for a normal exit. It refuses
// (KY_E_SIDE: Server.exe has no start menu; KY_E_PHASE: before GameReady, in a joined game or with a world loaded;
// KY_E_UNAVAILABLE: the game controller is not known; KY_E_INTERNAL: CreateProcess failed) before it starts
// anything. A second call while a restart is under way returns KY_OK and does nothing.
KyubuResult restart_game(std::string* why = nullptr);

// What a restarted process found at boot.
struct RestartWait {
    bool restarted = false;     // KYUBU_RESTART_PARENT was set
    bool ended = false;         // the previous process had ended when the wait returned
    DWORD waited_ms = 0;
    DWORD error = 0;            // the wait failed with this error (the handle was not inherited)
};
// Boot calls this before it opens the log: waits up to 10 s for the process that restarted this one, so the two
// never share latest.log, and removes the variable from the environment.
RestartWait restart_wait_for_parent();

}  // namespace kyubu
