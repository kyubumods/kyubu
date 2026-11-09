#pragma once
#include "common.h"

namespace kyubu {

struct CrashHooks {
    // Returns the id of the mod owning `addr` (its DLL or generated code), or empty.
    std::function<std::string(uintptr_t)> mod_for_address;
    // Mod whose callback is currently running on this thread, or empty.
    std::function<std::string()> active_mod;
};

void crash_install(const std::string& kyubu_dir, CrashHooks hooks);
void crash_set_hooks(CrashHooks hooks);

// Writes a crash report for the given exception (also used by tests). Returns the report path.
std::string crash_write_report(EXCEPTION_POINTERS* ep, const char* reason);

// At boot: handles kyubu/crash-pending.json from a previous crash. `policy` is "ask", "disable" or "ignore".
// Returns the mod id the user/policy chose to disable (empty when none).
std::string crash_handle_pending(const std::string& kyubu_dir, const std::string& policy);

}  // namespace kyubu
