// anim_capture: the game side of the Animator (openspec add-ability-animation, section 7; design D7). A dev mod with full trust and no
// window. It draws nothing itself: the game draws, and the mod supplies the clip and the clock through the editor's calls (3.5).
// Task 7.1 gives `anim preview <clip> <t_ms>` and `anim preview off`; task 7.3 gives `anim capture <clip> [times_ms] [views]`; task 7.4
// gives `anim rig` and `anim bake <pose id> [duration_ms]`.
//
// The loader's `anim` builtin hands a subcommand it does not know to the console command `<mod id>:anim.<subcommand>`, so this mod
// registers `anim_capture:anim.preview` and `anim_capture:anim.capture` and the author types `anim preview ...`.
//
// Capture (D11). A job walks the steps (a time, a view) one after the other. A step sets the preview clock and the player's yaw, waits
// kSettleMs of real time and a draw (the game eases its pose vectors), then, inside `render.overlay`, which runs before the overlay's
// own draw, reads the back buffer, crops the character and keeps the vectors and matrices of the player's last draw. When the last step is
// done the preview and the yaw are given back and `sheet.png` and `pose.json` are written to <game>/kyubu/capture/<clip>-<stamp>/.
// The vectors are read by a mid hook at the join site (so after the clip's tracks were written there: the loader's detour puts the game's
// values back when the draw returns), the matrices by a function hook on draw_creature after the original returns (7.2).
//
// Rig (D12). The same job with one step (the front view, no clip): the pose.json frame also holds `models`, the model table index of each
// slot of the draw record, and `appearance`. The index is the loader slot; the private `catalog.csv` of the pose library names its file.
//
// Bake (D12). The pose is a vanilla pose id, played by a generated clip with a `[borrow]` table on the preview clock: this mod advances
// the clock by real time (a vanilla mode would need an ability and a forced mode, and 65 and 83 stop the game; the clock also makes the
// run repeatable). Every draw of the local player is a frame of pose.json. Then the draws are thinned to 30 a second, written as a clip of
// `set` tracks and played back the same way; the replay's vectors are compared with the bake's, and both are in the sheet (top row the
// game's pose, bottom row the replay). This is the check that the baked vectors reproduce the pose.
//
// File inbox (task 7.6, decision 7). A request file `<game>/kyubu/inbox/<name>.txt` holds one console line; it must be one of this mod's
// own `anim` words (inbox.h). The poll runs in the render.overlay handler, every 500 ms and only while no job runs; the file is removed
// before it is run and the reply is written beside it as `<name>.reply`. The Animator's `In game` button writes such a file.
//
// Without dev mode the mod does not load: kyubu_mod_init returns 1 and the loader marks it failed with that reason. The macro
// KYUBU_MOD always returns 0, so the three entry points are written out.
#include <windows.h>
#include <d3d9.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "capture_args.h"
#include "capture_sheet.h"
#include "inbox.h"
#include "kyubu.hpp"
#include "preview_args.h"

