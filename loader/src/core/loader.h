// Global loader state shared by all subsystems.
#pragma once
#include "common.h"
#include "config.h"
#include "host.h"

#include <atomic>

namespace kyubu {

struct Loader {
    HostInfo host;
    Config config;
    std::string game_dir;
    std::string kyubu_dir;     // <game>/kyubu
    std::string mods_dir;      // <game>/mods
    std::string config_path;   // <game>/kyubu/config.toml
    HMODULE self_module = nullptr;

    bool passthrough = true;   // true until the build is verified
    bool safe_mode = false;
    std::string safe_reason;
    bool dev_mode = false;
    bool test_mode = false;    // --kyubu-test given
    std::string test_suite;
    std::vector<std::wstring> args;

    std::atomic<KyubuPhase> phase{KY_PHASE_BOOT};
    KyubuHostMode mode = KY_MODE_MENU_OR_SINGLEPLAYER;

    bool has_arg(const wchar_t* a) const;
    std::wstring arg_value(const wchar_t* a) const;   // "--x value" or "--x=value"
};

Loader& L();

// Defined in boot.cpp
void boot_main();
void shutdown_main();

}  // namespace kyubu
