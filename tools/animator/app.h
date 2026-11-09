// The Animator's window: a Win32 window with a Direct3D 9 device and Dear ImGui (openspec add-ability-animation, tasks 6.1 and 6.3).
// It owns the main loop. The view, the parts list and the play controls are drawn from here; the timeline and the other panels of
// 6.5 to 6.9 are added to it.
#pragma once
#include <string>

namespace kanimator {

struct AppOptions {
    std::string clip_path;    // a clip file to show, or empty for a window with no clip (6.7 adds New and Open)
    std::string rig_path;     // a rig file, or empty for the stand-in rig (rigs/standin.rig.toml beside the program)
    std::string models_dir;   // a folder of .cub models for a rig's named models, or empty: a named model is drawn as a box (6.8)
    std::string library_dir;  // the pose library folder for this run, or empty: the one kept in the settings (6.8)
};

// Opens the window and runs until it is closed. Returns the process exit code: 0 after a normal close, 1 when the window or
// the device cannot be created (a message box says why).
int run_app(const AppOptions& options);

}  // namespace kanimator