namespace {

using namespace kyubu::anim_capture;

kyubu::Mod* g_mod = nullptr;

// cube::Creature (sdk/include/cubeworld/types.h): id at 0x8, data at 0x10 with body_yaw at 0x20 in it. The draw record is at +0x1498.
constexpr uintptr_t kCreatureId = 0x8;
constexpr uintptr_t kCreatureYaw = 0x10 + 0x20;
constexpr uintptr_t kRecordOffset = 0x1498;
constexpr const char* kBuild = "cube-0x51EA955E";
constexpr uint64_t kSettleMs = 1500;     // real time a pose is held before its picture: the game eases the vectors
constexpr uint64_t kStartWaitMs = 10000; // the player's first draw after the command

void say(KyubuPrintFn print, void* ctx, const std::string& text) {
    print(ctx, text.c_str());
    g_mod->info("%s", text.c_str());   // the log keeps it too: replays read the log
}

// The line that ended the last job (task 7.6): the log has it, and an inbox reply gets it as its last line.
std::string g_last_outcome;

void outcome(bool ok, const std::string& text) {
    g_last_outcome = text;
    if (ok) g_mod->info("%s", text.c_str());
    else g_mod->warn("%s", text.c_str());
}

void inbox_poll();   // after the commands

// ---- the job
struct Step {
    size_t time;   // index into the times
    int view;      // index of kViews
};
enum class Kind { Capture, Rig };
int64_t g_local_id = 0;

struct Job {
    Kind kind = Kind::Capture;
    bool active = false;
    bool started = false;            // the first draw of the player was seen and the first step applied
    CaptureArgs args;
    std::vector<Step> steps;
    size_t at = 0;                   // the step being held
    uint64_t begun_ms = 0, settle_until_ms = 0, draw_mark = 0;
    uintptr_t creature = 0;
    float saved_yaw = 0;
    std::vector<SheetCell> cells;
    std::vector<FrameData> frames;
} g_job;

struct Snapshot {
    uint64_t draws = 0;              // the local player's draws since the job began
    FrameData data;
    bool have_vectors = false;       // the mid hook ran in this draw
    uintptr_t creature = 0;
} g_snap;

// ---- the bake
constexpr int kBakePics = 4;              // pictures per phase, at 0, 1/4, 2/4 and 3/4 of its duration
constexpr double kKeyGapMs = 1000.0 / 30; // the keys of the replay clip: 30 a second
constexpr double kPosLimit = 0.25;        // voxels; the mimic gate's tolerance (task 1.3), also the bake's replay check
constexpr double kRotLimit = 1.0;         // degrees
constexpr int kTailSlot = 0xD4;           // the record's tail model slot: empty for a creature with no tail

struct Bake {
    bool active = false, started = false;
    int phase = 0;                        // 0 the game's pose (the borrow clip), 1 the baked vectors played back (the set clip)
    BakeArgs args;
    std::string clip[2];                  // the ids
    std::string set_text;
    double duration_ms[2] = {0, 0};
    std::chrono::steady_clock::time_point phase_begin;
    uint64_t begun_ms = 0, draw_mark = 0;
    double set_ms = 0;                    // the preview time that the next draw uses
    int pics = 0;
    bool tail_lacking = false;
    uintptr_t creature = 0;
    std::vector<FrameData> frames[2];
    std::vector<SheetCell> cells[2];
} g_bake;

bool g_hooks_installed = false;
std::string g_hooks_why;
KyubuId g_join_id = 0, g_draw_id = 0;

bool is_local_record(const void* rec, uintptr_t* creature_out) {
    uint32_t creature = 0;
    std::memcpy(&creature, rec, 4);
    if (!creature || !(g_job.active || g_bake.active)) return false;
    int64_t id = 0;
    std::memcpy(&id, reinterpret_cast<const void*>(uintptr_t(creature) + kCreatureId), 8);
    if (id != g_local_id) return false;
    *creature_out = creature;
    return true;
}

void KYUBU_CALL on_join_site(KyubuCpuContext* ctx, void*) {
    uintptr_t creature = 0;
    const void* rec = reinterpret_cast<const void*>(uintptr_t(ctx->edi));
    if (!is_local_record(rec, &creature)) return;
    read_vectors(rec, &g_snap.data);
    g_snap.have_vectors = true;
}

// void __thiscall draw_creature(record, ten stack arguments), `ret 0x28`: __fastcall with an unused EDX has the thiscall ABI.
using DrawFn = void(__fastcall*)(void*, void*, void*, void*, void*, void*, uint32_t, void*, void*, void*, uint32_t, void*);
DrawFn g_draw_next = nullptr;

// The model table (ModelLoader at GameController + 0x2F8): false when the controller or the vector cannot be read.
bool load_model_table(std::vector<uint32_t>* out) {
    const uintptr_t global = g_mod->symbol("cube::game_controller");
    uint32_t controller = 0, begin = 0, end = 0;
    if (!global || !g_mod->read(global, controller) || !controller) return false;
    if (!g_mod->read(controller + 0x300 + 4, begin) || !g_mod->read(controller + 0x300 + 8, end)) return false;
    if (!begin || end <= begin || (end - begin) / 4 > 100000) return false;
    out->resize((end - begin) / 4);
    return g_mod->api()->mem_read(begin, out->data(), out->size() * 4) == KY_OK;
}

void __fastcall on_draw(void* rec, void* edx, void* a1, void* a2, void* a3, void* a4, uint32_t a5, void* a6, void* a7, void* a8,
                        uint32_t a9, void* a10) {
    g_draw_next(rec, edx, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    uintptr_t creature = 0;
    if (!is_local_record(rec, &creature) || !g_snap.have_vectors) return;
    read_matrices(rec, &g_snap.data);
    if (g_job.active && g_job.kind == Kind::Rig) {
        std::vector<uint32_t> table;
        if (load_model_table(&table)) read_model_slots(rec, table.data(), table.size(), &g_snap.data);
        uint8_t appearance[2 * kAppearanceCount];
        if (g_mod->api()->mem_read(creature + kAppearance[0].offset, appearance, sizeof appearance) == KY_OK)
            read_appearance(appearance, &g_snap.data);
    }
    if (g_bake.active && g_bake.started) {
        FrameData f = g_snap.data;
        f.index = int(g_bake.frames[g_bake.phase].size());
        f.t_ms = g_bake.set_ms;
        // The record's own +4 and +8 read 0 at the join site from the second draw of a borrowed pose on (the vectors still follow the pose),
        // so the frame names what the mod fed: the pose id, and the preview time as the pose's own time. The replay draws no pose id.
        f.pose_id = g_bake.phase == 0 ? uint32_t(g_bake.args.pose) : 0;
        f.mode_time_ms = uint32_t(std::lround(g_bake.set_ms));
        f.view = "live";
        std::memcpy(&f.body_yaw, reinterpret_cast<const void*>(creature + kCreatureYaw), 4);
        uint32_t tail = 0;
        std::memcpy(&tail, static_cast<const uint8_t*>(rec) + kTailSlot, 4);
        g_bake.tail_lacking = tail == 0;
        g_bake.frames[g_bake.phase].push_back(f);
    }
    g_snap.creature = creature;
    ++g_snap.draws;
    g_snap.have_vectors = false;
}

bool install_hooks() {
    if (g_hooks_installed) return true;
    if (!g_hooks_why.empty()) return false;
    KyubuHostInfo host;
    std::memset(&host, 0, sizeof host);
    host.size = sizeof host;
    if (g_mod->api()->host_info(&host) != KY_OK || std::strcmp(host.build_id, kBuild) != 0) {
        g_hooks_why = std::string("the draw record is mapped for ") + kBuild + " only";
        return false;
    }
    const uintptr_t draw = g_mod->symbol("draw_creature"), join = g_mod->symbol("draw_creature_join_site");
    if (!draw || !join) {
        g_hooks_why = "the symbols draw_creature and draw_creature_join_site are not known to this loader";
        return false;
    }
    if (g_mod->api()->hook_mid(g_mod->handle(), join, on_join_site, nullptr, &g_join_id) != KY_OK) {
        g_hooks_why = "the join site could not be hooked (see the log)";
        return false;
    }
    g_draw_id = g_mod->hook(draw, on_draw, &g_draw_next);
    if (!g_draw_id) {
        g_mod->unhook(g_join_id);
        g_hooks_why = "draw_creature could not be hooked (see the log)";
        return false;
    }
    g_hooks_installed = true;
    return true;
}

// ---- files
bool make_dirs(const std::string& path) {
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '\\' && path[i] != '/') continue;
        if (i == 0 || path[i - 1] == ':') continue;
        const std::string part = path.substr(0, i);
        if (!CreateDirectoryA(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return CreateDirectoryA(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string safe_name(const std::string& clip) {
    std::string s = clip;
    for (char& c : s)
        if (c == ':' || c == '/' || c == '\\') c = '_';
    return s;
}

std::string capture_dir(const std::string& clip) {
    char game[MAX_PATH] = {};
    g_mod->api()->game_dir(game, sizeof game);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return std::string(game) + "\\kyubu\\capture\\" + safe_name(clip) + "-" + stamp;
}

bool write_file(const std::string& path, const void* data, size_t len) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), std::streamsize(len));
    return bool(f);
}

// ---- the back buffer, before the overlay draws
bool read_back_buffer(IDirect3DDevice9* dev, std::vector<uint8_t>* px, int* w, int* h, std::string* why) {
    IDirect3DSurface9* bb = nullptr;
    IDirect3DSurface9* sys = nullptr;
    bool ok = false;
    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        *why = "GetBackBuffer failed";
    } else {
        D3DSURFACE_DESC d;
        bb->GetDesc(&d);
        if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) {
            *why = "the back buffer's format is " + std::to_string(int(d.Format)) + ", not a 32 bit one";
        } else if (FAILED(hr = dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys) {
            *why = "CreateOffscreenPlainSurface failed (hr " + std::to_string(long(hr)) + ")";
        } else if (FAILED(hr = dev->GetRenderTargetData(bb, sys))) {
            *why = "GetRenderTargetData failed (hr " + std::to_string(long(hr)) + ")";
        } else {
            D3DLOCKED_RECT lr;
            if (FAILED(hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                *why = "LockRect failed (hr " + std::to_string(long(hr)) + ")";
            } else {
                *w = int(d.Width);
                *h = int(d.Height);
                px->resize(size_t(*w) * *h * 4);
                for (int y = 0; y < *h; ++y)
                    std::memcpy(px->data() + size_t(y) * *w * 4, static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, size_t(*w) * 4);
                sys->UnlockRect();
                ok = true;
            }
        }
    }
    if (sys) sys->Release();
    if (bb) bb->Release();
    return ok;
}

// ---- the steps
void restore_player() {
    g_mod->animation_preview_clear();
    if (g_job.started && g_job.creature) g_mod->write<float>(g_job.creature + kCreatureYaw, g_job.saved_yaw);
}

void fail(const std::string& why) {
    restore_player();
    outcome(false, std::string(g_job.kind == Kind::Rig ? "anim rig" : "anim capture") + ": failed: " + why);
    g_job.active = false;
}

void finish() {
    restore_player();
    g_job.active = false;
    const bool rig = g_job.kind == Kind::Rig;
    const char* what = rig ? "anim rig" : "anim capture";
    const std::string dir = capture_dir(rig ? "rig" : g_job.args.clip);
    if (!make_dirs(dir)) {
        outcome(false, std::string(what) + ": failed: could not create " + dir);
        return;
    }
    const int cols = int(g_job.args.views.size());
    const Image sheet = make_sheet(rig ? "rig (the local player at rest)" : "capture " + g_job.args.clip, g_job.cells, cols);
    const std::vector<uint8_t> png = encode_png(sheet);
    const std::string json = pose_json(rig ? "rig" : "capture", rig ? "" : g_job.args.clip, kBuild, g_job.frames);
    if (png.empty() || !write_file(dir + "\\sheet.png", png.data(), png.size()) || !write_file(dir + "\\pose.json", json.data(), json.size())) {
        outcome(false, std::string(what) + ": failed: could not write the files under " + dir);
        return;
    }
    if (rig && !g_job.frames.empty() && !g_job.frames[0].has_models)
        g_mod->warn("anim rig: the model table could not be read: pose.json has no `models`");
    outcome(true, std::string(what) + ": done: " + dir + " (" + std::to_string(g_job.frames.size()) + " frames; sheet.png, pose.json)");
}

// Poses the player for step `at`: the yaw of its view and the preview clock at its time.
bool apply_step() {
    const Step& s = g_job.steps[g_job.at];
    const CaptureView& v = kViews[s.view];
    g_mod->write<float>(g_job.creature + kCreatureYaw, v.body_yaw);
    const double t = g_job.args.times_ms[s.time];
    const KyubuResult r = g_job.kind == Kind::Rig ? KY_OK : g_mod->animation_preview(g_job.args.clip.c_str(), t);
    if (r != KY_OK) {
        PreviewArgs p;
        p.kind = PreviewArgs::Kind::Show;
        p.clip = g_job.args.clip;
        p.t_ms = t;
        fail(preview_reply(p, int(r)));
        return false;
    }
    g_job.draw_mark = g_snap.draws;
    g_job.settle_until_ms = GetTickCount64() + kSettleMs;
    return true;
}

// ---- the bake's phases
void bake_restore() { g_mod->animation_preview_clear(); }

void bake_fail(const std::string& why) {
    bake_restore();
    outcome(false, "anim bake: failed: " + why);
    g_bake.active = false;
}

bool bake_begin_phase(int phase) {
    Bake& b = g_bake;
    b.phase = phase;
    b.set_ms = 0;
    b.pics = 0;
    b.draw_mark = g_snap.draws;
    b.phase_begin = std::chrono::steady_clock::now();
    const KyubuResult r = g_mod->animation_preview(b.clip[phase].c_str(), 0);
    if (r != KY_OK) {
        bake_fail("the preview of " + b.clip[phase] + " was refused (result " + std::to_string(int(r)) + ")");
        return false;
    }
    return true;
}

std::string bake_summary(const VectorDiff& d, bool tail_skipped, bool* ok) {
    *ok = d.frames > 0 && d.max_pos <= kPosLimit && d.max_rot <= kRotLimit;
    char b[320];
    std::snprintf(b, sizeof b, "largest difference %.4f voxels (%s) and %.4f degrees (%s) over %d draws, limits %.2f and %.1f%s: %s", d.max_pos,
                  d.pos_part >= 0 ? kPartVecs[d.pos_part].name : "-", d.max_rot, d.rot_part >= 0 ? kPartVecs[d.rot_part].name : "-", d.frames,
                  kPosLimit, kRotLimit, tail_skipped ? ", the tail left out (this creature has no tail model)" : "", *ok ? "ok" : "DIFFERS");
    return b;
}

void bake_finish() {
    Bake& b = g_bake;
    bake_restore();
    b.active = false;
    VectorDiff d;
    for (const FrameData& f : b.frames[1]) {
        FrameData ref;
        interp_vectors(b.frames[0], f.t_ms, &ref);
        FrameData a = f, c = ref;
        if (b.tail_lacking) {
            std::memset(a.rot[5], 0, sizeof a.rot[5]);
            std::memset(c.rot[5], 0, sizeof c.rot[5]);
        }
        accumulate_diff(a, c, &d);
    }
    bool ok = false;
    const std::string summary = bake_summary(d, b.tail_lacking, &ok);
    const std::string dir = capture_dir("bake_pose" + std::to_string(b.args.pose));
    if (!make_dirs(dir)) {
        outcome(false, "anim bake: failed: could not create " + dir);
        return;
    }
    // top row: the game's pose; bottom row: the replay. A phase that took fewer pictures is padded with its last one.
    std::vector<SheetCell> cells = b.cells[0];
    const size_t cols = cells.size();
    for (size_t i = 0; i < cols; ++i) {
        if (b.cells[1].empty()) break;
        cells.push_back(b.cells[1][i < b.cells[1].size() ? i : b.cells[1].size() - 1]);
    }
    // the sheet's font has no brackets or commas
    const Image sheet = make_sheet("bake pose " + std::to_string(b.args.pose) + ": top row the game, bottom row the baked vectors played back",
                                   cells, int(cols ? cols : 1));
    const std::vector<uint8_t> png = encode_png(sheet);
    const std::string pose = pose_json("bake", b.clip[0], kBuild, b.frames[0]);
    const std::string replay = pose_json("bake-replay", b.clip[1], kBuild, b.frames[1]);
    if (png.empty() || !write_file(dir + "\\sheet.png", png.data(), png.size()) || !write_file(dir + "\\pose.json", pose.data(), pose.size()) ||
        !write_file(dir + "\\replay.json", replay.data(), replay.size()) || !write_file(dir + "\\bake.anim.toml", b.set_text.data(), b.set_text.size())) {
        outcome(false, "anim bake: failed: could not write the files under " + dir);
        return;
    }
    char head[160];
    std::snprintf(head, sizeof head, "anim bake: pose %d done: %d draws in %g ms, the replay of the baked vectors %d draws: ", b.args.pose,
                  int(b.frames[0].size()), b.duration_ms[0], int(b.frames[1].size()));
    outcome(true, std::string(head) + summary + "; " + dir);
}

// Called when a phase's clock has run out.
void bake_end_phase() {
    Bake& b = g_bake;
    if (b.phase == 1) {
        bake_finish();
        return;
    }
    if (b.frames[0].size() < 2) {
        bake_fail("the local player was drawn " + std::to_string(b.frames[0].size()) + " times: nothing to bake");
        return;
    }
    const std::vector<size_t> kept = thin_frames(b.frames[0], kKeyGapMs);
    double duration = 0;
    b.set_text = bake_clip_text(b.frames[0], kept, &duration, !b.tail_lacking);
    if (b.set_text.empty()) {
        bake_fail("the baked draws have no time span: nothing to replay");
        return;
    }
    char err[512] = {};
    const std::string name = "bake_pose" + std::to_string(b.args.pose) + "_set";
    const KyubuResult r = g_mod->animation_define_from_text(name.c_str(), b.set_text.c_str(), err, sizeof err);
    if (r != KY_OK) {
        bake_fail("the clip of the baked vectors was refused: " + std::string(err[0] ? err : "no reason given") + " (result " +
                  std::to_string(int(r)) + ")");
        return;
    }
    b.clip[1] = "anim_capture:" + name;
    b.duration_ms[1] = duration;
    g_mod->info("anim bake: pose %d: %d draws, %d keys per track; playing them back as %s", b.args.pose, int(b.frames[0].size()), int(kept.size()),
                b.clip[1].c_str());
    bake_begin_phase(1);
}

void bake_overlay(KyubuEvent& ev) {
    Bake& b = g_bake;
    const uint64_t now = GetTickCount64();
    if (!b.started) {
        if (g_snap.draws == 0) {
            if (now - b.begun_ms > kStartWaitMs) bake_fail("the local player was not drawn (is a world loaded?)");
            return;
        }
        b.creature = g_snap.creature;
        b.started = true;
        bake_begin_phase(0);
        return;
    }
    // A picture of the draw that just ran, which used `set_ms`.
    if (g_snap.draws > b.draw_mark) {
        b.draw_mark = g_snap.draws;
        if (b.pics < kBakePics && b.set_ms >= b.duration_ms[b.phase] * b.pics / kBakePics) {
            auto* rd = kyubu::Mod::data<KyubuRenderData>(ev);
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            std::string why;
            if (!rd || !rd->device || !read_back_buffer(static_cast<IDirect3DDevice9*>(rd->device), &px, &w, &h, &why)) {
                bake_fail(why.empty() ? "no render device" : why);
                return;
            }
            b.cells[b.phase].push_back({crop_bgra(px.data(), w * 4, w, h, crop_box(w, h)), frame_label(std::round(b.set_ms), b.phase ? "replay" : "game")});
            ++b.pics;
        }
    }
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - b.phase_begin).count();
    if (elapsed >= b.duration_ms[b.phase]) {
        bake_end_phase();
        return;
    }
    if (g_mod->animation_preview(b.clip[b.phase].c_str(), elapsed) != KY_OK) {
        bake_fail("the preview of " + b.clip[b.phase] + " went away");
        return;
    }
    b.set_ms = elapsed;
}

void on_overlay(KyubuEvent& ev) {
    inbox_poll();
    if (g_bake.active) {
        bake_overlay(ev);
        return;
    }
    if (!g_job.active) return;
    const uint64_t now = GetTickCount64();
    if (!g_job.started) {
        if (g_snap.draws == 0) {
            if (now - g_job.begun_ms > kStartWaitMs) fail("the local player was not drawn (is a world loaded?)");
            return;
        }
        g_job.creature = g_snap.creature;
        std::memcpy(&g_job.saved_yaw, reinterpret_cast<const void*>(g_job.creature + kCreatureYaw), 4);
        g_job.started = true;
        g_job.at = 0;
        apply_step();
        return;
    }
    if (now < g_job.settle_until_ms || g_snap.draws <= g_job.draw_mark) return;
    auto* rd = kyubu::Mod::data<KyubuRenderData>(ev);
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    std::string why;
    if (!rd || !rd->device || !read_back_buffer(static_cast<IDirect3DDevice9*>(rd->device), &px, &w, &h, &why)) {
        fail(why.empty() ? "no render device" : why);
        return;
    }
    const Step& s = g_job.steps[g_job.at];
    FrameData f = g_snap.data;
    f.index = int(g_job.at);
    f.t_ms = g_job.args.times_ms[s.time];
    f.view = kViews[s.view].name;
    f.body_yaw = kViews[s.view].body_yaw;
    g_job.frames.push_back(f);
    g_job.cells.push_back({crop_bgra(px.data(), w * 4, w, h, crop_box(w, h)), frame_label(f.t_ms, f.view)});
    if (++g_job.at >= g_job.steps.size()) {
        finish();
        return;
    }
    apply_step();
}

// ---- the commands
void KYUBU_CALL cmd_preview(const char* args, KyubuPrintFn print, void* ctx, void*) {
    const PreviewArgs p = parse_preview_args(args ? args : "");
    int result = KY_OK;
    if (p.kind == PreviewArgs::Kind::Show) result = g_mod->animation_preview(p.clip.c_str(), p.t_ms);
    else if (p.kind == PreviewArgs::Kind::Off) result = g_mod->animation_preview_clear();
    say(print, ctx, preview_reply(p, result));
}

void KYUBU_CALL cmd_capture(const char* args, KyubuPrintFn print, void* ctx, void*) {
    const CaptureArgs a = parse_capture_args(args ? args : "");
    if (!a.ok) {
        say(print, ctx, "anim capture: " + a.error);
        return;
    }
    if (g_job.active || g_bake.active) {
        say(print, ctx, "anim capture: a capture, rig or bake is running; its folder is named in the log when it ends");
        return;
    }
    KyubuCreature player = 0;
    KyubuCreatureInfo info;
    std::memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (g_mod->creature_player(player) != KY_OK || g_mod->creature_get(player, info) != KY_OK) {
        say(print, ctx, "anim capture: there is no local player (load a world first)");
        return;
    }
    if (!install_hooks()) {
        say(print, ctx, "anim capture: " + g_hooks_why);
        return;
    }
    // The clip must exist before anything is held: a preview of it, and off again.
    const KyubuResult r = g_mod->animation_preview(a.clip.c_str(), 0);
    if (r != KY_OK) {
        PreviewArgs p;
        p.kind = PreviewArgs::Kind::Show;
        p.clip = a.clip;
        say(print, ctx, "anim capture: " + preview_reply(p, int(r)).substr(std::strlen("anim preview: ")));
        g_mod->animation_preview_clear();
        return;
    }
    g_job = Job();
    g_job.args = a;
    for (size_t t = 0; t < a.times_ms.size(); ++t)
        for (int v : a.views) g_job.steps.push_back({t, v});
    g_local_id = info.id;
    g_job.begun_ms = GetTickCount64();
    g_job.active = true;
    g_snap = Snapshot();
    char b[160];
    std::snprintf(b, sizeof b, "anim capture: %s, %d times, %d views: started (about %d s); the folder is named in the log", a.clip.c_str(),
                  int(a.times_ms.size()), int(a.views.size()), int(g_job.steps.size() * (kSettleMs + 100) / 1000) + 1);
    say(print, ctx, b);
}

// The local player and the hooks, for `rig` and `bake`; false after printing why not.
bool ready_for_job(const char* what, KyubuPrintFn print, void* ctx, int64_t* id) {
    const std::string name = std::string("anim ") + what + ": ";
    if (g_job.active || g_bake.active) {
        say(print, ctx, name + "a capture, rig or bake is running; its folder is named in the log when it ends");
        return false;
    }
    KyubuCreature player = 0;
    KyubuCreatureInfo info;
    std::memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (g_mod->creature_player(player) != KY_OK || g_mod->creature_get(player, info) != KY_OK) {
        say(print, ctx, name + "there is no local player (load a world first)");
        return false;
    }
    if (!install_hooks()) {
        say(print, ctx, name + g_hooks_why);
        return false;
    }
    *id = info.id;
    return true;
}

void KYUBU_CALL cmd_rig(const char* args, KyubuPrintFn print, void* ctx, void*) {
    const std::string bad = rig_args_error(args ? args : "");
    if (!bad.empty()) {
        say(print, ctx, "anim rig: " + bad);
        return;
    }
    int64_t id = 0;
    if (!ready_for_job("rig", print, ctx, &id)) return;
    g_job = Job();
    g_job.kind = Kind::Rig;
    g_job.args.ok = true;
    g_job.args.times_ms = {0.0};
    g_job.args.views = {0};   // front: the same yaw as a capture, so the matrices compare
    g_job.steps.push_back({0, 0});
    g_local_id = id;
    g_job.begun_ms = GetTickCount64();
    g_job.active = true;
    g_snap = Snapshot();
    say(print, ctx, "anim rig: started (about 2 s); keep the player still; the folder is named in the log");
}

void KYUBU_CALL cmd_bake(const char* args, KyubuPrintFn print, void* ctx, void*) {
    const BakeArgs a = parse_bake_args(args ? args : "");
    if (!a.ok) {
        say(print, ctx, "anim bake: " + a.error);
        return;
    }
    int64_t id = 0;
    if (!ready_for_job("bake", print, ctx, &id)) return;
    char text[160];
    std::snprintf(text, sizeof text, "# played by anim_capture (anim bake): pose %d, nothing else\nduration = %.8g\n\n[borrow]\nmode = %d\n", a.pose,
                  a.duration_ms, a.pose);
    char err[512] = {};
    const std::string name = "bake_pose" + std::to_string(a.pose);
    const KyubuResult r = g_mod->animation_define_from_text(name.c_str(), text, err, sizeof err);
    if (r != KY_OK) {
        say(print, ctx, "anim bake: the clip of pose " + std::to_string(a.pose) + " was refused: " + std::string(err[0] ? err : "no reason given"));
        return;
    }
    g_bake = Bake();
    g_bake.args = a;
    g_bake.clip[0] = "anim_capture:" + name;
    g_bake.duration_ms[0] = a.duration_ms;
    g_bake.begun_ms = GetTickCount64();
    g_bake.active = true;
    g_local_id = id;
    g_snap = Snapshot();
    char b[200];
    std::snprintf(b, sizeof b, "anim bake: pose %d, %g ms: started (about %g s with the replay); stand still; the folder is named in the log", a.pose,
                  a.duration_ms, a.duration_ms * 2 / 1000 + 1);
    say(print, ctx, b);
}

// ---- the file inbox (task 7.6; design decision 7)
// <game>/kyubu/inbox/<name>.txt holds one console line (inbox.h). Every kInboxPollMs, when no job runs, the oldest request by name is read,
// removed (so a crash or a long job cannot run it twice) and run through the command above, with the print function collecting the reply.
// The reply is <name>.reply beside it: the request, what the command answered, and, for a job that takes time, a last line `result: ...`
// with the folder it wrote. The log has the same lines.
constexpr uint64_t kInboxPollMs = 500;
std::string g_inbox_dir;           // empty until kyubu_mod_init has made it
uint64_t g_inbox_next_ms = 0;
std::string g_inbox_waiting;       // the reply file of a request whose job is still running
std::string g_inbox_stuck;         // a request file that could not be removed: it is not run again

void KYUBU_CALL inbox_collect(void* ctx, const char* text) {
    auto* out = static_cast<std::string*>(ctx);
    *out += text ? text : "";
    *out += "\n";
}

bool inbox_oldest(std::string* name) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_inbox_dir + "\\*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        const std::string n = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !is_inbox_request_name(n) || n == g_inbox_stuck) continue;
        if (!found || n < *name) *name = n;
        found = true;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

void inbox_poll() {
    if (g_inbox_dir.empty()) return;
    const uint64_t now = GetTickCount64();
    if (now < g_inbox_next_ms) return;
    g_inbox_next_ms = now + kInboxPollMs;
    const bool busy = g_job.active || g_bake.active;
    if (!g_inbox_waiting.empty() && !busy) {
        std::ofstream(g_inbox_waiting, std::ios::binary | std::ios::app) << "result: " << g_last_outcome << "\n";
        g_inbox_waiting.clear();
    }
    if (busy || !g_inbox_waiting.empty()) return;
    std::string name;
    if (!inbox_oldest(&name)) return;
    const std::string path = g_inbox_dir + "\\" + name;
    std::string text;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return;   // still being written, or locked: the next poll
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (text.empty()) return;   // dropped without the temporary name: wait for its content
    if (!DeleteFileA(path.c_str())) {
        g_mod->warn("anim_capture: inbox: could not remove %s: it is not run", path.c_str());
        g_inbox_stuck = name;
        return;
    }
    const InboxRequest r = parse_inbox_request(text);
    std::string reply = "request: " + (r.line.empty() ? name : r.line) + "\n";
    if (!r.ok) {
        reply += r.error + "\n";
        g_mod->warn("anim_capture: %s: %s", name.c_str(), r.error.c_str());
    } else {
        g_mod->info("anim_capture: inbox %s: %s", name.c_str(), r.line.c_str());
        std::string out;
        const char* args = r.args.c_str();
        if (r.sub == "preview") cmd_preview(args, inbox_collect, &out, nullptr);
        else if (r.sub == "capture") cmd_capture(args, inbox_collect, &out, nullptr);
        else if (r.sub == "rig") cmd_rig(args, inbox_collect, &out, nullptr);
        else cmd_bake(args, inbox_collect, &out, nullptr);
        reply += out;
        if (g_job.active || g_bake.active) g_inbox_waiting = g_inbox_dir + "\\" + inbox_reply_name(name);
    }
    std::ofstream(g_inbox_dir + "\\" + inbox_reply_name(name), std::ios::binary | std::ios::trunc) << reply;
}

}  // namespace

