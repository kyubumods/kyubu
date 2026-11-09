// kyubu-animator: the Animator (openspec add-ability-animation, section 6). A standalone program: no game, no loader.
//   kyubu-animator [clip.anim.toml] [--rig <file.rig.toml>] [--models <folder>] [--library <folder>]
//                               opens the window, with the clip shown on the rig (the stand-in rig unless --rig names another)
//   kyubu-animator --sheet <clip> --out <png> [--t 0,0.5,1] [--ability-ms N] [--rig ..] [--models ..] [--library ..]
//                               writes the labelled two-view sheet of the mimic and exits, with no window (task 6.9)
//   kyubu-animator --video <clip> --out <mp4> [--title T] [--size 1280x720] [--fps 30] [--yaw 35] [--pitch 15] [--zoom 1]
//                  [--follow] [--frame-ms A-B,C-D] [--hold-ms 500] [--clock] [--no-sound] [--volume 1] [--frames <folder>]
//                  [--ffmpeg <exe>] [--rig ..] [--models ..] [--library ..]
//                               writes the clip as an MP4 (through ffmpeg) with its title top left and the rig's [[sound]]s
//                               on its sound track, and exits, with no window
//   kyubu-animator --parity <capture> [--parity <capture> ...] --rig <file.rig.toml> [--models ..] [--tolerance-pos N]
//                               [--tolerance-angle N] [--mark]
//                               compares the rig with the game's part matrices of one or more captures and exits, with no window
//                               (task 7.5, the mimic gate); --mark writes `approximate` into the rig file
//   kyubu-animator --version    prints the version and exits with no window (for CI)
// Exit codes: 0 done, 1 the window could not be created or the sheet or the parity could not be written, 2 a usage error,
// 3 the parity report is written and a node is outside the tolerance.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app.h"
#include "parity.h"
#include "sheet.h"
#include "video.h"

namespace {

#ifndef KYUBU_ANIMATOR_VERSION
#define KYUBU_ANIMATOR_VERSION "0"
#endif

const char* const kUsage =
    "usage: kyubu-animator [clip.anim.toml] [--rig <file.rig.toml>] [--models <folder>] [--library <folder>]\n"
    "       kyubu-animator --sheet <clip.anim.toml> --out <sheet.png> [--t 0,0.5,1] [--ability-ms N]\n"
    "                      [--rig <file.rig.toml>] [--models <folder>] [--library <folder>]\n"
    "       kyubu-animator --video <clip.anim.toml> --out <file.mp4> [--title <text>] [--size 1280x720] [--fps 30]\n"
    "                      [--yaw 35] [--pitch 15] [--zoom 1] [--follow] [--frame-ms A-B,C-D] [--hold-ms 500] [--clock]\n"
    "                      [--no-sound] [--volume 1] [--frames <folder>]\n"
    "                      [--ffmpeg <ffmpeg.exe>] [--rig <file.rig.toml>] [--models <folder>] [--library <folder>]\n"
    "       kyubu-animator --parity <capture> [--parity <capture> ...] --rig <file.rig.toml> [--models <folder>]\n"
    "                      [--tolerance-pos N] [--tolerance-angle N] [--mark]\n"
    "       kyubu-animator --version\n";

// The program is a window program: it has no console of its own. For the commands that print, take the console of the
// parent (a shell) when stdout is not already a file or a pipe.
void use_parent_console() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) return;   // redirected: the handle is inherited
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
}

// The stand-in rig, when no --rig is given: beside the program, then the source tree's copy (the window looks in the same two).
std::vector<std::string> rig_candidates() {
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string dir(buf, n);
    const size_t slash = dir.find_last_of("\\/");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash);
    return {dir + "\\rigs\\standin.rig.toml", std::string(KYUBU_ANIMATOR_RIG_DIR) + "/standin.rig.toml"};
}

