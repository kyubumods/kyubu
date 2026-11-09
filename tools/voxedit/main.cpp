// kyubu-voxedit: a voxel model editor (docs/dev-tools.md, "kyubu-voxedit, the model editor").
//   kyubu-voxedit [model.cub | figure.rig.toml] [--models <folder>]
//   kyubu-voxedit --version
#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "app.h"

#ifndef KYUBU_VOXEDIT_VERSION
#define KYUBU_VOXEDIT_VERSION "0"
#endif

namespace {

void use_parent_console() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
}

// The arguments as UTF-8 (argv is in the ANSI code page).
std::string arg_utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

int main() {
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    kvox::AppOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = arg_utf8(wargv[i]);
        if (a == "--version") {
            use_parent_console();
            std::printf("kyubu-voxedit %s\n", KYUBU_VOXEDIT_VERSION);
            return 0;
        }
        if (a == "--models" && i + 1 < argc) {
            opt.models_dir = arg_utf8(wargv[++i]);
        } else if (!a.empty() && a[0] != '-' && opt.path.empty()) {
            opt.path = a;
        } else {
            use_parent_console();
            std::fprintf(stderr, "usage: kyubu-voxedit [model.cub | figure.rig.toml] [--models <folder>]\n");
            return 2;
        }
    }
    LocalFree(wargv);
    return kvox::run_app(opt);
}
