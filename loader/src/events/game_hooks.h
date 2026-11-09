// Hooks into the game that produce loader events (frame, render, input, world, network, exit).
#pragma once
#include "common.h"

namespace kyubu {

void install_client_hooks();
void install_server_hooks();
void install_common_hooks();

// World session tracking
const std::string& current_world();
bool world_loaded();

HWND game_window();

// True on the thread that loaded the loader: the game's main thread, which runs its frame loop and draws.
bool on_main_thread();

}  // namespace kyubu
