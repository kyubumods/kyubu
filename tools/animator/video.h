// The Animator's offline video: a clip drawn on the rig from start to end, a frame every 1/fps s, with its title in the top-left
// corner, written as an MP4 by ffmpeg (or as PNG frames). Drawn by the sheet's software rasteriser (sheet.h), so it needs no window:
//   kyubu-animator --video <clip> --out <file.mp4> [--title <text>] [--size 1280x720] [--fps 30] [--yaw 35] [--pitch 15]
//                  [--zoom 1] [--follow] [--frame-ms A-B,C-D] [--hold-ms 500] [--clock] [--no-sound] [--volume 1] [--frames <folder>] [--ffmpeg <ffmpeg.exe>]
//                  [--rig ..] [--models ..]
// The sound track mixes the rig's [[sound]]s at their markers (the clip's mod's assets/sounds, then the Sounds panel's folder).
// The camera frames every frame of the clip at once (one fixed camera), or with --follow keeps a fixed distance and tracks the
// figure (for a clip that flies far). Points past 2000 voxels (a model hidden past the far plane by a selector) are not framed.
#pragma once
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "sheet.h"

namespace kanimator {

struct VideoCommand {
    SheetCommand scene;           // the clip, rig, models, library and ability_ms (its times and out_path are not used)
    std::string out_path;         // the MP4; may be empty when frames_dir is given
    std::string frames_dir;       // when set, every frame is also written there as frame_00000.png ...
    std::string title;            // drawn top left; empty: the clip's name
    std::string ffmpeg = "ffmpeg";
    int width = 1280, height = 720, fps = 30;
    int supersample = 2;          // drawn this many times larger, then averaged down (smooth edges)
    float yaw_deg = 35, pitch_deg = 15, zoom = 1;
    bool follow = false;
    bool clock = false;           // the time in the top-right corner
    float hold_ms = 500;          // the last frame is held this long (longer when a sound runs past the clip)
    bool sound = true;            // the rig's [[sound]]s at their markers, mixed into the MP4's sound track (silent when it has none)
    float volume = 1;             // the whole sound track is multiplied by this (0.25 = 12 dB quieter)
    std::vector<std::pair<float, float>> frame_spans;   // ms: the fixed camera frames only these times (none: all); others may leave the picture
};

// 0 = written, 1 = it could not be (the reason on `err`); progress and notes on `out`. Never opens a window.
int run_video_command(const VideoCommand& cmd, std::FILE* out, std::FILE* err);

}  // namespace kanimator