int usage_error(const char* why) {
    use_parent_console();
    if (why) std::fprintf(stderr, "%s\n", why);
    std::fputs(kUsage, stderr);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        use_parent_console();
        std::printf("kyubu-animator %s\n", KYUBU_ANIMATOR_VERSION);
        return 0;
    }
    kanimator::AppOptions opt;
    kanimator::SheetCommand sheet;
    bool want_sheet = false, sheet_flag = false;
    kanimator::VideoCommand video;
    bool want_video = false, video_flag = false;   // video_flag: a flag of --video that is not --video itself
    kanimator::ParityCommand parity;
    bool parity_flag = false;   // a flag of --parity that is not --parity itself
    // A number above 0 that follows a flag; false (and a usage error to return) when it is not one.
    auto positive = [](const char* v, double& out) {
        char* end = nullptr;
        out = std::strtod(v, &end);
        return end != v && *end == 0 && out > 0;
    };
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool has_value = i + 1 < argc;
        auto number = [&](double& v) {
            char* end = nullptr;
            v = std::strtod(argv[++i], &end);
            return end != argv[i] && *end == 0;
        };
        double v = 0;
        if (std::strcmp(a, "--video") == 0 && has_value && !want_video) {
            want_video = true;
            video.scene.clip_path = argv[++i];
        } else if (std::strcmp(a, "--title") == 0 && has_value) {
            video_flag = true;
            video.title = argv[++i];
        } else if (std::strcmp(a, "--frames") == 0 && has_value) {
            video_flag = true;
            video.frames_dir = argv[++i];
        } else if (std::strcmp(a, "--ffmpeg") == 0 && has_value) {
            video_flag = true;
            video.ffmpeg = argv[++i];
        } else if (std::strcmp(a, "--size") == 0 && has_value) {
            video_flag = true;
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) != 2) return usage_error("--size takes WxH, such as 1280x720");
            video.width = w;
            video.height = h;
        } else if ((std::strcmp(a, "--fps") == 0 || std::strcmp(a, "--yaw") == 0 || std::strcmp(a, "--pitch") == 0 ||
                    std::strcmp(a, "--zoom") == 0 || std::strcmp(a, "--hold-ms") == 0 || std::strcmp(a, "--supersample") == 0 ||
                    std::strcmp(a, "--volume") == 0) &&
                   has_value) {
            video_flag = true;
            if (!number(v)) return usage_error("--fps, --yaw, --pitch, --zoom, --hold-ms, --supersample and --volume take a number");
            if (std::strcmp(a, "--fps") == 0) video.fps = static_cast<int>(v);
            else if (std::strcmp(a, "--yaw") == 0) video.yaw_deg = static_cast<float>(v);
            else if (std::strcmp(a, "--pitch") == 0) video.pitch_deg = static_cast<float>(v);
            else if (std::strcmp(a, "--zoom") == 0) video.zoom = static_cast<float>(v);
            else if (std::strcmp(a, "--hold-ms") == 0) video.hold_ms = static_cast<float>(v);
            else if (std::strcmp(a, "--volume") == 0) video.volume = static_cast<float>(v);
            else video.supersample = static_cast<int>(v);
        } else if (std::strcmp(a, "--follow") == 0) {
            video_flag = true;
            video.follow = true;
        } else if (std::strcmp(a, "--clock") == 0) {
            video_flag = true;
            video.clock = true;
        } else if (std::strcmp(a, "--no-sound") == 0) {
            video_flag = true;
            video.sound = false;
        } else if (std::strcmp(a, "--frame-ms") == 0 && has_value) {
            video_flag = true;
            // One or more spans, comma-separated: 0-2400,4600-7170.
            const std::string spans = argv[++i];
            size_t at = 0;
            while (at <= spans.size()) {
                const size_t comma = std::min(spans.find(',', at), spans.size());
                float from = 0, to = 0;
                if (std::sscanf(spans.substr(at, comma - at).c_str(), "%f-%f", &from, &to) != 2 || from < 0 || to < from)
                    return usage_error("--frame-ms takes spans of the clip in ms, such as 0-1500 or 0-1500,3000-4000");
                video.frame_spans.emplace_back(from, to);
                at = comma + 1;
            }
        } else if (std::strcmp(a, "--sheet") == 0 && has_value && !want_sheet) {
            want_sheet = true;
            sheet.clip_path = argv[++i];
        } else if (std::strcmp(a, "--parity") == 0 && has_value) {
            parity.capture_paths.push_back(argv[++i]);
        } else if (std::strcmp(a, "--mark") == 0) {
            parity_flag = true;
            parity.mark = true;
        } else if ((std::strcmp(a, "--tolerance-pos") == 0 || std::strcmp(a, "--tolerance-angle") == 0) && has_value) {
            parity_flag = true;
            const bool is_pos = std::strcmp(a, "--tolerance-pos") == 0;
            double v = 0;
            if (!positive(argv[++i], v)) return usage_error("--tolerance-pos and --tolerance-angle take a number above 0");
            (is_pos ? parity.tol.pos : parity.tol.angle) = v;
        } else if (std::strcmp(a, "--t") == 0 && has_value) {
            sheet_flag = true;
            sheet.times = argv[++i];
        } else if (std::strcmp(a, "--out") == 0 && has_value) {
            sheet_flag = true;
            sheet.out_path = argv[++i];
        } else if (std::strcmp(a, "--ability-ms") == 0 && has_value) {
            sheet_flag = true;
            const char* v = argv[++i];
            char* end = nullptr;
            const float ms = std::strtof(v, &end);
            if (end == v || *end != 0 || !(ms > 0)) return usage_error("--ability-ms takes a number of milliseconds above 0");
            sheet.ability_ms = ms;
        } else if ((std::strcmp(a, "--rig") == 0 || std::strcmp(a, "--models") == 0 || std::strcmp(a, "--library") == 0) && has_value) {
            (std::strcmp(a, "--rig") == 0 ? opt.rig_path : std::strcmp(a, "--models") == 0 ? opt.models_dir : opt.library_dir) = argv[++i];
        } else if (a[0] != '-' && opt.clip_path.empty()) {
            opt.clip_path = a;
        } else {
            return usage_error(nullptr);
        }
    }
    if (!parity.capture_paths.empty() || parity_flag) {
        if (parity.capture_paths.empty()) return usage_error("--mark and the tolerances belong to --parity <capture>");
        if (want_sheet || sheet_flag) return usage_error("--sheet and --parity are separate commands");
        if (!opt.clip_path.empty()) return usage_error("--parity takes no clip");
        if (opt.rig_path.empty()) return usage_error("--parity needs --rig <file.rig.toml>");
        use_parent_console();
        parity.rig_path = opt.rig_path;
        parity.models_dir = opt.models_dir;
        return kanimator::run_parity_command(parity, stdout, stderr);
    }
    if (want_video || video_flag) {
        if (!want_video) return usage_error("--title, --size, --fps, --yaw, --pitch, --zoom, --follow, --hold-ms, --clock, --frames and --ffmpeg belong to --video <clip>");
        if (want_sheet) return usage_error("--video and --sheet are separate commands");
        if (!opt.clip_path.empty()) return usage_error("give the clip once: --video <clip>");
        use_parent_console();
        video.out_path = sheet.out_path;
        video.scene.ability_ms = sheet.ability_ms;
        video.scene.rig_path = opt.rig_path;
        video.scene.models_dir = opt.models_dir;
        video.scene.library_dir = opt.library_dir;
        video.scene.rig_candidates = rig_candidates();
        return kanimator::run_video_command(video, stdout, stderr);
    }
    if (want_sheet || sheet_flag) {
        if (!want_sheet) return usage_error("--t, --out and --ability-ms belong to --sheet <clip>");
        if (sheet.out_path.empty()) return usage_error("--sheet needs --out <sheet.png>");
        if (!opt.clip_path.empty()) return usage_error("give the clip once: --sheet <clip>");
        use_parent_console();
        sheet.rig_path = opt.rig_path;
        sheet.models_dir = opt.models_dir;
        sheet.library_dir = opt.library_dir;
        sheet.rig_candidates = rig_candidates();
        return kanimator::run_sheet_command(sheet, stdout, stderr);
    }
    return kanimator::run_app(opt);
}
