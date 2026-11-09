// kyubu-voxedit's window: Win32, Direct3D 9 and Dear ImGui, as the Animator's (tools/animator/app.cpp).
#pragma once
#include <string>

namespace kvox {

struct AppOptions {
    std::string path;         // a .cub or a .rig.toml to open, or empty
    std::string models_dir;   // for a rig: where its models are (empty: the rig's folder)
};

// Opens the window and runs until it is closed. 0 after a normal close, 1 when the window or the device cannot be made.
int run_app(const AppOptions& options);

}  // namespace kvox