extern "C" KYUBU_EXPORT int KYUBU_CALL kyubu_mod_query(KyubuModQuery* q) {
    q->api_version = KYUBU_API_VERSION;
    std::strncpy(q->id, "anim_capture", sizeof q->id - 1);
    q->sides = KY_SIDE_CLIENT;
    return 0;
}

extern "C" KYUBU_EXPORT int KYUBU_CALL kyubu_mod_init(const KyubuApi* api, KyubuModHandle h, KyubuSide s) {
    KyubuHostInfo host;
    std::memset(&host, 0, sizeof host);
    host.size = sizeof host;
    if (!api->host_info || api->host_info(&host) != KY_OK || !host.dev_mode) return 1;   // dev mode only
    if (s != KY_SIDE_CLIENT) return 0;
    g_mod = new kyubu::Mod(api, h, s);
    if (api->register_command(h, "anim_capture:anim.preview", "anim preview <clip> <t_ms> | anim preview off: hold a clip's pose on the local player, no ability", cmd_preview, nullptr) != KY_OK)
        g_mod->warn("anim_capture: could not register the preview command");
    if (api->register_command(h, "anim_capture:anim.capture", "anim capture <clip> [times_ms] [views]: a sheet and pose.json of the clip on the local player, under kyubu/capture/", cmd_capture, nullptr) != KY_OK)
        g_mod->warn("anim_capture: could not register the capture command");
    if (api->register_command(h, "anim_capture:anim.rig", "anim rig: the local player at rest: vectors, part matrices and the model of each slot, under kyubu/capture/", cmd_rig, nullptr) != KY_OK)
        g_mod->warn("anim_capture: could not register the rig command");
    if (api->register_command(h, "anim_capture:anim.bake", "anim bake <pose id> [duration_ms]: every draw of a vanilla pose on the local player, replayed as a set clip and compared, under kyubu/capture/", cmd_bake, nullptr) != KY_OK)
        g_mod->warn("anim_capture: could not register the bake command");
    g_mod->on("render.overlay", on_overlay);
    char game[MAX_PATH] = {};
    if (api->game_dir(game, sizeof game) == KY_OK && make_dirs(std::string(game) + "\\kyubu\\inbox")) g_inbox_dir = std::string(game) + "\\kyubu\\inbox";
    else g_mod->warn("anim_capture: the inbox folder could not be made: the file inbox is off");
    g_mod->info("anim_capture: loaded (anim preview <clip> <t_ms> | anim preview off | anim capture <clip> [times_ms] [views] | anim rig | anim bake <pose id> [duration_ms])");
    return 0;
}

extern "C" KYUBU_EXPORT void KYUBU_CALL kyubu_mod_shutdown(KyubuSide) {}
