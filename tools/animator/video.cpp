// The Animator's offline video: see video.h.
#include "video.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <fstream>
#include <iterator>

#include "effects.h"
#include "particles.h"
#include "sounds.h"

namespace kanimator {

namespace {

constexpr float kFarPoint = 2000;   // a point further than this from the origin is a hidden model: not framed

// ffmpeg reading raw RGB frames on its stdin, started with no console window (the Animator is a window program).
class Encoder {
public:
    // `wav_path`, when not empty: the sound track, muxed in as AAC.
    bool start(const std::string& ffmpeg, const std::string& out_path, int w, int h, int fps, const std::string& wav_path,
               std::string& why) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE read_end = nullptr;
        if (!CreatePipe(&read_end, &write_, &sa, 1 << 20)) {
            why = "cannot make a pipe for ffmpeg";
            return false;
        }
        SetHandleInformation(write_, HANDLE_FLAG_INHERIT, 0);
        std::string cmd = "\"" + ffmpeg + "\" -y -loglevel error -f rawvideo -pix_fmt rgb24 -s " + std::to_string(w) + "x" +
                          std::to_string(h) + " -r " + std::to_string(fps) + " -i -" +
                          (wav_path.empty() ? std::string() : " -i \"" + wav_path + "\" -c:a aac -b:a 192k") +
                          " -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -movflags +faststart \"" + out_path + "\"";
        STARTUPINFOA si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = read_end;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(read_end);
        if (!ok) {
            CloseHandle(write_);
            write_ = nullptr;
            why = "cannot start ffmpeg (" + ffmpeg + "); give its path with --ffmpeg";
            return false;
        }
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        return true;
    }
    bool write(const anim_capture::Image& img) {
        const char* p = reinterpret_cast<const char*>(img.rgb.data());
        size_t left = img.rgb.size();
        while (left > 0) {
            DWORD n = 0;
            if (!WriteFile(write_, p, static_cast<DWORD>(std::min<size_t>(left, 1 << 20)), &n, nullptr) || n == 0) return false;
            p += n;
            left -= n;
        }
        return true;
    }
    // Closes its input and waits: the exit code of ffmpeg (0 = the file is written).
    int finish() {
        if (write_) CloseHandle(write_);
        write_ = nullptr;
        if (!process_) return -1;
        WaitForSingleObject(process_, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process_, &code);
        CloseHandle(process_);
        process_ = nullptr;
        return static_cast<int>(code);
    }
    ~Encoder() { finish(); }

private:
    HANDLE write_ = nullptr;
    HANDLE process_ = nullptr;
};

constexpr int kMixRate = 48000;   // the sound track: stereo, this many frames a second

bool read_all_text(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// One sample of a .wav's data as -1 to 1.
float wav_sample(const WavInfo& w, const uint8_t* p) {
    if (w.format == 3) {
        float f;
        std::memcpy(&f, p, 4);
        return f;
    }
    switch (w.bits) {
    case 8: return (static_cast<int>(p[0]) - 128) / 128.0f;
    case 16: return static_cast<int16_t>(p[0] | p[1] << 8) / 32768.0f;
    case 24: return static_cast<float>(static_cast<int32_t>(static_cast<uint32_t>(p[0] << 8 | p[1] << 16 | p[2] << 24)) / 2147483648.0);
    case 32: return static_cast<float>(static_cast<int32_t>(p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24) / 2147483648.0);
    }
    return 0;
}

// Adds a sound into the stereo `mix` from `start_ms`, played at `pitch` (faster and higher above 1, as XAudio2's frequency ratio)
// and `volume`. Mono goes to both sides; past two channels only the first two are heard. The mix grows to hold it.
void mix_sound(std::vector<float>& mix, const LoadedSound& s, float start_ms, float volume, float pitch) {
    const WavInfo& w = s.info;
    const size_t frames = w.block_align ? w.data_size / w.block_align : 0;
    if (!w.ok || frames == 0) return;
    const uint8_t* data = s.bytes.data() + w.data_offset;
    const int bytes_per = w.bits / 8;
    const double step = static_cast<double>(w.rate) * pitch / kMixRate;   // source frames a mix frame
    const size_t out_frames = static_cast<size_t>(std::floor((frames - 1) / step)) + 1;
    const size_t first = static_cast<size_t>(std::lround(std::max(0.0f, start_ms) * kMixRate / 1000.0));
    if (mix.size() < (first + out_frames) * 2) mix.resize((first + out_frames) * 2, 0.0f);
    auto at = [&](size_t f, int ch) { return wav_sample(w, data + f * w.block_align + std::min<int>(ch, w.channels - 1) * bytes_per); };
    for (size_t n = 0; n < out_frames; ++n) {
        const double pos = n * step;
        const size_t i = static_cast<size_t>(pos);
        const float u = static_cast<float>(pos - i);
        const size_t j = std::min(i + 1, frames - 1);
        for (int ch = 0; ch < 2; ++ch) mix[(first + n) * 2 + ch] += volume * (at(i, ch) + (at(j, ch) - at(i, ch)) * u);
    }
}

// The stereo mix as a 16-bit PCM .wav of `frames` frames (cut or padded with silence).
bool write_wav(const std::string& path, const std::vector<float>& mix, size_t frames) {
    const uint32_t data_bytes = static_cast<uint32_t>(frames * 4);
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                     0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    auto u32 = [&h](int at, uint32_t v) {
        for (int k = 0; k < 4; ++k) h[at + k] = static_cast<uint8_t>(v >> (8 * k));
    };
    u32(4, 36 + data_bytes);
    u32(24, kMixRate);
    u32(28, kMixRate * 4);
    u32(40, data_bytes);
    std::vector<uint8_t> b(frames * 4);
    for (size_t i = 0; i < frames * 2; ++i) {
        const float v = i < mix.size() ? std::clamp(mix[i], -1.0f, 1.0f) : 0.0f;
        const uint16_t s = static_cast<uint16_t>(static_cast<int16_t>(std::lround(v * 32767)));
        b[i * 2] = static_cast<uint8_t>(s);
        b[i * 2 + 1] = static_cast<uint8_t>(s >> 8);
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(h), sizeof h);
    if (f) f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

// The average of each `k` by `k` block: a picture drawn `k` times larger, brought back to size.
anim_capture::Image downsample(const anim_capture::Image& big, int k) {
    if (k <= 1) return big;
    anim_capture::Image img(big.w / k, big.h / k);
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            int sum[3] = {0, 0, 0};
            for (int dy = 0; dy < k; ++dy)
                for (int dx = 0; dx < k; ++dx) {
                    const uint8_t* p = big.at(x * k + dx, y * k + dy);
                    for (int c = 0; c < 3; ++c) sum[c] += p[c];
                }
            const int n = k * k;
            img.set(x, y, static_cast<uint8_t>((sum[0] + n / 2) / n), static_cast<uint8_t>((sum[1] + n / 2) / n),
                    static_cast<uint8_t>((sum[2] + n / 2) / n));
        }
    return img;
}

void text_with_shadow(anim_capture::Image& img, int x, int y, const std::string& s, int scale) {
    const int off = std::max(1, scale / 2);
    anim_capture::draw_text(img, x + off, y + off, s, scale, 16, 18, 24);
    anim_capture::draw_text(img, x, y, s, scale, 240, 240, 240);
}

struct Frame {
    std::vector<Affine> world;
    Mesh sparks, rings;
    bool has_points = false;
    Sphere ball;
};

}  // namespace

int run_video_command(const VideoCommand& cmd, std::FILE* out, std::FILE* err) {
    auto fail = [&](const std::string& why) {
        std::fprintf(err, "kyubu-animator --video: %s\n", why.c_str());
        return 1;
    };
    if (cmd.out_path.empty() && cmd.frames_dir.empty()) return fail("give --out <file.mp4> or --frames <folder>");
    if (cmd.width < 16 || cmd.height < 16 || cmd.width % 2 || cmd.height % 2) return fail("--size takes even numbers, 16 or more");
    if (cmd.fps < 1 || cmd.fps > 120) return fail("--fps takes 1 to 120");
    const int ss = std::clamp(cmd.supersample, 1, 4);

    SheetScene scene;
    std::string why;
    if (!load_sheet_scene(cmd.scene, scene, out, why)) return fail(why);
    const Rig& rig = scene.rig.rig;
    const anim::Clip& clip = scene.clip.clip;
    const float length = anim::resolve_duration(clip, cmd.scene.ability_ms);
    if (!(length > 0)) return fail("the clip has no length: nothing to draw");

    // Every frame's matrices and effects first, so the camera can frame them all.
    ParticlePlan plan;
    if (!rig.emitters.empty())
        plan = plan_particles(rig, clip, length, [&](float ms) { return base_pose_for(scene.library, clip, ms).pose; });
    EffectPlan effect_plan;
    if (rig.has_effects())
        effect_plan = plan_effects(rig, clip, length, [&](float ms) { return base_pose_for(scene.library, clip, ms).pose; });
    const int count = static_cast<int>(std::floor(length * cmd.fps / 1000.0f + 1e-3f)) + 1;
    std::vector<Frame> frames(count);
    std::vector<double> times(count);
    std::vector<Vec3> all;
    for (int i = 0; i < count; ++i) {
        const float ms = std::min(length, i * 1000.0f / cmd.fps);
        times[i] = ms;
        const BaseResult base = base_pose_for(scene.library, clip, ms);
        anim::Timing at;
        at.elapsed_ms = ms;
        at.duration_ms = length;
        at.run_ms = length;
        Solved solved = solve(rig, base.pose, clip, at);
        Frame& f = frames[i];
        std::vector<Vec3> pts;
        auto keep = [&](Vec3 p) {
            if (kanimator::length(p) < kFarPoint) pts.push_back(p);
        };
        for (size_t n = 0; n < scene.meshes.size() && n < solved.world.size(); ++n) {
            Vec3 lo, hi;
            if (!mesh_bounds(scene.meshes[n], lo, hi)) continue;
            for (int c = 0; c < 8; ++c) {
                const float p[3] = {c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z};
                float q[3];
                solved.world[n].apply(p, q);
                keep({q[0], q[1], q[2]});
            }
        }
        f.world = std::move(solved.world);
        const std::vector<LiveParticle> live = live_particles(rig, plan, ms, false);
        for (const LiveParticle& p : live) keep({p.pos[0], p.pos[1], p.pos[2]});
        f.sparks = particle_mesh(live);
        const LiveEffects fx = live_effects(rig, effect_plan, ms, false);
        const Mesh solid = pillar_mesh(rig, fx.pillars);
        f.sparks.tris.insert(f.sparks.tris.end(), solid.tris.begin(), solid.tris.end());
        f.rings = ring_mesh(rig, fx.rings);
        for (const Mesh* m : {&solid, static_cast<const Mesh*>(&f.rings)}) {
            Vec3 lo, hi;
            if (!mesh_bounds(*m, lo, hi)) continue;
            keep(lo);
            keep(hi);
        }
        if (!pts.empty()) {
            f.has_points = true;
            f.ball = bounding_sphere(pts.data(), pts.size());
            if (std::getenv("KYUBU_VIDEO_DEBUG")) std::fprintf(out, "%d %.0f %.0f %.0f r %.0f\n", i, f.ball.centre.x, f.ball.centre.y, f.ball.centre.z, f.ball.radius);
            bool framed = cmd.frame_spans.empty();
            for (const auto& s : cmd.frame_spans) framed = framed || (ms >= s.first && ms <= s.second);
            if (framed) all.insert(all.end(), pts.begin(), pts.end());
        }
    }

    // The camera of each frame.
    Camera base_cam;
    base_cam.yaw_deg = cmd.yaw_deg;
    base_cam.pitch_deg = std::clamp(cmd.pitch_deg, -kMaxPitchDeg, kMaxPitchDeg);
    std::vector<Vec3> targets(count);
    float grid_half = 64;
    if (!cmd.follow) {
        const Sphere ball = bounding_sphere(all.data(), all.size());
        base_cam.frame(ball.centre, ball.radius);
        std::fill(targets.begin(), targets.end(), ball.centre);
        grid_half = std::max(std::fabs(ball.centre.x), std::fabs(ball.centre.y)) + ball.radius * 1.3f;
    } else {
        // Each frame's centre (a frame with nothing to frame takes its neighbour's), smoothed over half a second either way so the
        // camera glides; one distance for the whole clip, from the median figure (a burst of effects does not shrink the dragon).
        std::vector<Vec3> centre(count);
        std::vector<float> radii;
        int last = -1;
        for (int i = 0; i < count; ++i) {
            if (frames[i].has_points) {
                centre[i] = frames[i].ball.centre;
                radii.push_back(frames[i].ball.radius);
                if (last < 0)
                    for (int j = 0; j < i; ++j) centre[j] = centre[i];
                last = i;
            } else if (last >= 0) {
                centre[i] = centre[last];
            }
        }
        const float sigma = cmd.fps * 0.25f;
        const int reach = static_cast<int>(std::ceil(sigma * 2));
        float radius = 1;
        if (!radii.empty()) {
            std::nth_element(radii.begin(), radii.begin() + radii.size() / 2, radii.end());
            radius = std::max(1.0f, radii[radii.size() / 2]);
        }
        for (int i = 0; i < count; ++i) {
            Vec3 sum{};
            float wsum = 0;
            for (int j = std::max(0, i - reach); j <= std::min(count - 1, i + reach); ++j) {
                const float w = std::exp(-0.5f * (j - i) * (j - i) / (sigma * sigma));
                sum = sum + centre[j] * w;
                wsum += w;
            }
            targets[i] = sum * (1.0f / wsum);
            grid_half = std::max(grid_half, std::max(std::fabs(targets[i].x), std::fabs(targets[i].y)) + radius * 1.3f);
        }
        base_cam.frame(targets[0], radius);
    }
    base_cam.distance = std::clamp(base_cam.distance / std::max(cmd.zoom, 0.05f), kMinDistance, kMaxDistance);
    const float grid_step = grid_half > 160 ? 16.0f : 8.0f;
    grid_half = std::ceil(grid_half / grid_step) * grid_step;

    // The sound track: each of the rig's `[[sound]]`s at its markers, as the window plays them (one lap, no loop), looked up in the
    // clip's mod's assets/sounds and then the Sounds panel's folder. A sound that runs past the clip holds the last frame longer.
    int hold = static_cast<int>(std::lround(cmd.hold_ms * cmd.fps / 1000.0f));
    std::string wav_path;
    if (!cmd.out_path.empty() && cmd.sound) {
        std::string panel_folder, settings;
        const std::string sp = settings_file_path();
        if (!sp.empty() && read_all_text(sp, settings)) panel_folder = parse_settings(settings).sound_folder;
        const std::vector<std::string> folders = sound_folders(cmd.scene.clip_path, panel_folder);
        SoundCache cache;
        std::vector<float> mix;
        for (const SoundCue& cue : plan_sounds(rig, clip, length, false)) {
            const RigSound& rs = rig.sounds[cue.sound];
            const std::string path = find_sound(rs.file, folders);
            if (path.empty()) {
                std::fprintf(out, "note: sound %s not found: not heard\n", rs.file.c_str());
                continue;
            }
            const std::shared_ptr<const LoadedSound> s = cache.get(path);
            if (!s->info.ok) {
                std::fprintf(out, "note: sound %s: %s\n", rs.file.c_str(), s->info.error.c_str());
                continue;
            }
            mix_sound(mix, *s, cue.t_ms, rs.volume, std::clamp(rs.pitch, 0.25f, 4.0f));
            std::fprintf(out, "sound %s at %.0f ms\n", rs.file.c_str(), cue.t_ms);
        }
        const size_t sound_frames = mix.size() / 2;
        const int needed = static_cast<int>(std::ceil(sound_frames * static_cast<double>(cmd.fps) / kMixRate));
        if (needed > count + hold) hold = needed - count;
        const size_t total = static_cast<size_t>(std::llround((count + hold) * static_cast<double>(kMixRate) / cmd.fps));
        wav_path = cmd.out_path + ".sound.wav";
        for (float& v : mix) v *= cmd.volume;
        if (!write_wav(wav_path, mix, total)) return fail("cannot write " + wav_path);
    }
    struct RemoveFile {
        std::string path;
        ~RemoveFile() {
            if (!path.empty()) std::remove(path.c_str());
        }
    } remove_wav{wav_path};

    Encoder enc;
    if (!cmd.out_path.empty() && !enc.start(cmd.ffmpeg, cmd.out_path, cmd.width, cmd.height, cmd.fps, wav_path, why)) return fail(why);
    const std::string title = cmd.title.empty() ? scene.clip_name : cmd.title;
    const int scale = std::max(2, cmd.height / 180);
    const int margin = scale * 4;
    anim_capture::Image img;
    for (int i = 0; i < count; ++i) {
        Camera cam = base_cam;
        cam.target = targets[i];
        img = downsample(render_figure_wh(scene.meshes, frames[i].world, cam, cmd.width * ss, cmd.height * ss, grid_half,
                                          grid_step, &frames[i].sparks, &frames[i].rings, ss),
                         ss);
        text_with_shadow(img, margin, margin, title, scale);
        if (cmd.clock) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f s", times[i] / 1000.0);
            const int s2 = std::max(2, scale * 2 / 3);
            text_with_shadow(img, img.w - margin - anim_capture::text_width(buf, s2), margin, buf, s2);
        }
        const int repeats = i + 1 == count ? 1 + hold : 1;
        for (int r = 0; r < repeats; ++r)
            if (!cmd.out_path.empty() && !enc.write(img)) {
                enc.finish();
                return fail("ffmpeg stopped taking frames");
            }
        if (!cmd.frames_dir.empty()) {
            char name[32];
            std::snprintf(name, sizeof name, "/frame_%05d.png", i);
            const std::vector<uint8_t> png = anim_capture::encode_png(img);
            std::ofstream f(cmd.frames_dir + name, std::ios::binary | std::ios::trunc);
            if (f) f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
            if (!f) return fail("cannot write " + cmd.frames_dir + name);
        }
    }
    if (!cmd.out_path.empty()) {
        const int code = enc.finish();
        if (code != 0) return fail("ffmpeg failed (exit code " + std::to_string(code) + ")");
    }
    std::fprintf(out, "wrote %s: %d frames + %d held, %dx%d at %d fps, %.2f s, rig %s\n",
                 cmd.out_path.empty() ? cmd.frames_dir.c_str() : cmd.out_path.c_str(), count, hold, cmd.width, cmd.height, cmd.fps,
                 (count + hold) / static_cast<double>(cmd.fps), rig.name.c_str());
    return 0;
}

}  // namespace kanimator
