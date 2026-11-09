// Everything the loader does after the host is verified: symbols, hooks, mods, events, phases.
#pragma once
#include <string>

#include "kyubu_api.h"

namespace kyubu {
void runtime_boot();
void runtime_shutdown();
// Console helpers
std::string runtime_reload_mod(const std::string& id);
std::string runtime_lua_eval(KyubuSide side, const std::string& code);
// Called by game hooks
void runtime_on_frame_begin();
void runtime_on_frame_end();
void runtime_on_game_ready();
void runtime_on_server_tick(double dt_ms);
}  // namespace kyubu
