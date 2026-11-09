// The Animator's window: see app.h. The shape is the Win32 and Direct3D 9 example of Dear ImGui: one window class, one device,
// a reset when the device is lost or the window changes size. The figure is drawn into the middle of the window with the fixed
// pipeline from the pure code of camera.h, cubmesh.h, rig.h and playback.h; ImGui draws the panels over it.
#include "app.h"

#include <d3d9.h>
#include <windows.h>

#include <commdlg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "audio.h"
#include "camera.h"
#include "checks.h"
#include "cubmesh.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "ingame.h"
#include "kanim_core.h"
#include "keyinspector.h"
#include "model.h"
#include "particles.h"
#include "effects.h"
#include "playback.h"
#include "poselib.h"
#include "posing.h"
#include "rig.h"
#include "sheet.h"
#include "sounds.h"
#include "timeline.h"

// The backend's header keeps this declaration in an '#if 0' block: copy it here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#ifndef KYUBU_ANIMATOR_RIG_DIR
#define KYUBU_ANIMATOR_RIG_DIR ""
#endif

namespace kanimator {

namespace {

namespace anim = kyubu::game::anim;

IDirect3D9* g_d3d = nullptr;
IDirect3DDevice9* g_dev = nullptr;
D3DPRESENT_PARAMETERS g_pp = {};
bool g_device_lost = false;
UINT g_resize_w = 0, g_resize_h = 0;
bool g_close_requested = false;   // WM_CLOSE came: the loop asks about unsaved edits before the window goes

bool create_device(HWND hwnd) {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return false;
    g_pp = {};
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_pp.EnableAutoDepthStencil = TRUE;   // the 3D view needs depth
    g_pp.AutoDepthStencilFormat = D3DFMT_D16;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;   // vsync
    return g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_pp, &g_dev) >= 0;
}

void destroy_device() {
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    if (g_d3d) { g_d3d->Release(); g_d3d = nullptr; }
}

void reset_device() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_dev->Reset(&g_pp);
    if (hr == D3DERR_INVALIDCALL) return;   // a device object is still held: nothing here holds one
    ImGui_ImplDX9_CreateDeviceObjects();
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_resize_w = LOWORD(lp);
            g_resize_h = HIWORD(lp);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;   // no ALT application menu
        break;
    case WM_CLOSE:
        g_close_requested = true;   // not DefWindowProc: the loop asks about unsaved edits, then calls DestroyWindow
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// What the window shows

constexpr float kAssumedAbilityMs = 1000;   // an ability's length when a clip says `duration = "ability"` (kanim's own assumption)
constexpr float kGridHalf = 64;             // the ground grid reaches this far from the origin, in voxels
constexpr float kGridStep = 8;
constexpr float kTriadLength = 24;
constexpr float kLeftPanelW = 240;
constexpr float kBottomPanelH = 250;   // the play controls and the timeline
constexpr float kRightPanelW = 300;    // the key inspector over the curve view
constexpr float kKeyPanelH = 270;      // the inspector's share of the right panel
constexpr uint64_t kWatchMs = 400;     // how often the clip file is looked at for an outside edit

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string exe_dir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

// The one setting kept between runs (the pose library folder): %APPDATA%\kyubu-animator\settings.json. Empty when there is no
// APPDATA (the setting is then not kept).
std::string settings_path() { return settings_file_path(); }

void save_settings(const Settings& st) {
    const std::string p = settings_path();
    if (p.empty()) return;
    CreateDirectoryA(p.substr(0, p.find_last_of('\\')).c_str(), nullptr);   // exists already is fine
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (f) f << settings_text(st);
}

std::string first_errors(const std::vector<anim::ClipError>& errors, size_t max = 3) {
    std::string s;
    for (size_t i = 0; i < errors.size() && i < max; ++i) {
        if (i) s += "\n";
        s += anim::format_error(errors[i]);
    }
    if (errors.size() > max) s += "\n... and " + std::to_string(errors.size() - max) + " more";
    return s;
}

struct Scene {
    // The rig and what is drawn for each of its nodes.
    Rig rig;
    bool rig_ok = false;
    std::string rig_path;
    std::string rig_error;
    std::string models_dir;
    std::vector<Mesh> meshes;            // per node: empty for a node with no model
    std::vector<std::string> mesh_note;  // per node: why a box stands for a named model, or empty
    std::vector<std::pair<int, int>> tree;   // the nodes depth first, children in file order: (node, depth), for the parts list

    // The clip.
    ClipDoc doc;
    bool clip_loaded = false;            // a file was given and read
    bool clip_ok = false;                // and it is valid
    anim::Clip clip;
    std::string clip_name;
    std::string clip_error;
    float ability_ms = kAssumedAbilityMs;
    bool ability_set = false;            // the author typed a length: the clip-longer-than-ability check runs only then

    Playback pb;
    Camera cam;
    ViewPreset preset = ViewPreset::Free;
    bool show_grid = true;
    bool show_axes = true;
    int selected = -1;                   // a node of the rig: a click in the view or a line of the list
    Solved solved;

    // The rig's emitters (particles.h): their bursts over the clip, planned again when the clip's text or length changes.
    ParticlePlan plan;
    std::string plan_key;                // the clip's text and length the plan was made for; empty: none
    Mesh sparks;                         // the particles alive at the scrub time, in the figure's frame
    size_t spark_count = 0;
    // The rig's shockwaves and pillars (effects.h): their starts at the clip's markers, planned with the particles.
    EffectPlan effect_plan;
    Mesh pillars;                        // opaque, in the figure's frame
    Mesh rings;                          // see-through (the alpha in the vertex colours), in the figure's frame
    size_t ring_count = 0, pillar_count = 0;

    // Posing by hand (6.4).
    std::vector<NodeBox> boxes;          // per node: the box around its mesh, for picking
    Handles handles;                     // of the selected node's part, at the solved pose
    HandleHit hover;                     // the handle under the pointer
    struct PoseDrag {
        bool active = false;
        HandleKind kind = HandleKind::Turn;
        int axis = 0;
        HandleDrag math;
        std::string start_text;          // the clip's text at the press: each move edits this, the release makes the one undo step
        PoseEdit edit;                   // what is edited; `delta` is the latest
        PoseEditResult last;             // the edit of the latest move
    } drag;

    // The timeline (6.5).
    TimelineView tl;
    std::vector<TimelineRow> rows;       // of `clip`, made again each frame
    Selection key_sel;                   // keys of `clip`
    KeyClipboard clipboard;
    bool snap_on = true;
    float snap_step = kDefaultSnapMs;
    struct KeyDrag {
        enum Mode { None, Move, Box, Dead } mode = None;   // Dead: Escape ended it, the rest of the press does nothing
        ImVec2 press;
        bool moved = false;              // the press has gone past the slop
        float grabbed_ms = 0;            // Move: the time of the key under the pointer at the press
        float first_ms = 0;              // Move: the time of the first key of the selection at the press
        std::string start_text;          // Move: the clip's text at the press, as in PoseDrag
        Selection start_sel;             // Move: the keys moved; Box: the keys kept (Ctrl held)
        KeyEditResult last;              // Move: the edit of the latest pointer position
        int row0 = 0;                    // Box: the corner at the press
        float ms0 = 0;
    } kdrag;

    // The key inspector and the curve view (6.6).
    struct Inspect {
        bool active = false;             // a slider or a dot of the curve is held: `clip` shows the edit, the document is not touched
        std::string start_text;          // the clip's text at the press: each move edits this, the release makes the one undo step
        KeyRef key;                      // the key edited
        int axis = -1;                   // a curve drag: the component moved; -1 for a slider
        float v0[3] = {0, 0, 0};         // a curve drag: the key's vector at the press
        float grab_dy = 0;               // a curve drag: the dot's height above the pointer at the press, in pixels
        float range_lo = 0, range_hi = 1;   // a curve drag: the scale of the curve at the press (it would chase the dot)
        KeyEditResult last;              // the edit of the latest move
    } insp;
    std::string status;                 // the last thing an edit, an undo or a save did, or why it did not
    std::string note;                   // a second line for a pose edit: what to know about it (6.8)
    std::string note_for;               // the status it belongs to: the note is shown only while `status` is still that

    // Files, the file watch and the checks panel (6.7).
    enum class After { None, New, Open, Quit };   // what waits for the unsaved edits to be dealt with
    enum class Modal { None, Unsaved, Conflict, Recover };   // the question the window is asking
    Modal modal = Modal::None;
    After after = After::None;           // Unsaved: what to do once the author has chosen Save or Discard
    After todo = After::None;            // runs at the top of the next frame (a file dialog must not open inside an ImGui frame)
    std::string recover_text;            // Recover: the autosave found beside the clip
    uint64_t next_watch_ms = 0;
    std::vector<Check> checks;
    std::string checks_text;             // what `checks` was made from
    float checks_ability = -1;
    bool checks_rig = false;
    bool checks_valid = false;

    // The pose library and the base pose (6.8).
    PoseLibrary lib;
    PoseLibrary::Scan lib_scan;
    char lib_buf[512] = {0};             // the folder as typed; the library holds the folder that was last scanned
    bool show_pose = false;              // Show: the baked pose alone is drawn, at show_ms, with no tracks
    int show_id = -1;                    // the pose the panel works on (Show, Capture at t, Bake to keys); -1: none chosen
    float show_ms = 0;                   // the time in that pose
    BaseResult base;                     // the clip's base pose at the scrub time (solve_scene makes it)

    // The offline sheet (6.9).
    char sheet_times[64] = "0,0.5,1";    // fractions of the clip, as `kanim sample --t`
    char sheet_out[512] = {0};           // the PNG's path typed; empty: beside the clip

    // In game (7.6): the dev-mode file inbox. Off at every start; the game folder is kept in the settings.
    bool ingame_on = false;
    char game_buf[512] = {0};            // the folder that holds Cube.exe
    char ingame_id[128] = {0};           // the clip's id in the game, `<mod id>:<name>`; empty: taken from the clip's path

    // Sounds (sounds.h, audio.h): the rig's `[[sound]]`s at the clip's markers while it plays, and the .wav files of the Sounds panel.
    AudioOut audio;
    SoundCache sound_cache;
    bool sound_on = true;                // play the rig's sounds at the markers
    float sound_volume = 1;              // the panel's, times each sound's own; kept in the settings
    char sound_buf[512] = {0};           // the folder as typed
    std::string sound_folder;            // the folder that was last scanned: the sounds are looked up in it (kept in the settings)
    char sound_filter[64] = {0};
    std::vector<std::string> sound_list;       // the .wav files of sound_folder
    std::vector<std::string> clip_sound_list;  // the .wav files of the clip's mod's assets/sounds
    std::string sound_scan_key;          // the clip's path and the folder the two lists were read for
    std::vector<SoundCue> cues;          // the rig's sounds over the clip
    std::string cues_key;                // the clip's text, length and loop they were planned for
    bool sound_fresh = true;             // the next step of the playback is the first after Play: a cue at its start is heard
    std::string last_sound;              // the last sound played, for the view's top line
    std::string sound_note;              // why the last sound that failed did not play
};

void add_subtree(const Rig& rig, int node, int depth, std::vector<std::pair<int, int>>& out) {
    out.push_back({node, depth});
    for (size_t i = 0; i < rig.nodes.size(); ++i)
        if (rig.nodes[i].parent == node) add_subtree(rig, static_cast<int>(i), depth + 1, out);
}

void build_meshes(Scene& s) {
    s.tree.clear();
    if (s.rig.root >= 0) add_subtree(s.rig, s.rig.root, 0, s.tree);
    s.meshes.assign(s.rig.nodes.size(), Mesh{});
    s.mesh_note.assign(s.rig.nodes.size(), std::string());
    s.boxes.assign(s.rig.nodes.size(), NodeBox{});
    for (size_t i = 0; i < s.rig.nodes.size(); ++i) s.meshes[i] = node_mesh(s.rig, i, s.models_dir, s.mesh_note[i]);
    for (size_t i = 0; i < s.meshes.size(); ++i)
        s.boxes[i].valid = mesh_bounds(s.meshes[i], s.boxes[i].lo, s.boxes[i].hi);
}

void load_rig(Scene& s, const std::string& explicit_path) {
    std::vector<std::string> candidates;
    if (!explicit_path.empty()) {
        candidates.push_back(explicit_path);
    } else {
        candidates.push_back(exe_dir() + "\\rigs\\standin.rig.toml");
        candidates.push_back(std::string(KYUBU_ANIMATOR_RIG_DIR) + "/standin.rig.toml");
    }
    std::string text;
    for (const std::string& c : candidates) {
        if (c.empty() || !read_file(c, text)) continue;
        s.rig_path = c;
        RigLoad r = parse_rig(text, c);
        if (r.ok) {
            s.rig = std::move(r.rig);
            s.rig_ok = true;
            s.rig_error.clear();
        } else {
            s.rig_ok = false;
            s.rig_error = first_errors(r.errors);
        }
        return;
    }
    s.rig_ok = false;
    s.rig_error = "no rig file found (looked for " + candidates.front() + ")";
}

void apply_clip_length(Scene& s) {
    s.pb.set_length(s.clip_ok ? anim::resolve_duration(s.clip, s.ability_ms) : 0.0f);
}

void load_clip(Scene& s, const std::string& path) {
    s.clip_loaded = true;
    s.clip_ok = false;
    std::string err;
    if (!s.doc.open(path, err)) {
        s.clip_error = err;
        return;
    }
    name_from_path(path, s.clip_name);
    if (s.clip_name.empty()) {
        size_t slash = path.find_last_of("\\/");
        s.clip_name = slash == std::string::npos ? path : path.substr(slash + 1);
    }
    anim::ClipLoad cl = anim::parse_clip(s.doc.text(), s.clip_name);
    if (!cl.ok) {
        s.clip_error = first_errors(cl.errors);
        return;
    }
    s.clip = std::move(cl.clip);
    s.clip_ok = true;
    s.clip_error.clear();
    apply_clip_length(s);
}

// The pose the panel works on: the one chosen, or else the one the clip borrows.
int panel_pose_id(const Scene& s) {
    if (s.show_id >= 0) return s.show_id;
    if (s.clip_ok && s.clip.borrow.present) return static_cast<int>(s.clip.borrow.mode);
    return -1;
}

void solve_scene(Scene& s) {
    if (!s.rig_ok) return;
    anim::Pose base;
    s.base = BaseResult();
    const BakedPose* shown = s.show_pose ? s.lib.find(panel_pose_id(s)) : nullptr;
    if (shown) {
        s.solved = solve_pose(s.rig, sample_baked(*shown, s.show_ms));
    } else if (s.clip_ok && s.pb.length_ms > 0) {
        s.base = base_pose_for(s.lib, s.clip, s.pb.time_ms);   // the borrowed pose at the retimed clock, or the neutral pose
        s.solved = solve(s.rig, s.base.pose, s.clip, view_timing(s.pb));
    } else {
        s.solved = solve_pose(s.rig, base);
    }
    s.sparks.tris.clear();
    s.spark_count = 0;
    s.pillars.tris.clear();
    s.rings.tris.clear();
    s.ring_count = s.pillar_count = 0;
    if (!shown && s.clip_ok && s.pb.length_ms > 0 && (!s.rig.emitters.empty() || s.rig.has_effects())) {
        std::string key = s.doc.text();
        key += "\n#length " + std::to_string(s.pb.length_ms);
        if (key != s.plan_key) {
            auto base_at = [&s](float ms) { return base_pose_for(s.lib, s.clip, ms).pose; };
            s.plan = plan_particles(s.rig, s.clip, s.pb.length_ms, base_at);
            s.effect_plan = plan_effects(s.rig, s.clip, s.pb.length_ms, base_at);
            s.plan_key = std::move(key);
        }
        const std::vector<LiveParticle> live = live_particles(s.rig, s.plan, s.pb.time_ms, s.pb.loop);
        s.spark_count = live.size();
        s.sparks = particle_mesh(live);
        const LiveEffects fx = live_effects(s.rig, s.effect_plan, s.pb.time_ms, s.pb.loop);
        s.ring_count = fx.rings.size();
        s.pillar_count = fx.pillars.size();
        s.pillars = pillar_mesh(s.rig, fx.pillars);
        s.rings = ring_mesh(s.rig, fx.rings);
    }
    s.handles = make_handles(s.rig, s.solved, s.selected);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Sounds

std::string file_part(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::vector<std::string> scene_sound_folders(const Scene& s) { return sound_folders(s.doc.path(), s.sound_folder); }

// Reads the panel's two lists again when the clip's file or the scanned folder changed (or `force`: the Scan button).
void scan_sounds(Scene& s, bool force) {
    const std::string key = s.doc.path() + "\n" + s.sound_folder;
    if (!force && key == s.sound_scan_key) return;
    s.sound_scan_key = key;
    s.sound_list = list_sounds(s.sound_folder);
    s.clip_sound_list = list_sounds(clip_sound_folder(s.doc.path()));
}

// Plays the file at `path` at `volume` times the panel's. Why it did not play goes to the panel.
bool play_sound_path(Scene& s, const std::string& path, float volume, float pitch) {
    std::string why;
    if (!s.audio.play(s.sound_cache.get(path), volume * s.sound_volume, pitch, &why)) {
        s.sound_note = file_part(path) + ": " + why;
        return false;
    }
    s.last_sound = file_part(path);
    return true;
}

// Plays the rig's sound `i`, looked up in the sound folders.
bool play_rig_sound(Scene& s, size_t i) {
    const RigSound& rs = s.rig.sounds[i];
    const std::string path = find_sound(rs.file, scene_sound_folders(s));
    if (path.empty()) {
        s.sound_note = rs.file + ": not in the clip's mod's assets/sounds nor in the sound folder";
        return false;
    }
    return play_sound_path(s, path, rs.volume, rs.pitch);
}

// The rig's sounds over the clip, planned again when the clip's text, its length or the loop changes.
void plan_scene_sounds(Scene& s) {
    if (!s.rig_ok || !s.clip_ok || s.rig.sounds.empty() || s.pb.length_ms <= 0) {
        s.cues.clear();
        s.cues_key.clear();
        return;
    }
    std::string key = s.doc.text();
    key += "\n#length " + std::to_string(s.pb.length_ms) + (s.pb.loop ? " loop" : "");
    if (key == s.cues_key) return;
    s.cues = plan_sounds(s.rig, s.clip, s.pb.length_ms, s.pb.loop);
    s.cues_key = std::move(key);
}

// After the playback's step from `before_ms`: the cues it passed are played. A step that began paused plays none (a scrub is
// silent) and makes the next one the first after Play.
void play_due_sounds(Scene& s, bool was_playing, float before_ms) {
    s.audio.tick();
    if (!was_playing) {
        s.sound_fresh = true;
        return;
    }
    const bool fresh = s.sound_fresh;
    s.sound_fresh = false;
    plan_scene_sounds(s);
    if (!s.sound_on || s.cues.empty()) return;
    const bool wrapped = s.pb.time_ms < before_ms;
    for (size_t i : sounds_due(s.cues, before_ms, s.pb.time_ms, wrapped, fresh)) play_rig_sound(s, s.cues[i].sound);
}

// Takes the clip from the document's text again (after an undo). The text was valid when it became a step, so this does not fail.
void reparse_clip(Scene& s) {
    anim::ClipLoad cl = anim::parse_clip(s.doc.text(), s.clip_name);
    if (!cl.ok) {
        s.clip_ok = false;
        s.clip_error = first_errors(cl.errors);
        s.pb.set_length(0);
        return;
    }
    s.clip = std::move(cl.clip);
    s.clip_ok = true;
    s.clip_error.clear();
    apply_clip_length(s);
}

// The ball around every drawn node at the current pose.
Sphere scene_bounds(const Scene& s) {
    std::vector<Vec3> pts;
    for (size_t i = 0; i < s.meshes.size() && i < s.solved.world.size(); ++i) {
        Vec3 lo, hi;
        if (!mesh_bounds(s.meshes[i], lo, hi)) continue;
        const Affine& w = s.solved.world[i];
        for (int c = 0; c < 8; ++c) {
            float p[3] = {c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z}, q[3];
            w.apply(p, q);
            pts.push_back({q[0], q[1], q[2]});
        }
    }
    return bounding_sphere(pts.data(), pts.size());
}

void frame_scene(Scene& s) {
    Sphere b = scene_bounds(s);
    s.cam.frame(b.centre, b.radius);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Drawing

D3DMATRIX to_d3d(const Mat4& column_vector) {
    Mat4 t = transposed(column_vector);   // Direct3D multiplies a row vector from the left
    D3DMATRIX d;
    static_assert(sizeof(D3DMATRIX) == sizeof(t.m), "D3DMATRIX is 16 floats");
    std::memcpy(&d, t.m, sizeof d);
    return d;
}

void add_line(std::vector<MeshVertex>& v, Vec3 a, Vec3 b, uint32_t color) {
    v.push_back({a.x, a.y, a.z, color});
    v.push_back({b.x, b.y, b.z, color});
}

void set_view_states() {
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);   // the faces are drawn from both sides: no mesh is open
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    g_dev->SetTexture(0, nullptr);
    // colour = vertex colour times the texture factor (white, or a tint for the selected node)
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
    g_dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
}

void draw_view(const Scene& s, const Viewport& vp) {
    D3DVIEWPORT9 d3dvp;
    d3dvp.X = static_cast<DWORD>(std::max(vp.x, 0.0f));
    d3dvp.Y = static_cast<DWORD>(std::max(vp.y, 0.0f));
    d3dvp.Width = static_cast<DWORD>(std::max(vp.w, 1.0f));
    d3dvp.Height = static_cast<DWORD>(std::max(vp.h, 1.0f));
    d3dvp.MinZ = 0;
    d3dvp.MaxZ = 1;
    g_dev->SetViewport(&d3dvp);

    set_view_states();
    D3DMATRIX id = to_d3d(Mat4{});
    D3DMATRIX view = to_d3d(view_matrix(s.cam));
    D3DMATRIX proj = to_d3d(projection_matrix(s.cam, vp.h > 0 ? vp.w / vp.h : 1.0f));
    g_dev->SetTransform(D3DTS_WORLD, &id);
    g_dev->SetTransform(D3DTS_VIEW, &view);
    g_dev->SetTransform(D3DTS_PROJECTION, &proj);

    if (s.show_grid) {
        std::vector<MeshVertex> lines;
        for (float g = -kGridHalf; g <= kGridHalf; g += kGridStep) {
            uint32_t c = g == 0 ? 0xFF6A6D75 : 0xFF45484F;
            add_line(lines, {g, -kGridHalf, 0}, {g, kGridHalf, 0}, c);
            add_line(lines, {-kGridHalf, g, 0}, {kGridHalf, g, 0}, c);
        }
        g_dev->DrawPrimitiveUP(D3DPT_LINELIST, static_cast<UINT>(lines.size() / 2), lines.data(), sizeof(MeshVertex));
    }

    if (s.rig_ok) {
        for (size_t i = 0; i < s.meshes.size() && i < s.solved.world.size(); ++i) {
            const Mesh& m = s.meshes[i];
            if (m.empty()) continue;
            const Affine& w = s.solved.world[i];
            D3DMATRIX world = to_d3d(mat4_from_rt(w.r, w.t));
            g_dev->SetTransform(D3DTS_WORLD, &world);
            // the selected node is tinted yellow; a node the parity check found approximate (task 7.5) is tinted red
            g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, static_cast<int>(i) == s.selected ? 0xFFFFEB8C
                                                       : s.rig.nodes[i].approximate ? 0xFFFF9A8C : 0xFFFFFFFF);
            g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(m.triangle_count()), m.tris.data(), sizeof(MeshVertex));
        }
        g_dev->SetTransform(D3DTS_WORLD, &id);
        g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
        // The particles and the pillars, already in the figure's frame, in batches the device can take in one call.
        constexpr size_t kBatch = 30000;   // triangles
        auto draw_batched = [&](const Mesh& m) {
            for (size_t first = 0; first < m.triangle_count(); first += kBatch) {
                const size_t n = std::min(kBatch, m.triangle_count() - first);
                g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(n), m.tris.data() + first * 3, sizeof(MeshVertex));
            }
        };
        draw_batched(s.sparks);
        draw_batched(s.pillars);
        // The shockwaves last, see-through as EMTweaks draws them: blended, tested against the depth and not written to it.
        if (!s.rings.empty()) {
            g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            draw_batched(s.rings);
            g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        }
    }

    if (s.rig_ok && s.handles.ok) {
        // The handles of the selected part: a ring per turn, an arrow per move, always on top. The one under the pointer, or being
        // dragged, is yellow.
        const uint32_t axis_color[3] = {0xFFE05050, 0xFF50D060, 0xFF5090F0};
        const uint32_t lit = 0xFFFFE040;
        const float radius = handle_size(s.cam);
        const bool dragging = s.drag.active;
        auto is_lit = [&](HandleKind k, int a) {
            if (dragging) return s.drag.kind == k && s.drag.axis == a;
            return s.hover.hit && s.hover.kind == k && s.hover.axis == a;
        };
        std::vector<MeshVertex> lines;
        for (int a = 0; a < 3; ++a) {
            if (s.handles.turn[a].present) {
                const std::vector<Vec3> pts = ring_points(s.handles, a, radius);
                const uint32_t c = is_lit(HandleKind::Turn, a) ? lit : axis_color[a];
                for (size_t i = 0; i + 1 < pts.size(); ++i) add_line(lines, pts[i], pts[i + 1], c);
            }
            if (s.handles.move[a].present) {
                const Vec3 dir = s.handles.move[a].axis;
                const Vec3 tip = s.handles.origin + dir * (radius * 1.35f);
                const uint32_t c = is_lit(HandleKind::Move, a) ? lit : axis_color[a];
                add_line(lines, s.handles.origin, tip, c);
                Vec3 side = normalized(cross(dir, s.cam.forward()));
                const float barb = radius * 0.14f;
                add_line(lines, tip, tip - dir * barb + side * (barb * 0.5f), c);
                add_line(lines, tip, tip - dir * barb - side * (barb * 0.5f), c);
            }
        }
        g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_dev->DrawPrimitiveUP(D3DPT_LINELIST, static_cast<UINT>(lines.size() / 2), lines.data(), sizeof(MeshVertex));
        g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    }

    if (s.show_axes) {
        // The frame of the rig reference at the origin: x to the figure's right (red), y forward (green), z up (blue). Always on top.
        std::vector<MeshVertex> triad;
        add_line(triad, {0, 0, 0}, {kTriadLength, 0, 0}, 0xFFE05050);
        add_line(triad, {0, 0, 0}, {0, kTriadLength, 0}, 0xFF50D060);
        add_line(triad, {0, 0, 0}, {0, 0, kTriadLength}, 0xFF5090F0);
        g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_dev->DrawPrimitiveUP(D3DPT_LINELIST, 3, triad.data(), sizeof(MeshVertex));
        g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    }

    D3DVIEWPORT9 full = d3dvp;
    full.X = 0;
    full.Y = 0;
    full.Width = static_cast<DWORD>(ImGui::GetIO().DisplaySize.x);
    full.Height = static_cast<DWORD>(ImGui::GetIO().DisplaySize.y);
    g_dev->SetViewport(&full);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Panels and input

void toggle_play(Scene& s) {
    if (s.pb.playing) {
        s.pb.pause();
        s.audio.stop_all();   // a pause by hand silences the sounds; the clip's own end lets them ring out
    } else {
        s.pb.play();
    }
}

void pick_preset(Scene& s, ViewPreset p) {
    s.preset = p;
    s.cam.set_preset(p);
}

// The menu bar.
void do_undo(Scene& s) {
    if (s.drag.active || s.insp.active) return;
    if (!s.doc.undo()) {
        s.status = "nothing to undo";
        return;
    }
    reparse_clip(s);
    s.key_sel.clear();   // the keys are others now
    s.status = "undone";
}

// The key edits of the timeline (6.5). Each is one undo step.
bool idle_for_keys(const Scene& s) {
    return s.clip_ok && s.pb.length_ms > 0 && !s.drag.active && s.kdrag.mode == Scene::KeyDrag::None;
}

bool can_edit_keys(const Scene& s) { return idle_for_keys(s) && !s.insp.active; }

float snapped(const Scene& s, float t_ms) { return snap_ms(t_ms, s.snap_on ? s.snap_step : 0.0f, s.pb.length_ms); }

bool commit_key_edit(Scene& s, KeyEditResult r, const std::string& what) {
    if (!r.ok) {
        s.status = r.error;
        return false;
    }
    s.doc.set_text(r.text);
    s.clip = std::move(r.clip);
    s.key_sel = std::move(r.selection);
    s.status = what;
    return true;
}

std::string count_of(size_t n, const char* one) { return std::to_string(n) + " " + one + (n == 1 ? "" : "s"); }

void do_copy_keys(Scene& s) {
    if (!can_edit_keys(s)) return;
    if (s.key_sel.empty()) {
        s.status = "select keys in the timeline to copy them";
        return;
    }
    s.clipboard = copy_keys(s.clip, s.key_sel, s.pb.length_ms);
    s.status = count_of(s.clipboard.keys.size(), "key") + " copied";
}

void do_paste_keys(Scene& s) {
    if (!can_edit_keys(s)) return;
    const float at = snapped(s, s.pb.time_ms);
    KeyEditResult r = paste_keys(s.doc.text(), s.clipboard, at, s.pb.length_ms);
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s pasted at %.0f ms", count_of(r.selection.size(), "key").c_str(), at);
    std::string what = buf;
    if (r.replaced) what += ", " + std::to_string(r.replaced) + " replaced";
    if (r.dropped) what += ", " + std::to_string(r.dropped) + " past the end dropped";
    if (r.made_tracks) what += ", " + count_of(static_cast<size_t>(r.made_tracks), "new track");
    commit_key_edit(s, std::move(r), what);
}

void do_delete_keys(Scene& s) {
    if (!can_edit_keys(s)) return;
    if (s.key_sel.empty()) {
        s.status = "select keys in the timeline to delete them";
        return;
    }
    const size_t n = s.key_sel.size();
    commit_key_edit(s, delete_keys(s.doc.text(), s.key_sel), count_of(n, "key") + " deleted");
}

void do_save(Scene& s) {
    std::string err;
    if (s.drag.active || s.insp.active || !s.clip_loaded) return;
    if (s.doc.save(err)) s.status = "saved " + s.clip_name + ".anim.toml";
    else s.status = err;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Files (6.7): new, open, the file watch, the autosave and the question about unsaved edits.

bool held_edit(const Scene& s) { return s.drag.active || s.insp.active || s.kdrag.mode != Scene::KeyDrag::None; }

// The file dialog of Windows. `save` asks for the name of a clip that does not exist yet. Empty when the author cancels.
std::string pick_clip_path(HWND owner, bool save) {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Animation clips (*.anim.toml)\0*.anim.toml\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(sizeof buf / sizeof buf[0]);
    ofn.lpstrTitle = save ? L"New clip: choose the file" : L"Open a clip";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | (save ? 0 : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// "name", "name.toml" and "name.anim" all give "name.anim.toml"; a path that ends so already is left as it is.
std::string with_clip_suffix(std::string path) {
    auto ends = [&](const char* tail) {
        const size_t n = std::strlen(tail);
        return path.size() >= n && path.compare(path.size() - n, n, tail) == 0;
    };
    if (ends(".anim.toml")) return path;
    if (ends(".toml")) path.resize(path.size() - 5);
    if (ends(".anim")) path.resize(path.size() - 5);
    return path + ".anim.toml";
}

// `d` becomes the document: the clip is parsed, the selection and the held edits are gone, the view starts at 0.
void adopt_doc(Scene& s, ClipDoc&& d) {
    s.doc = std::move(d);
    s.clip_loaded = true;
    s.clip_name.clear();
    if (!name_from_path(s.doc.path(), s.clip_name)) {
        const std::string& path = s.doc.path();
        const size_t slash = path.find_last_of("\\/");
        s.clip_name = slash == std::string::npos ? path : path.substr(slash + 1);
    }
    s.key_sel.clear();
    s.pb.pause();
    s.pb.time_ms = 0;
    s.tl.zoom = 1;
    reparse_clip(s);
    s.checks_valid = false;
}

// An autosave that an earlier run left beside the clip and that is not the file: ask whether to take it.
void offer_recovery(Scene& s) {
    std::string text;
    if (s.clip_loaded && s.doc.find_autosave(text)) {
        s.recover_text = std::move(text);
        s.modal = Scene::Modal::Recover;
    }
}

// Reads `path` into a fresh document, so a file that cannot be read leaves the open clip as it is.
bool open_clip_path(Scene& s, const std::string& path) {
    ClipDoc d;
    std::string err;
    if (!d.open(path, err)) {
        s.status = err;
        return false;
    }
    adopt_doc(s, std::move(d));
    s.status = s.clip_ok ? "opened " + s.clip_name + ".anim.toml" : "opened " + s.clip_name + ".anim.toml, which has errors: see Checks";
    offer_recovery(s);
    return true;
}

// A new clip at `path` (the suffix added). Nothing is written until Save; a file that exists is not taken over (Open it).
bool new_clip_path(Scene& s, const std::string& chosen) {
    const std::string path = with_clip_suffix(chosen);
    std::string name;
    if (!name_from_path(path, name)) {
        s.status = "not a clip name: the file name is 1 to 64 of a-z, 0-9, '_', '-' and '.', then .anim.toml";
        return false;
    }
    if (stamp_of(path).size >= 0) {
        s.status = name + ".anim.toml exists: use Open";
        return false;
    }
    ClipDoc d;
    d.start_new(path, new_clip_text(name));
    adopt_doc(s, std::move(d));
    s.status = "new clip " + name + ".anim.toml, not saved yet";
    return true;
}

void run_after(Scene& s, HWND hwnd, Scene::After a) {
    switch (a) {
    case Scene::After::New: {
        const std::string path = pick_clip_path(hwnd, true);
        if (!path.empty()) new_clip_path(s, path);
        break;
    }
    case Scene::After::Open: {
        const std::string path = pick_clip_path(hwnd, false);
        if (!path.empty()) open_clip_path(s, path);
        break;
    }
    case Scene::After::Quit:
        DestroyWindow(hwnd);
        break;
    case Scene::After::None:
        break;
    }
}

// New, Open or Quit was asked for. With unsaved edits the window asks first; the action itself runs at the top of the next frame.
void begin_after(Scene& s, Scene::After a) {
    if (held_edit(s) || s.modal != Scene::Modal::None) return;
    if (s.clip_loaded && s.doc.dirty()) {
        s.after = a;
        s.modal = Scene::Modal::Unsaved;
    } else {
        s.todo = a;
    }
}

// The file on disk was changed by someone else and the document has no unsaved edits: take it.
void reload_from_disk(Scene& s) {
    const std::string before = s.doc.text();
    std::string err;
    if (!s.doc.reload(err)) {
        s.status = err + " (the file watch tries again)";
        return;
    }
    if (s.doc.text() == before) return;   // the time changed, the text did not
    reparse_clip(s);
    s.key_sel.clear();
    s.status = "reloaded " + s.clip_name + ".anim.toml: it changed outside the Animator";
}

// Called every frame: looks at the file now and then. Not while a drag holds the document, nor while a question is open.
void watch_disk(Scene& s, uint64_t now_ms) {
    if (!s.clip_loaded || now_ms < s.next_watch_ms) return;
    s.next_watch_ms = now_ms + kWatchMs;
    if (held_edit(s) || s.modal != Scene::Modal::None) return;
    if (!s.doc.disk_changed()) return;
    if (s.doc.dirty()) s.modal = Scene::Modal::Conflict;
    else reload_from_disk(s);
}

void autosave_tick(Scene& s, uint64_t now_ms) {
    if (!s.clip_loaded || held_edit(s)) return;
    std::string err;
    if (s.doc.autosave_tick(static_cast<int64_t>(now_ms), kAutosaveMs, err)) s.status = "autosaved to " + s.clip_name + ".anim.toml.autosave";
    else if (!err.empty()) s.status = "autosave failed: " + err;
}

// The checks are made again when the text, the typed ability length or the rig changes.
void refresh_checks(Scene& s) {
    if (!s.clip_loaded) {
        s.checks.clear();
        s.checks_valid = false;
        return;
    }
    const float ability = s.ability_set ? s.ability_ms : 0.0f;
    if (s.checks_valid && s.checks_ability == ability && s.checks_rig == s.rig_ok && s.checks_text == s.doc.text()) return;
    s.checks = run_checks(s.doc.text(), s.clip_name, ability, s.rig_ok ? &s.rig : nullptr);
    s.checks_text = s.doc.text();
    s.checks_ability = ability;
    s.checks_rig = s.rig_ok;
    s.checks_valid = true;
}

void draw_menus(HWND hwnd, Scene& s) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        const bool idle = !held_edit(s) && s.modal == Scene::Modal::None;
        if (ImGui::MenuItem("New clip...", nullptr, false, idle)) begin_after(s, Scene::After::New);
        if (ImGui::MenuItem("Open clip...", nullptr, false, idle)) begin_after(s, Scene::After::Open);
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, idle && s.clip_loaded && s.clip_ok && s.doc.dirty())) do_save(s);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, s.doc.undo_depth() > 0 && !s.drag.active)) do_undo(s);
        ImGui::Separator();
        const bool keys = can_edit_keys(s) && !s.key_sel.empty();
        if (ImGui::MenuItem("Copy keys", "Ctrl+C", false, keys)) do_copy_keys(s);
        if (ImGui::MenuItem("Paste keys at the scrub time", "Ctrl+V", false, can_edit_keys(s) && !s.clipboard.empty())) do_paste_keys(s);
        if (ImGui::MenuItem("Delete keys", "Del", false, keys)) do_delete_keys(s);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Front", nullptr, s.preset == ViewPreset::Front)) pick_preset(s, ViewPreset::Front);
        if (ImGui::MenuItem("Left", nullptr, s.preset == ViewPreset::Left)) pick_preset(s, ViewPreset::Left);
        if (ImGui::MenuItem("Free", nullptr, s.preset == ViewPreset::Free)) pick_preset(s, ViewPreset::Free);
        ImGui::Separator();
        if (ImGui::MenuItem("Frame the figure")) frame_scene(s);
        ImGui::MenuItem("Ground grid", nullptr, &s.show_grid);
        ImGui::MenuItem("Axes", nullptr, &s.show_axes);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Playback")) {
        if (ImGui::MenuItem(s.pb.playing ? "Pause" : "Play", "Space", false, s.pb.length_ms > 0)) toggle_play(s);
        ImGui::MenuItem("Loop", nullptr, &s.pb.loop);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// The question the window is asking, if any. Each button ends it.
void draw_modals(Scene& s) {
    const char* title = s.modal == Scene::Modal::Unsaved   ? "Unsaved changes"
                        : s.modal == Scene::Modal::Conflict ? "The file changed on disk"
                                                            : "Recover an autosave?";
    if (s.modal == Scene::Modal::None) return;
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    const ImVec2 mid(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.4f);
    ImGui::SetNextWindowPos(mid, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const std::string file = s.clip_name + ".anim.toml";
    if (s.modal == Scene::Modal::Unsaved) {
        ImGui::Text("%s has changes that are not saved.", file.c_str());
        if (ImGui::Button("Save", ImVec2(110, 0))) {
            do_save(s);
            if (!s.doc.dirty()) {
                s.todo = s.after;
                s.after = Scene::After::None;
                s.modal = Scene::Modal::None;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(110, 0))) {
            s.doc.drop_autosave();
            s.todo = s.after;
            s.after = Scene::After::None;
            s.modal = Scene::Modal::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) {
            s.after = Scene::After::None;
            s.modal = Scene::Modal::None;
            ImGui::CloseCurrentPopup();
        }
    } else if (s.modal == Scene::Modal::Conflict) {
        ImGui::Text("%s was changed outside the Animator, and it has changes here that are not saved.", file.c_str());
        if (ImGui::Button("Load the file", ImVec2(150, 0))) {
            s.doc.drop_autosave();
            std::string err;
            if (s.doc.reload(err)) {
                // The document has the file's text; its undo stack is gone.
                reparse_clip(s);
                s.key_sel.clear();
                s.status = "loaded " + file + " from disk: the changes made here are gone";
            } else {
                s.status = err;
            }
            s.modal = Scene::Modal::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep my changes", ImVec2(150, 0))) {
            s.doc.accept_disk();   // the next Save overwrites the outside edit
            s.status = "kept the changes made here: Save overwrites the outside edit";
            s.modal = Scene::Modal::None;
            ImGui::CloseCurrentPopup();
        }
    } else {
        ImGui::Text("An autosave of %s differs from the file.", file.c_str());
        ImGui::TextDisabled("The Animator ended before it was saved. Recover takes the autosave as unsaved changes.");
        if (ImGui::Button("Recover", ImVec2(110, 0))) {
            anim::ClipLoad cl = anim::parse_clip(s.recover_text, s.clip_name);
            if (cl.ok) {
                s.doc.set_text(s.recover_text);
                reparse_clip(s);
                s.key_sel.clear();
                s.status = "recovered the autosave of " + file + ": not saved yet";
            } else {
                s.status = "the autosave has errors and was not taken: " + first_errors(cl.errors, 1);
            }
            s.recover_text.clear();
            s.modal = Scene::Modal::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(110, 0))) {
            s.doc.drop_autosave();
            s.recover_text.clear();
            s.status = "autosave discarded";
            s.modal = Scene::Modal::None;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

// The checks panel's list: errors in red, warnings in amber, the fix hint as a tooltip.
void draw_checks(const Scene& s) {
    const size_t errors = count_checks(s.checks, Severity::Error), warnings = count_checks(s.checks, Severity::Warning);
    if (errors == 0 && warnings == 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "Checks: no findings");
        return;
    }
    ImGui::TextColored(errors ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(1, 0.7f, 0.3f, 1), "Checks: %s, %s", count_of(errors, "error").c_str(),
                       count_of(warnings, "warning").c_str());
    for (const Check& c : s.checks) {
        ImGui::PushStyleColor(ImGuiCol_Text, c.severity == Severity::Error ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(1, 0.7f, 0.3f, 1));
        ImGui::TextWrapped("%s", format_check(c).c_str());
        ImGui::PopStyleColor();
        if (!c.hint.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.hint.c_str());
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The pose library, Show, Capture at t and Bake to keys (6.8). The edits are poselib.h's; this is the panel.

void scan_library(Scene& s, const std::string& folder) {
    s.lib_scan = s.lib.scan(folder);
    if (!s.lib.find(panel_pose_id(s))) s.show_pose = false;   // the pose is gone from the library
}

// A clip text from one of the pose edits: parsed with the loader's validator and taken as one undo step, or refused whole.
bool commit_text_edit(Scene& s, const std::string& text, const std::string& what) {
    anim::ClipLoad cl = anim::parse_clip(text, s.clip_name);
    if (!cl.ok) {
        s.status = "the edit would make the clip invalid: " + first_errors(cl.errors, 1);
        return false;
    }
    s.doc.set_text(text);
    s.clip = std::move(cl.clip);
    s.key_sel.clear();
    apply_clip_length(s);
    s.status = what;
    s.note.clear();
    return true;
}

void do_capture_at(Scene& s) {
    if (!can_edit_keys(s)) {
        s.status = "open a clip to capture into";
        return;
    }
    const int id = panel_pose_id(s);
    const BakedPose* p = s.lib.find(id);
    if (!p) {
        s.status = id < 0 ? "choose a baked pose first" : "pose " + std::to_string(id) + " is not in the pose library";
        return;
    }
    const float at = snapped(s, s.pb.time_ms);
    std::string out, err;
    CaptureResult r;
    if (!capture_at(s.doc.text(), s.clip, *p, s.show_ms, at / s.pb.length_ms, s.pb.length_ms, out, err, &r)) {
        s.status = err;
        return;
    }
    char buf[220];
    std::snprintf(buf, sizeof buf, "pose %d (%.0f ms) captured at %.0f ms: %d tracks made, %d keys added, %d replaced", id, s.show_ms, at,
                  r.tracks_made, r.keys_added, r.keys_replaced);
    if (commit_text_edit(s, out, buf) && r.over_add_tracks) {
        s.note = std::to_string(r.over_add_tracks) + " of those part and channel pairs also have an add track: the set key runs after it";
        s.note_for = s.status;
    }
}

void do_bake_to_keys(Scene& s) {
    if (!can_edit_keys(s)) {
        s.status = "open a clip to bake into";
        return;
    }
    const int id = panel_pose_id(s);
    const BakedPose* p = s.lib.find(id);
    if (!p) {
        s.status = id < 0 ? "choose a baked pose first" : "pose " + std::to_string(id) + " is not in the pose library";
        return;
    }
    std::string out, err;
    BakeResult r;
    if (!bake_to_keys(s.doc.text(), s.clip, *p, s.pb.length_ms, out, err, &r)) {
        s.status = err;
        return;
    }
    char buf[220];
    std::snprintf(buf, sizeof buf, "pose %d baked: %d keys on %d tracks, %d set tracks replaced", id, r.keys, r.tracks_made,
                  r.tracks_replaced);
    std::string note;
    if (r.dropped) note = std::to_string(r.dropped) + " draws past the clip's end were dropped. ";
    if (s.clip.borrow.present && static_cast<int>(s.clip.borrow.mode) == id) note += "The clip still borrows this pose: remove [borrow], the pose is in the keys now.";
    if (commit_text_edit(s, out, buf) && !note.empty()) {
        s.note = note;
        s.note_for = s.status;
    }
}

Settings current_settings(const Scene& s) {
    Settings st;
    st.library_folder = s.lib_buf;
    st.game_folder = s.game_buf;
    st.sound_folder = s.sound_folder;
    st.sound_volume = s.sound_volume;
    return st;
}

// The clip's id for `anim preview`: what the author typed, else `<mod id>:<name>` from the clip's path, else the file's name alone (the
// game then answers that it has no such clip, and the status says what the line was).
std::string ingame_id_of(const Scene& s) {
    if (s.ingame_id[0]) return s.ingame_id;
    const std::string derived = ingame_clip_id(s.doc.path(), s.clip_name);
    return derived.empty() ? s.clip_name : derived;
}

void draw_pose_library(Scene& s) {
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Pose library", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##libfolder", s.lib_buf, sizeof s.lib_buf);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The folder of baked poses (anim bake): its folders, or its captures folder, hold bake_pose<id>-<stamp>/pose.json.");
    if (ImGui::Button("Scan")) {
        scan_library(s, s.lib_buf);
        save_settings(current_settings(s));
        s.status = s.lib_scan.problems.empty() || s.lib_scan.loaded ? count_of(static_cast<size_t>(s.lib_scan.loaded), "baked pose") + " read"
                                                                       : s.lib_scan.problems[0];
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d poses", static_cast<int>(s.lib.size()));
    for (size_t i = 0; i < s.lib_scan.problems.size() && i < 3; ++i)
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", s.lib_scan.problems[i].c_str());
    if (s.lib_scan.problems.size() > 3) ImGui::TextDisabled("... and %d more", static_cast<int>(s.lib_scan.problems.size()) - 3);

    if (s.lib.size() == 0) {
        ImGui::TextWrapped("No baked poses. Run anim bake <pose id> in the game, copy the folder here and scan.");
        return;
    }
    // The pose to work on: a combo of the ids the library holds.
    const int id = panel_pose_id(s);
    char label[32];
    std::snprintf(label, sizeof label, id >= 0 ? "pose %d" : "choose a pose", id);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##poseid", label)) {
        for (int pid : s.lib.ids()) {
            char item[32];
            std::snprintf(item, sizeof item, "pose %d", pid);
            if (ImGui::Selectable(item, pid == id)) {
                s.show_id = pid;
                s.show_ms = 0;
            }
        }
        ImGui::EndCombo();
    }
    const BakedPose* p = s.lib.find(id);
    if (id >= 0 && !p) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "pose %d is not in the library", id);
        return;
    }
    if (!p) return;
    const float length = static_cast<float>(p->length_ms());
    s.show_ms = std::clamp(s.show_ms, 0.0f, length);
    ImGui::Checkbox("Show", &s.show_pose);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draw the baked pose alone, at the pose time below. The clip's tracks are not drawn.");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##posetime", &s.show_ms, 0.0f, std::max(length, 1.0f), "pose time %.0f ms");
    ImGui::TextDisabled("%d draws, %.0f ms", static_cast<int>(p->frames.size()), length);
    const bool can = can_edit_keys(s);
    if (!can) ImGui::BeginDisabled();
    if (ImGui::Button("Capture at t")) do_capture_at(s);
    if (ImGui::IsItemHovered() && can)
        ImGui::SetTooltip("Make a key of each part's vectors at the pose time, at the scrub time of the clip (set tracks).");
    ImGui::SameLine();
    if (ImGui::Button("Bake to keys")) do_bake_to_keys(s);
    if (ImGui::IsItemHovered() && can)
        ImGui::SetTooltip("Turn the whole pose into keys (30 a second at most) from the clip's time 0. It replaces the set tracks of the\n"
                          "parts it writes; add tracks stay. One undo step.");
    if (!can) ImGui::EndDisabled();
}

bool contains_any_case(const std::string& text, const char* part) {
    std::string a = text, b = part;
    for (char& ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (char& ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return a.find(b) != std::string::npos;
}

// One folder's .wav files in the panel's list: a click plays the file as it is (volume 1 times the panel's, pitch 1).
void sound_rows(Scene& s, const std::string& folder, const std::vector<std::string>& names) {
    for (const std::string& n : names) {
        if (s.sound_filter[0] && !contains_any_case(n, s.sound_filter)) continue;
        const std::string path = folder + "/" + n;
        if (ImGui::Selectable(n.c_str())) play_sound_path(s, path, 1, 1);
        if (ImGui::IsItemHovered()) {
            const std::shared_ptr<const LoadedSound> snd = s.sound_cache.get(path);
            std::string tip = describe_wav(snd->info);
            if (snd->info.ok && !snd->info.game_takes) tip += "\nThe game does not load this format: it takes 16-bit PCM or 32-bit float.";
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }
}

void draw_sounds_panel(Scene& s) {
    const ImVec4 warn(1, 0.7f, 0.3f, 1);
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Sounds")) return;
    scan_sounds(s, false);
    if (!s.audio.ok()) {
        ImGui::PushStyleColor(ImGuiCol_Text, warn);
        ImGui::TextWrapped("silent: %s", s.audio.why().c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Checkbox("At markers", &s.sound_on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While the clip plays, each [[sound]] of the rig plays at the clip's markers of its `at` (a scrub is\n"
                          "silent). A pause by hand stops them; the clip's end lets them ring out.");
    ImGui::SameLine();
    if (ImGui::Button("Stop##sounds")) s.audio.stop_all();
    ImGui::SameLine();
    ImGui::TextDisabled("%d playing", static_cast<int>(s.audio.playing()));
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##soundvolume", &s.sound_volume, 0.0f, 2.0f, "volume %.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) save_settings(current_settings(s));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Times each sound's own volume: at 1, a sound of volume 1 is as loud as the file.");

    const std::vector<std::string> folders = scene_sound_folders(s);
    if (s.rig_ok && !s.rig.sounds.empty()) {
        ImGui::TextDisabled("the rig's sounds (click: play)");
        for (size_t i = 0; i < s.rig.sounds.size(); ++i) {
            const RigSound& rs = s.rig.sounds[i];
            const std::string path = find_sound(rs.file, folders);
            const size_t marks = s.clip_ok ? marker_times(s.clip, rs.at, 1).size() : 0;
            const std::string label = rs.file + "  at " + rs.at;
            const bool tinted = path.empty() || marks == 0;
            if (tinted) ImGui::PushStyleColor(ImGuiCol_Text, path.empty() ? warn : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str())) play_rig_sound(s, i);
            ImGui::PopID();
            if (tinted) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                std::string tip;
                if (path.empty()) {
                    tip = "not found in:";
                    for (const std::string& f : folders) tip += "\n  " + f;
                    if (folders.empty()) tip += " no folder (the clip is not in a mod's assets/animations, and none is scanned)";
                } else {
                    tip = path + "\n" + describe_wav(s.sound_cache.get(path)->info);
                }
                tip += marks == 0 ? "\nthe clip has no marker \"" + rs.at + "\"" : "\n" + count_of(marks, "marker") + " \"" + rs.at + "\"";
                char nums[96];
                std::snprintf(nums, sizeof nums, "\nvolume %.2f, pitch %.2f, delay %.0f ms", rs.volume, rs.pitch, rs.delay_ms);
                tip += nums;
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }
    } else if (s.rig_ok) {
        ImGui::TextDisabled("the rig has no [[sound]]");
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##soundfolder", "a folder of .wav files", s.sound_buf, sizeof s.sound_buf);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A folder of .wav files, such as the game's (cwmod extract writes them to <work>/vanilla/sounds). A sound is\n"
                          "looked for in the clip's mod's assets/sounds first, as the game serves a mod's own file first.");
    if (ImGui::Button("Scan##sounds")) {
        s.sound_folder = s.sound_buf;
        s.sound_note.clear();
        scan_sounds(s, true);
        save_settings(current_settings(s));
        s.status = count_of(s.sound_list.size() + s.clip_sound_list.size(), "sound file") + " found";
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d files", static_cast<int>(s.sound_list.size() + s.clip_sound_list.size()));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##soundfilter", "filter", s.sound_filter, sizeof s.sound_filter);
    ImGui::BeginChild("##soundlist", ImVec2(0, 160), ImGuiChildFlags_Borders);
    const std::string clip_folder = clip_sound_folder(s.doc.path());
    if (!s.clip_sound_list.empty()) {
        ImGui::TextDisabled("the clip's mod");
        ImGui::PushID("clip");
        sound_rows(s, clip_folder, s.clip_sound_list);
        ImGui::PopID();
    }
    if (!s.sound_list.empty()) {
        ImGui::TextDisabled("%s", file_part(s.sound_folder).c_str());
        ImGui::PushID("folder");
        sound_rows(s, s.sound_folder, s.sound_list);
        ImGui::PopID();
    }
    if (s.sound_list.empty() && s.clip_sound_list.empty()) ImGui::TextDisabled("no .wav files: type a folder and scan");
    ImGui::EndChild();
    if (!s.sound_note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, warn);
        ImGui::TextWrapped("%s", s.sound_note.c_str());
        ImGui::PopStyleColor();
    }
}

// Where the sheet goes when no path is typed: beside the clip, `<name>.sheet.png`. Empty when no clip file is open.
std::string default_sheet_path(const Scene& s) {
    std::string p = s.doc.path();
    if (p.empty()) return p;
    const std::string ext = ".anim.toml";
    if (p.size() > ext.size() && p.compare(p.size() - ext.size(), ext.size(), ext) == 0) p.resize(p.size() - ext.size());
    return p + ".sheet.png";
}

// Writes the labelled two-view sheet of the clip as it is on screen (unsaved edits included) on the rig, with the same code as
// `kyubu-animator --sheet`.
void do_write_sheet(Scene& s) {
    if (!s.rig_ok || !s.clip_ok) {
        s.status = "open a valid clip on a valid rig first";
        return;
    }
    std::vector<float> fractions;
    if (!kyubu::kanim::parse_times(s.sheet_times, fractions)) {
        s.status = "sheet times: numbers from 0 to 1, such as 0,0.5,1";
        return;
    }
    if (fractions.size() > kSheetMaxTimes) {
        s.status = "at most " + std::to_string(kSheetMaxTimes) + " times in one sheet";
        return;
    }
    const std::string path = s.sheet_out[0] ? std::string(s.sheet_out) : default_sheet_path(s);
    if (path.empty()) {
        s.status = "type the path of the PNG";
        return;
    }
    SheetRequest req;
    req.rig = &s.rig;
    req.meshes = &s.meshes;
    req.clip = &s.clip;
    req.library = &s.lib;
    req.clip_name = s.clip_name;
    req.ability_ms = s.ability_ms;
    req.fractions = fractions;
    req.grid = s.show_grid;
    SheetResult r = make_mimic_sheet(req);
    if (!r.ok) {
        s.status = r.error;
        return;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(r.png.data()), static_cast<std::streamsize>(r.png.size()));
    if (!f) {
        s.status = "cannot write " + path;
        return;
    }
    // The file's name only: the status line is drawn over the view, and a full path runs into its title.
    const size_t slash = path.find_last_of("\\/");
    s.status = "sheet written: " + (slash == std::string::npos ? path : path.substr(slash + 1));
    std::string note;
    for (const std::string& n : r.notes) note += (note.empty() ? "" : ". ") + n;
    s.note = note;
    s.note_for = s.status;
}

void draw_sheet_panel(Scene& s) {
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Sheet")) return;
    ImGui::TextDisabled("times, as fractions of the clip");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##sheettimes", s.sheet_times, sizeof s.sheet_times);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Up to %d numbers from 0 to 1, comma between (as kanim sample --t). Each time is a row: the front and left views.",
                          static_cast<int>(kSheetMaxTimes));
    const std::string def = default_sheet_path(s);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##sheetout", def.empty() ? "path of the PNG" : def.c_str(), s.sheet_out, sizeof s.sheet_out);
    const bool can = s.rig_ok && s.clip_ok;
    if (!can) ImGui::BeginDisabled();
    if (ImGui::Button("Write sheet")) do_write_sheet(s);
    if (ImGui::IsItemHovered() && can)
        ImGui::SetTooltip("A labelled picture of the mimic: the clip on this rig at those times, in the front and left views. Its header\n"
                          "names the rig, so it is not taken for a capture of the game. kyubu-animator --sheet does the same with no window.");
    if (!can) ImGui::EndDisabled();
}

void draw_ingame_panel(Scene& s) {
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("In game")) return;
    ImGui::Checkbox("Send to the game (dev mode)", &s.ingame_on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off at every start. On, the button drops anim preview <clip> <t_ms> in <game>/kyubu/inbox/, where the\n"
                          "anim_capture mod reads it (the game must run in dev mode). The clip must be saved in the game's mod folder;\n"
                          "the game reloads it when it changes.");
    if (!s.ingame_on) return;
    ImGui::TextDisabled("the game folder (Cube.exe)");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##gamefolder", s.game_buf, sizeof s.game_buf)) save_settings(current_settings(s));
    ImGui::TextDisabled("the clip's id in the game");
    ImGui::SetNextItemWidth(-1);
    const std::string derived = ingame_clip_id(s.doc.path(), s.clip_name);
    ImGui::InputTextWithHint("##gameid", derived.empty() ? "mod:name" : derived.c_str(), s.ingame_id, sizeof s.ingame_id);
    const bool can = s.clip_loaded && s.game_buf[0] && !ingame_id_of(s).empty();
    if (!can) ImGui::BeginDisabled();
    if (ImGui::Button("In game")) {
        const std::string line = ingame_preview_line(ingame_id_of(s), s.pb.time_ms);
        std::string why;
        s.status = ingame_drop(s.game_buf, line, &why) ? "sent to the game: " + line : "not sent: " + why;
    }
    if (ImGui::IsItemHovered() && can)
        ImGui::SetTooltip("Holds the clip's pose at the scrub time on the player in the game. `anim preview off` in the console ends it.");
    if (!can) ImGui::EndDisabled();
}

void draw_parts(Scene& s, float top, float height) {
    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(ImVec2(kLeftPanelW, height));
    ImGui::Begin("Parts", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    if (s.rig_ok) {
        ImGui::TextUnformatted(s.rig.name.c_str());
        if (!s.clip_loaded) ImGui::TextDisabled("no clip: File > New clip or Open clip");
        else if (s.clip_ok) ImGui::Text("clip %s%s, %.0f ms", s.clip_name.c_str(), s.doc.dirty() ? " *" : "", s.pb.length_ms);
        else ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "the clip has errors:");
        if (s.clip_loaded) draw_checks(s);
        ImGui::Separator();
        const uint32_t moving = s.solved.applied.pos_parts | s.solved.applied.rot_parts;
        for (const auto& entry : s.tree) {
            const int i = entry.first;
            const Node& n = s.rig.nodes[i];
            ImGui::Indent(10.0f * entry.second);
            std::string label = n.name;
            if (n.has_part && (moving & (1u << static_cast<int>(n.part)))) label += "  *";
            if (n.approximate) label += "  ~";
            ImGui::PushID(i);
            if (ImGui::Selectable(label.c_str(), s.selected == i)) s.selected = s.selected == i ? -1 : i;
            ImGui::PopID();
            if (!s.mesh_note[i].empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.mesh_note[i].c_str());
            else if (n.approximate && ImGui::IsItemHovered())
                ImGui::SetTooltip("approximate: the parity check found it outside the tolerance of the game's matrices");
            ImGui::Unindent(10.0f * entry.second);
        }
        if (s.solved.missing_parts) {
            std::string lack;
            for (int p = 0; p < anim::kPartCount; ++p)
                if (s.solved.missing_parts & (1u << p)) lack += std::string(lack.empty() ? "" : ", ") + anim::part_name(static_cast<anim::Part>(p));
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "the rig has no node for: %s", lack.c_str());
        }
        ImGui::Separator();
        if (s.selected >= 0) {
            if (s.handles.ok) {
                ImGui::TextWrapped("selected %s: its handles edit %s", s.rig.nodes[s.selected].name.c_str(), anim::part_name(s.handles.part));
            } else {
                ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s: no part to pose (no node above it answers to one, or the rig never reads its vectors)",
                                   s.rig.nodes[s.selected].name.c_str());
            }
        } else {
            ImGui::TextDisabled("click a part, or a line here, to pose it");
        }
        ImGui::TextDisabled("* a track of the clip moves it now");
    } else {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "No rig");
        ImGui::TextWrapped("%s", s.rig_error.c_str());
        if (s.clip_loaded) draw_checks(s);
    }
    draw_pose_library(s);
    draw_sounds_panel(s);
    draw_sheet_panel(s);
    draw_ingame_panel(s);
    ImGui::End();
}

constexpr float kTimelineLabelW = 110;   // the column of row names
constexpr float kRowH = 18;
constexpr float kRulerH = 22;
constexpr float kKeyGrabPx = 6;          // a press this near a key takes it
constexpr float kKeyDragSlopPx = 3;      // a press on a key that moves less than this is a click

// Keys the clip no longer has leave the selection (the clip was edited by another hand: a pose drag, a reload).
void drop_stale_selection(Scene& s) {
    std::erase_if(s.key_sel, [&](const KeyRef& k) {
        return !s.clip_ok || k.track < 0 || static_cast<size_t>(k.track) >= s.clip.tracks.size() || k.key < 0 ||
               static_cast<size_t>(k.key) >= s.clip.tracks[k.track].keys.size();
    });
}

// The pointer on the rows of the timeline: a press on a key selects it and a drag moves the selection, a press beside the keys
// draws a box, a double click adds a key. `row_at` is the row under the pointer, `hot` the key under it.
void handle_timeline_input(Scene& s, bool hovered, int row_at, KeyRef hot) {
    ImGuiIO& io = ImGui::GetIO();
    Scene::KeyDrag& kd = s.kdrag;
    const float len = s.pb.length_ms;
    const float mx = io.MousePos.x;
    const int n_rows = static_cast<int>(s.rows.size());

    if (kd.mode == Scene::KeyDrag::None && hovered && !s.drag.active && ImGui::IsMouseClicked(0)) {
        kd = Scene::KeyDrag{};
        kd.press = io.MousePos;
        if (hot.track >= 0) {
            if (io.KeyCtrl) {
                select_toggle(s.key_sel, hot);
            } else {
                if (!selected(s.key_sel, hot)) s.key_sel = {hot};
                kd.mode = Scene::KeyDrag::Move;
                kd.grabbed_ms = s.clip.tracks[hot.track].keys[hot.key].t * len;
                kd.start_text = s.doc.text();
                kd.start_sel = s.key_sel;
                kd.first_ms = s.clip.tracks[kd.start_sel[0].track].keys[kd.start_sel[0].key].t * len;
                s.pb.pause();
                s.pb.scrub(kd.grabbed_ms);   // the view shows the key that was clicked
            }
        } else if (mx >= s.tl.x) {
            if (ImGui::IsMouseDoubleClicked(0) && row_at >= 0 && row_at < n_rows) {
                const TimelineRow& row = s.rows[row_at];
                const float at = snapped(s, s.tl.ms_at(mx));
                char buf[96];
                std::snprintf(buf, sizeof buf, "key added on %s at %.0f ms", row_label(row).c_str(), at);
                commit_key_edit(s, add_key(s.doc.text(), row.part, row.channel, at, len), buf);
            } else {
                kd.mode = Scene::KeyDrag::Box;
                kd.row0 = row_at;
                kd.ms0 = s.tl.ms_at(mx);
                kd.start_sel = io.KeyCtrl ? s.key_sel : Selection{};
                s.key_sel = kd.start_sel;
            }
        }
        return;
    }
    if (kd.mode == Scene::KeyDrag::None) return;

    if (!ImGui::IsMouseDown(0)) {
        if (kd.mode == Scene::KeyDrag::Move) {
            if (kd.last.ok && kd.last.text != kd.start_text) {
                const float by = kd.last.clip.tracks[kd.last.selection[0].track].keys[kd.last.selection[0].key].t * len -
                                 kd.first_ms;
                char buf[128];
                std::snprintf(buf, sizeof buf, "%s moved by %.0f ms", count_of(kd.last.selection.size(), "key").c_str(), by);
                std::string what = buf;
                if (kd.last.replaced) what += ", " + std::to_string(kd.last.replaced) + " replaced";
                commit_key_edit(s, std::move(kd.last), what);
            } else if (kd.moved) {
                reparse_clip(s);   // the drag came back to where it began
                s.key_sel = kd.start_sel;
            }
        }
        kd = Scene::KeyDrag{};
        return;
    }
    if (kd.mode != Scene::KeyDrag::Dead && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (kd.mode == Scene::KeyDrag::Move) {
            reparse_clip(s);
            s.status = "move cancelled";
        }
        s.key_sel = kd.start_sel;
        kd.mode = Scene::KeyDrag::Dead;
        return;
    }
    if (kd.mode == Scene::KeyDrag::Move) {
        const float dx = mx - kd.press.x;
        if (!kd.moved && std::fabs(dx) < kKeyDragSlopPx) return;
        kd.moved = true;
        // The key under the pointer goes to the grid; the others keep their distance to it.
        const float target = snapped(s, kd.grabbed_ms + dx / s.tl.px_per_ms());
        KeyEditResult r = move_keys(kd.start_text, kd.start_sel, target - kd.grabbed_ms, len);
        if (!r.ok) {
            s.status = r.error;
            return;
        }
        s.clip = r.clip;   // the view and the rows show it at once; the document is not touched until the release
        s.key_sel = r.selection;
        kd.last = std::move(r);
    } else if (kd.mode == Scene::KeyDrag::Box) {
        s.key_sel = kd.start_sel;
        for (const KeyRef& k : keys_in_box(s.clip, s.rows, kd.row0, row_at, kd.ms0, s.tl.ms_at(mx), len)) select_add(s.key_sel, k);
    }
}

// The play controls and the timeline: the ruler (a press on it scrubs), a row per part and channel, the hide row and the marker row.
void draw_timeline(Scene& s, float top, float width, float height) {
    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::Begin("Timeline", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGuiIO& io = ImGui::GetIO();
    const bool has_clip = s.clip_ok && s.pb.length_ms > 0;
    const float len = s.pb.length_ms;

    ImGui::BeginDisabled(!has_clip);
    if (ImGui::Button(s.pb.playing ? "Pause" : "Play", ImVec2(70, 0))) toggle_play(s);
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &s.pb.loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    float speed = s.pb.speed;
    if (ImGui::SliderFloat("Speed", &speed, kMinSpeed, kMaxSpeed, "%.1fx")) s.pb.set_speed(speed);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    if (ImGui::InputFloat("Ability length (ms)", &s.ability_ms, 0, 0, "%.0f")) {
        s.ability_ms = std::clamp(s.ability_ms, 1.0f, 600000.0f);
        s.ability_set = true;
        apply_clip_length(s);   // only a clip with `duration = "ability"` follows it
    }
    ImGui::SameLine(0, 20);
    ImGui::Checkbox("Snap", &s.snap_on);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(50);
    if (ImGui::InputFloat("ms##snap", &s.snap_step, 0, 0, "%.0f")) s.snap_step = std::clamp(s.snap_step, 1.0f, 1000.0f);
    ImGui::SameLine(0, 20);
    ImGui::BeginDisabled(!has_clip);
    const float mid = s.tl.x + s.tl.width * 0.5f;
    if (ImGui::Button("-##zoom")) s.tl.zoom_at(mid, 0.5f);
    ImGui::SameLine();
    if (ImGui::Button("+##zoom")) s.tl.zoom_at(mid, 2.0f);
    ImGui::SameLine();
    if (ImGui::Button("Fit")) s.tl.zoom = 1;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ctrl+wheel zooms, Shift+wheel or a middle drag pans");
    ImGui::SameLine(0, 20);
    ImGui::Text("%.0f / %.0f ms", s.pb.time_ms, len);
    if (!s.key_sel.empty()) {
        ImGui::SameLine(0, 20);
        ImGui::TextDisabled("%s selected", count_of(s.key_sel.size(), "key").c_str());
    }

    drop_stale_selection(s);
    s.rows = has_clip ? timeline_rows(s.clip) : std::vector<TimelineRow>{};
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float key_w = std::max(ImGui::GetContentRegionAvail().x - kTimelineLabelW - ImGui::GetStyle().ScrollbarSize - 8.0f, 50.0f);
    s.tl.fit(origin.x + kTimelineLabelW, key_w, len);
    const float key_x0 = s.tl.x, key_x1 = s.tl.x + s.tl.width;
    const ImU32 col_text = ImGui::GetColorU32(ImGuiCol_Text), col_dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 col_scrub = IM_COL32(235, 90, 90, 255);

    // The ruler.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##ruler", ImVec2(kTimelineLabelW + key_w, kRulerH));
    const bool ruler_hovered = ImGui::IsItemHovered();
    if (has_clip && ImGui::IsItemActive() && !s.drag.active) {
        s.pb.pause();
        s.pb.scrub(snapped(s, s.tl.ms_at(io.MousePos.x)));
    }
    dl->AddRectFilled(ImVec2(key_x0, origin.y), ImVec2(key_x1, origin.y + kRulerH), IM_COL32(48, 50, 58, 255));
    dl->AddText(ImVec2(origin.x + 4, origin.y + 3), col_dim, "ms");
    if (has_clip) {
        dl->PushClipRect(ImVec2(key_x0 - 1, origin.y), ImVec2(key_x1 + 1, origin.y + kRulerH), true);
        const float step = tick_step_ms(s.tl.px_per_ms(), 70.0f);
        for (float t = std::floor(s.tl.start_ms / step) * step; t <= s.tl.start_ms + s.tl.visible_ms() + 0.5f; t += step) {
            const float x = s.tl.x_of(t);
            dl->AddLine(ImVec2(x, origin.y + kRulerH - 7), ImVec2(x, origin.y + kRulerH), col_dim);
            char buf[24];
            std::snprintf(buf, sizeof buf, "%.0f", t);
            dl->AddText(ImVec2(x + 3, origin.y + 2), col_dim, buf);
        }
        const float sx = s.tl.x_of(s.pb.time_ms);
        dl->AddTriangleFilled(ImVec2(sx - 5, origin.y + kRulerH - 9), ImVec2(sx + 5, origin.y + kRulerH - 9), ImVec2(sx, origin.y + kRulerH),
                              col_scrub);
        dl->PopClipRect();
    }

    // The rows. With Ctrl or Shift held the wheel is the timeline's (zoom, pan); else it scrolls the rows.
    ImGui::BeginChild("##rows", ImVec2(0, 0), 0, io.KeyCtrl || io.KeyShift ? ImGuiWindowFlags_NoScrollWithMouse : 0);
    if (!has_clip) {
        ImGui::TextDisabled("%s", s.clip_loaded ? "the clip has errors: nothing to show" : "no clip");
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    const ImVec2 ro = ImGui::GetCursorScreenPos();
    int n_rows = static_cast<int>(s.rows.size());
    const float total_h = static_cast<float>(n_rows + 2) * kRowH;
    ImGui::InvisibleButton("##keys", ImVec2(kTimelineLabelW + key_w, total_h));
    const bool hovered = ImGui::IsItemHovered();
    const float mx = io.MousePos.x;
    const int row_at = static_cast<int>(std::floor((io.MousePos.y - ro.y) / kRowH));
    KeyRef hot;
    if (hovered && s.kdrag.mode == Scene::KeyDrag::None && row_at >= 0 && row_at < n_rows && s.tl.px_per_ms() > 0)
        hot = key_near(s.clip, s.rows[row_at], s.tl.ms_at(mx), kKeyGrabPx / s.tl.px_per_ms(), len);
    handle_timeline_input(s, hovered, row_at, hot);
    if ((hovered || ruler_hovered) && io.MouseWheel != 0) {
        if (io.KeyCtrl) s.tl.zoom_at(mx, io.MouseWheel > 0 ? 1.25f : 0.8f);
        else if (io.KeyShift) s.tl.pan_px(io.MouseWheel * 40.0f);
    }
    if ((hovered || ruler_hovered) && ImGui::IsMouseDragging(2, 0.0f)) s.tl.pan_px(io.MouseDelta.x);

    // The input may have changed the clip: the rows are of the clip as it is now.
    s.rows = timeline_rows(s.clip);
    n_rows = static_cast<int>(s.rows.size());
    ImDrawList* rdl = ImGui::GetWindowDrawList();
    const float bottom = ro.y + std::max(static_cast<float>(n_rows + 2) * kRowH, ImGui::GetWindowHeight());
    for (int r = 0; r < n_rows + 2; ++r) {
        const float y0 = ro.y + static_cast<float>(r) * kRowH;
        if (r % 2) rdl->AddRectFilled(ImVec2(ro.x, y0), ImVec2(key_x1, y0 + kRowH), IM_COL32(255, 255, 255, 10));
        const std::string label = r < n_rows ? row_label(s.rows[r]) : r == n_rows ? "hide" : "markers";
        const bool lit = r < n_rows && s.handles.ok && s.rows[r].part == s.handles.part;
        rdl->AddText(ImVec2(ro.x + 4, y0 + 1), lit ? col_text : col_dim, label.c_str());
    }
    rdl->PushClipRect(ImVec2(key_x0 - 8, ro.y), ImVec2(key_x1 + 8, bottom), true);
    for (int r = 0; r < n_rows; ++r) {
        const float cy = ro.y + (static_cast<float>(r) + 0.5f) * kRowH;
        const TimelineRow& row = s.rows[r];
        if (row.keys.size() > 1) {
            const float xa = s.tl.x_of(s.clip.tracks[row.keys.front().track].keys[row.keys.front().key].t * len);
            const float xb = s.tl.x_of(s.clip.tracks[row.keys.back().track].keys[row.keys.back().key].t * len);
            rdl->AddLine(ImVec2(xa, cy), ImVec2(xb, cy), IM_COL32(130, 132, 145, 140));
        }
        for (const KeyRef& k : row.keys) {
            const float x = s.tl.x_of(s.clip.tracks[k.track].keys[k.key].t * len);
            const bool sel = selected(s.key_sel, k);
            const float h = sel || k == hot ? 6.0f : 5.0f;
            const ImU32 col = sel ? IM_COL32(255, 210, 70, 255) : k == hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(185, 188, 200, 255);
            rdl->AddQuadFilled(ImVec2(x, cy - h), ImVec2(x + h, cy), ImVec2(x, cy + h), ImVec2(x - h, cy), col);
        }
    }
    {
        const float y0 = ro.y + static_cast<float>(n_rows) * kRowH;
        for (const anim::Hide& h : s.clip.hides) {
            std::string names;
            for (uint32_t bit = 1; bit <= anim::kHideWeapons; bit <<= 1)
                if (h.groups & bit) names += std::string(names.empty() ? "" : ", ") + anim::hide_group_name(bit);
            const float xa = s.tl.x_of(h.from * len), xb = s.tl.x_of(h.to * len);
            rdl->AddRectFilled(ImVec2(xa, y0 + 3), ImVec2(std::max(xb, xa + 2), y0 + kRowH - 3), IM_COL32(150, 110, 205, 170));
            rdl->AddText(ImVec2(xa + 4, y0 + 1), col_text, names.c_str());
        }
        const float y1 = y0 + kRowH;
        for (const anim::Marker& m : s.clip.markers) {
            const float x = s.tl.x_of(m.t * len);
            rdl->AddTriangleFilled(ImVec2(x - 5, y1 + 3), ImVec2(x + 5, y1 + 3), ImVec2(x, y1 + 12), IM_COL32(240, 160, 70, 255));
            rdl->AddText(ImVec2(x + 8, y1 + 1), col_text, m.name.c_str());
        }
    }
    const float sx = s.tl.x_of(s.pb.time_ms);
    rdl->AddLine(ImVec2(sx, ro.y), ImVec2(sx, bottom), col_scrub);
    rdl->PopClipRect();
    if (s.kdrag.mode == Scene::KeyDrag::Box) {
        rdl->AddRectFilled(s.kdrag.press, io.MousePos, IM_COL32(120, 160, 255, 40));
        rdl->AddRect(s.kdrag.press, io.MousePos, IM_COL32(120, 160, 255, 200));
    }
    if (hot.track >= 0 && s.kdrag.mode == Scene::KeyDrag::None && static_cast<size_t>(hot.track) < s.clip.tracks.size() &&
        static_cast<size_t>(hot.key) < s.clip.tracks[hot.track].keys.size()) {
        const anim::Key& k = s.clip.tracks[hot.track].keys[hot.key];
        ImGui::SetTooltip("%.0f ms  [%.3g, %.3g, %.3g]  %s", k.t * len, k.v[0], k.v[1], k.v[2], anim::ease_name(k.ease));
    }
    ImGui::EndChild();
    ImGui::End();
}

// The buttons over the corner of the view, the rig's name and the key to the axes.
void draw_view_overlay(Scene& s, const Viewport& vp) {
    ImGui::SetNextWindowPos(ImVec2(vp.x + 8, vp.y + vp.h - 8), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::Begin("##viewbuttons", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    struct Choice { const char* label; ViewPreset preset; };
    const Choice choices[] = {{"Front", ViewPreset::Front}, {"Left", ViewPreset::Left}, {"Free", ViewPreset::Free}};
    for (size_t i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        const bool on = s.preset == choices[i].preset;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(choices[i].label)) pick_preset(s, choices[i].preset);
        if (on) ImGui::PopStyleColor();
    }
    ImGui::End();

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (s.rig_ok) {
        std::string text = s.rig.name;
        if (s.clip_ok) text += "  |  " + s.clip_name;
        if (!s.rig.emitters.empty()) text += "  |  " + std::to_string(s.spark_count) + " particles";
        if (!s.rig.shockwaves.empty()) text += "  |  " + std::to_string(s.ring_count) + " rings";
        if (!s.rig.pillars.empty()) text += "  |  " + std::to_string(s.pillar_count) + " pillars";
        if (!s.rig.sounds.empty())
            text += "  |  " + (!s.sound_on ? std::string("sounds off") : !s.audio.ok() ? std::string("silent") :
                               std::to_string(s.audio.playing()) + " sounds");
        ImVec2 sz = ImGui::CalcTextSize(text.c_str());
        dl->AddText(ImVec2(vp.x + vp.w - sz.x - 10, vp.y + 8), IM_COL32(200, 200, 205, 255), text.c_str());
    }
    if (!s.status.empty())
        dl->AddText(ImVec2(vp.x + 10, vp.y + 8), IM_COL32(230, 220, 150, 255), s.status.c_str());
    {
        // What the figure stands on, when it is not the neutral pose: a baked pose, or the pose the library lacks.
        std::string note;
        bool warn = false;
        const int shown_id = panel_pose_id(s);
        if (s.show_pose && shown_id >= 0) {
            if (s.lib.find(shown_id)) {
                char b[96];
                std::snprintf(b, sizeof b, "showing pose %d at %.0f ms: the clip's tracks are not drawn", shown_id, s.show_ms);
                note = b;
            } else {
                note = "pose " + std::to_string(shown_id) + " is not in the pose library";
                warn = true;
            }
        } else {
            note = base_label(s.base);
            warn = s.base.kind == BaseResult::Kind::Missing || s.base.past_end;
        }
        if (!note.empty())
            dl->AddText(ImVec2(vp.x + 10, vp.y + 26), warn ? IM_COL32(255, 180, 80, 255) : IM_COL32(150, 210, 230, 255), note.c_str());
        if (!s.note.empty() && s.status == s.note_for) dl->AddText(ImVec2(vp.x + 10, vp.y + 44), IM_COL32(230, 220, 150, 255), s.note.c_str());
    }
    if (s.show_axes) {
        struct Axis { Vec3 end; const char* label; ImU32 color; };
        const Axis axes[] = {{{kTriadLength + 3, 0, 0}, "x", IM_COL32(224, 80, 80, 255)},
                             {{0, kTriadLength + 3, 0}, "y", IM_COL32(80, 208, 96, 255)},
                             {{0, 0, kTriadLength + 3}, "z", IM_COL32(80, 144, 240, 255)}};
        for (const Axis& a : axes) {
            float px, py;
            if (project(s.cam, vp, a.end, px, py)) dl->AddText(ImVec2(px, py), a.color, a.label);
        }
    }
}

// What the three axes are called in a message.
const char* axis_name(int a) { return a == 0 ? "x" : a == 1 ? "y" : "z"; }

// ---------------------------------------------------------------------------------------------------------------------------------
// The key inspector and the curve view (6.6). The edits are keyinspector.h's; this is the window.

// The keys of the selection that the clip has now.
Selection live_selection(const Scene& s) {
    Selection out;
    if (!s.clip_ok) return out;
    for (const KeyRef& k : s.key_sel)
        if (k.track >= 0 && static_cast<size_t>(k.track) < s.clip.tracks.size() && k.key >= 0 &&
            static_cast<size_t>(k.key) < s.clip.tracks[k.track].keys.size())
            out.push_back(k);
    return out;
}

// A slider or a dot of the curve is held: every move edits the clip as it was at the press, and the release makes one undo step.
void insp_begin(Scene& s, KeyRef key, int axis, const float v0[3]) {
    s.insp.active = true;
    s.insp.start_text = s.doc.text();
    s.insp.key = key;
    s.insp.axis = axis;
    for (int a = 0; a < 3; ++a) s.insp.v0[a] = v0[a];
    s.insp.last = KeyEditResult{};
}

void insp_preview(Scene& s, const float v[3]) {
    KeyEditResult r = set_key_vec(s.insp.start_text, s.insp.key, v);
    if (!r.ok) {
        s.status = r.error;
        return;
    }
    s.clip = r.clip;   // the view shows it at once; the document is not touched until the release
    s.insp.last = std::move(r);
}

void insp_end(Scene& s, bool commit) {
    s.insp.active = false;
    KeyEditResult r = std::move(s.insp.last);
    s.insp.last = KeyEditResult{};
    if (commit && r.ok && r.text != s.insp.start_text) {
        const KeyRef k = s.insp.key;
        s.doc.set_text(r.text);
        s.clip = std::move(r.clip);
        s.key_sel = {k};
        const anim::Track& tr = s.clip.tracks[k.track];
        const anim::Key& key = tr.keys[k.key];
        char buf[200];
        std::snprintf(buf, sizeof buf, "%s.%s key %d: %.3g, %.3g, %.3g", anim::part_name(tr.part), anim::channel_name(tr.channel),
                      k.key + 1, key.v[0], key.v[1], key.v[2]);
        s.status = buf;
    } else {
        reparse_clip(s);   // a cancelled edit, or one that came back to where it began: the clip is the document's again
        if (!commit) s.status = "edit cancelled";
    }
}

// The ease of the selected keys, picked from the list. Every selected key gets it.
void draw_ease_combo(Scene& s, const Selection& live, const char* label) {
    static const anim::Ease kEases[] = {anim::Ease::Linear, anim::Ease::Smooth, anim::Ease::EaseIn, anim::Ease::EaseOut,
                                        anim::Ease::Hold};
    const anim::Ease first = s.clip.tracks[live[0].track].keys[live[0].key].ease;
    bool mixed = false;
    for (const KeyRef& k : live) mixed = mixed || s.clip.tracks[k.track].keys[k.key].ease != first;
    ImGui::BeginDisabled(!can_edit_keys(s));
    ImGui::SetNextItemWidth(130);
    if (ImGui::BeginCombo(label, mixed ? "mixed" : anim::ease_name(first))) {
        for (anim::Ease e : kEases) {
            if (!ImGui::Selectable(anim::ease_name(e), !mixed && e == first)) continue;
            const std::string what = std::string("ease ") + anim::ease_name(e) + " on " + count_of(live.size(), "key");
            commit_key_edit(s, set_keys_ease(s.doc.text(), live, e), what);
            break;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}

void draw_inspector(Scene& s, float x, float top, float w, float h) {
    ImGui::SetNextWindowPos(ImVec2(x, top));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("Key", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    // The item that held an edit is gone (the window was left, the selection changed): the edit ends as a release.
    if (s.insp.active && !ImGui::IsAnyItemActive()) insp_end(s, true);
    if (!s.clip_ok || !(s.pb.length_ms > 0)) {
        ImGui::TextDisabled("open a clip to edit its keys");
        ImGui::End();
        return;
    }
    const float len = s.pb.length_ms;
    const Selection live = live_selection(s);
    anim::Part mirror_from = anim::Part::Root;
    bool have_part = false;
    if (!live.empty()) {
        mirror_from = s.clip.tracks[live[0].track].part;
        have_part = true;
    } else if (s.handles.ok) {
        mirror_from = s.handles.part;
        have_part = true;
    }

    if (live.empty()) {
        ImGui::TextDisabled("no key selected");
        ImGui::TextWrapped("Click a key in the timeline or on the curve.");
    } else if (live.size() > 1) {
        ImGui::Text("%s selected", count_of(live.size(), "key").c_str());
        draw_ease_combo(s, live, "ease");
    } else {
        const KeyRef ref = live[0];
        const anim::Track tr = s.clip.tracks[ref.track];   // a copy: an edit below replaces the clip
        const anim::Key kc = tr.keys[ref.key];
        const bool pos = tr.channel == anim::Channel::Pos;
        ImGui::Text("%s.%s (%s)  key %d of %d", anim::part_name(tr.part), anim::channel_name(tr.channel),
                    tr.mode == anim::TrackMode::Set ? "set" : "add", ref.key + 1, static_cast<int>(tr.keys.size()));
        const bool idle = idle_for_keys(s);

        ImGui::BeginDisabled(!can_edit_keys(s));
        float t_ms = kc.t * len;
        ImGui::SetNextItemWidth(130);
        if (ImGui::InputFloat("t (ms)", &t_ms, 0, 0, "%.1f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "key moved to %.1f ms", t_ms);
            commit_key_edit(s, move_keys(s.doc.text(), {ref}, t_ms - kc.t * len, len), buf);
        }
        ImGui::EndDisabled();
        draw_ease_combo(s, live, "ease");

        // Sliders: the track's own range is the loader's, and a typed value (Ctrl+click) may go to it.
        ImGui::BeginDisabled(!idle);
        const float lim = pos ? 40.0f : 360.0f;
        float v[3] = {kc.v[0], kc.v[1], kc.v[2]};
        for (int a = 0; a < 3; ++a) {
            ImGui::PushID(a);
            const bool changed = ImGui::SliderFloat(axis_name(a), &v[a], -lim, lim, "%.2f");
            if (ImGui::IsItemActivated() && !s.insp.active) insp_begin(s, ref, -1, kc.v);
            if (changed && s.insp.active) insp_preview(s, v);
            if (ImGui::IsItemDeactivated() && s.insp.active && s.insp.axis < 0) insp_end(s, true);
            ImGui::PopID();
        }
        ImGui::EndDisabled();

        // Numeric entry of the whole vector: applied at Enter.
        ImGui::BeginDisabled(!can_edit_keys(s));
        float nv[3] = {kc.v[0], kc.v[1], kc.v[2]};
        ImGui::SetNextItemWidth(220);
        if (ImGui::InputFloat3("value", nv, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue))
            commit_key_edit(s, set_key_vec(s.doc.text(), ref, nv), "value entered");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Ctrl+click a slider to type; limit %.0f", value_limit(tr.channel));
    }

    ImGui::Separator();
    ImGui::BeginDisabled(live.empty() || !can_edit_keys(s));
    if (ImGui::Button("Reset to neutral")) {
        const size_t n = live.size();
        commit_key_edit(s, reset_keys(s.doc.text(), live, anim::Pose{}), count_of(n, "key") + " reset to neutral");
    }
    ImGui::EndDisabled();

    anim::Part partner = anim::Part::Root;
    const bool can_mirror = have_part && mirror_partner(mirror_from, partner);
    ImGui::BeginDisabled(!can_mirror || !can_edit_keys(s));
    const std::string label = can_mirror ? std::string("Mirror L/R: ") + anim::part_name(mirror_from) + " onto " +
                                               anim::part_name(partner)
                                         : std::string("Mirror L/R");
    if (ImGui::Button(label.c_str())) {
        KeyEditResult r = mirror_part(s.doc.text(), mirror_from);
        std::string what = std::string(anim::part_name(mirror_from)) + " mirrored onto " + anim::part_name(partner) + ": " +
                           count_of(static_cast<size_t>(r.made_tracks), "track") + " written";
        if (r.replaced) what += ", " + count_of(static_cast<size_t>(r.replaced), "key") + " replaced";
        commit_key_edit(s, std::move(r), what);
    }
    ImGui::EndDisabled();
    if (!have_part) ImGui::TextDisabled("select a key or a part to mirror it");
    else if (!can_mirror) ImGui::TextDisabled("%s has no left or right pair", anim::part_name(mirror_from));
    ImGui::End();
}

void draw_curve(Scene& s, float x, float top, float w, float h) {
    ImGui::SetNextWindowPos(ImVec2(x, top));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("Curve", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    const Selection live = live_selection(s);
    if (live.empty() || !(s.pb.length_ms > 0)) {
        ImGui::TextDisabled("select a key to see its track");
        ImGui::End();
        return;
    }
    const float len = s.pb.length_ms;
    const int ti = live[0].track;
    const anim::Track tr = s.clip.tracks[ti];   // a copy: an edit of the ease replaces the clip
    ImGui::Text("%s.%s", anim::part_name(tr.part), anim::channel_name(tr.channel));
    ImGui::SameLine();
    draw_ease_combo(s, live, "ease##curve");

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 40 || avail.y < 40) {
        ImGui::End();
        return;
    }
    const ImVec2 p1(p0.x + avail.x, p0.y + avail.y);
    constexpr float kPad = 8.0f;
    CurveRange range = curve_range(tr, len);
    if (s.insp.active && s.insp.axis >= 0) range = {s.insp.range_lo, s.insp.range_hi};   // held: the scale stays
    const float span = std::max(range.hi - range.lo, 1e-3f);
    auto px = [&](float u) { return p0.x + kPad + u * (avail.x - 2 * kPad); };
    auto py = [&](float v) { return p1.y - kPad - (v - range.lo) / span * (avail.y - 2 * kPad); };
    auto value_at = [&](float y) { return range.lo + (p1.y - kPad - y) / (avail.y - 2 * kPad) * span; };

    ImGui::InvisibleButton("##curvearea", avail);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(24, 26, 30, 255));
    if (range.lo < 0 && range.hi > 0) dl->AddLine(ImVec2(p0.x, py(0)), ImVec2(p1.x, py(0)), IM_COL32(90, 92, 100, 255));
    char lab[32];
    std::snprintf(lab, sizeof lab, "%.0f", range.hi);
    dl->AddText(ImVec2(p0.x + 3, p0.y + 1), IM_COL32(150, 150, 158, 255), lab);
    std::snprintf(lab, sizeof lab, "%.0f", range.lo);
    dl->AddText(ImVec2(p0.x + 3, p1.y - 15), IM_COL32(150, 150, 158, 255), lab);

    const ImU32 colors[3] = {IM_COL32(224, 80, 80, 255), IM_COL32(80, 208, 96, 255), IM_COL32(80, 144, 240, 255)};
    const int n = std::clamp(static_cast<int>(avail.x / 3), 16, 256);
    std::vector<float> ys;
    std::vector<ImVec2> pts;
    for (int a = 0; a < 3; ++a) {
        curve_samples(tr, a, n, ys, len);
        pts.clear();
        for (int i = 0; i <= n; ++i) pts.push_back(ImVec2(px(static_cast<float>(i) / static_cast<float>(n)), py(ys[i])));
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), colors[a], ImDrawFlags_None, 1.5f);
    }
    dl->AddLine(ImVec2(px(s.pb.time_ms / len), p0.y), ImVec2(px(s.pb.time_ms / len), p1.y), IM_COL32(230, 220, 150, 200));

    // The dots: one per key and component. A selected key has its time marked and bigger dots.
    struct Dot {
        int key, axis;
        ImVec2 at;
    };
    std::vector<Dot> dots;
    for (size_t k = 0; k < tr.keys.size(); ++k) {
        const bool sel = selected(s.key_sel, {ti, static_cast<int>(k)});
        const float kx = px(tr.keys[k].t);
        if (sel) dl->AddLine(ImVec2(kx, p0.y), ImVec2(kx, p1.y), IM_COL32(255, 255, 255, 70));
        for (int a = 0; a < 3; ++a) {
            const ImVec2 at(kx, py(tr.keys[k].v[a]));
            dots.push_back({static_cast<int>(k), a, at});
            dl->AddCircleFilled(at, sel ? 5.0f : 3.5f, colors[a]);
            if (sel) dl->AddCircle(at, 6.5f, IM_COL32(255, 255, 255, 255));
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated() && can_edit_keys(s)) {
        // A press on a dot selects its key and holds the dot; a press elsewhere scrubs.
        int best = -1;
        float best_d = 10.0f * 10.0f;
        for (size_t i = 0; i < dots.size(); ++i) {
            const float dx = dots[i].at.x - io.MousePos.x, dy = dots[i].at.y - io.MousePos.y;
            if (dx * dx + dy * dy < best_d) {
                best_d = dx * dx + dy * dy;
                best = static_cast<int>(i);
            }
        }
        if (best >= 0) {
            const KeyRef k{ti, dots[best].key};
            if (io.KeyCtrl) {
                select_toggle(s.key_sel, k);
            } else {
                if (!selected(s.key_sel, k)) s.key_sel = {k};
                s.pb.pause();
                s.pb.scrub(tr.keys[k.key].t * len);
                insp_begin(s, k, dots[best].axis, tr.keys[k.key].v);
                s.insp.grab_dy = dots[best].at.y - io.MousePos.y;
                s.insp.range_lo = range.lo;
                s.insp.range_hi = range.hi;
            }
        } else {
            s.pb.pause();
            s.pb.scrub(snapped(s, std::clamp((io.MousePos.x - p0.x - kPad) / (avail.x - 2 * kPad), 0.0f, 1.0f) * len));
        }
    }
    if (s.insp.active && s.insp.axis >= 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            insp_end(s, false);
        } else if (ImGui::IsItemActive()) {
            float v[3] = {s.insp.v0[0], s.insp.v0[1], s.insp.v0[2]};
            const float lim = value_limit(tr.channel);
            const float nv = std::clamp(value_at(io.MousePos.y + s.insp.grab_dy), -lim, lim);
            if (std::fabs(nv - v[s.insp.axis]) > 1e-4f) {
                v[s.insp.axis] = nv;
                insp_preview(s, v);
            }
        } else {
            insp_end(s, true);
        }
    }
    ImGui::End();
}

constexpr float kClickSlopPx = 4.0f;   // a press that moves less than this and is let go is a click; more orbits the view

// True when a key of the selected part's tracks is at the scrub time.
bool key_near_scrub(const Scene& s) {
    for (const anim::Track& tr : s.clip.tracks) {
        if (tr.part != s.handles.part) continue;
        for (const anim::Key& k : tr.keys)
            if (std::fabs(k.t * s.pb.length_ms - s.pb.time_ms) < kKeyEpsilonMs) return true;
    }
    return false;
}

// A press on a handle: from now on every move edits the clip as it was at the press, and the release makes one undo step.
void begin_pose_drag(Scene& s, const Viewport& vp, const ImVec2& mouse) {
    if (!s.clip_ok || !(s.pb.length_ms > 0)) {
        s.status = "open a clip to pose it";
        return;
    }
    if (s.show_pose) {
        s.status = "Show is on: the clip's tracks are not drawn; turn it off to pose";
        return;
    }
    const Ray ray = ray_through_pixel(s.cam, vp, mouse.x, mouse.y);
    if (!s.drag.math.begin(s.handles, s.hover.kind, s.hover.axis, ray)) {
        s.status = "that handle cannot be dragged from this view angle";
        return;
    }
    s.pb.pause();   // the key is written at the scrub time: it must hold still
    // A time left by playing goes to the grid, so the key lands where the timeline can put the scrub time again. A key within
    // half a millisecond is left alone: a click on a key in the timeline scrubs to it, on the grid or off.
    const float on_grid = snapped(s, s.pb.time_ms);
    if (!key_near_scrub(s) && on_grid != s.pb.time_ms) s.pb.scrub(on_grid);
    s.drag.active = true;
    s.drag.kind = s.hover.kind;
    s.drag.axis = s.hover.axis;
    s.drag.start_text = s.doc.text();
    s.drag.last = PoseEditResult{};
    s.drag.edit = PoseEdit{};
    s.drag.edit.part = s.handles.part;
    s.drag.edit.channel = s.drag.kind == HandleKind::Turn ? anim::Channel::Rot : anim::Channel::Pos;
    s.drag.edit.axis = s.drag.axis;
    s.drag.edit.t_ms = s.pb.time_ms;
    s.drag.edit.duration_ms = s.pb.length_ms;
    s.drag.edit.weight = anim::blend_weight(s.clip, view_timing(s.pb));   // the scrub time is fixed for the whole drag
}

void update_pose_drag(Scene& s, const Viewport& vp, const ImVec2& mouse) {
    const Ray ray = ray_through_pixel(s.cam, vp, mouse.x, mouse.y);
    if (!s.drag.math.update(ray)) return;
    s.drag.edit.delta = s.drag.math.delta();
    PoseEditResult r = edit_pose_key(s.drag.start_text, s.drag.edit);
    if (!r.ok) {
        s.status = r.error;
        return;
    }
    s.clip = r.clip;   // the view shows it at once; the document is not touched until the release
    s.drag.last = std::move(r);
}

void end_pose_drag(Scene& s, bool commit) {
    s.drag.math.end();
    s.drag.active = false;
    const PoseEditResult& r = s.drag.last;
    if (commit && r.ok && r.text != s.drag.start_text) {
        s.doc.set_text(r.text);
        s.clip = r.clip;
        s.key_sel = {{r.track, r.key}};   // the key the drag wrote is the one selected in the timeline
        char buf[200];
        std::snprintf(buf, sizeof buf, "%s %s %s: %.3g to %.3g at %.0f ms (%s)", anim::part_name(s.drag.edit.part),
                      s.drag.edit.channel == anim::Channel::Rot ? "rot" : "pos", axis_name(s.drag.edit.axis), r.before, r.after,
                      s.drag.edit.t_ms, r.made_track ? "new track" : r.made_key ? "new key" : "key changed");
        s.status = buf;
    } else {
        reparse_clip(s);   // a cancelled drag, or one that came back to where it began: the clip is the document's again
        if (!commit) s.status = "drag cancelled";
    }
}

// Orbit, pan and zoom in the view, when the pointer is over it and no panel wants it. A left press on a handle poses the part; on a
// part selects it (a click) or orbits (a drag); right or middle drag pans.
void handle_view_input(Scene& s, const Viewport& vp) {
    static int drag_button = -1;
    static bool orbiting = false;   // the left press has gone past the click slop
    static bool cancelled = false;  // Escape ended a pose drag: the rest of that press does nothing
    static ImVec2 press;
    ImGuiIO& io = ImGui::GetIO();
    const bool inside = io.MousePos.x >= vp.x && io.MousePos.x < vp.x + vp.w && io.MousePos.y >= vp.y && io.MousePos.y < vp.y + vp.h;
    const bool free_pointer = inside && !io.WantCaptureMouse;

    s.hover = HandleHit{};
    if (!s.drag.active && free_pointer && drag_button < 0 && s.rig_ok && s.handles.ok)
        s.hover = pick_handle(s.cam, vp, s.handles, io.MousePos.x, io.MousePos.y);
    if (s.hover.hit) {
        const char* what = s.hover.kind == HandleKind::Turn ? "turn" : "move";
        ImGui::SetTooltip("%s %s %s", what, anim::part_name(s.handles.part), axis_name(s.hover.axis));
    }

    if (drag_button < 0 && free_pointer) {
        for (int b = 0; b < 3; ++b) {
            if (!ImGui::IsMouseClicked(b)) continue;
            drag_button = b;
            orbiting = false;
            cancelled = false;
            press = io.MousePos;
            if (b == 0 && s.hover.hit) begin_pose_drag(s, vp, io.MousePos);
            break;
        }
    }
    if (drag_button >= 0) {
        if (ImGui::IsMouseDown(drag_button)) {
            if (drag_button == 0 && s.drag.active) {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    end_pose_drag(s, false);
                    cancelled = true;
                } else {
                    update_pose_drag(s, vp, io.MousePos);
                }
            } else if (drag_button == 0 && !cancelled) {
                const float dx = io.MousePos.x - press.x, dy = io.MousePos.y - press.y;
                if (!orbiting && dx * dx + dy * dy > kClickSlopPx * kClickSlopPx) {
                    orbiting = true;
                    s.cam.orbit(dx, dy);   // the part of the drag that was held back
                    s.preset = ViewPreset::Free;
                } else if (orbiting) {
                    s.cam.orbit(io.MouseDelta.x, io.MouseDelta.y);
                    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) s.preset = ViewPreset::Free;
                }
            } else if (drag_button != 0) {
                s.cam.pan(io.MouseDelta.x, io.MouseDelta.y, vp.h);
            }
        } else {
            if (drag_button == 0) {
                if (s.drag.active) {
                    end_pose_drag(s, true);
                } else if (!orbiting && !cancelled && s.rig_ok) {
                    // A click: the nearest part under the pointer, or none (a click on the empty view clears the selection).
                    const Ray ray = ray_through_pixel(s.cam, vp, io.MousePos.x, io.MousePos.y);
                    s.selected = pick_node(s.rig, s.solved.world, s.boxes, ray);
                }
            }
            drag_button = -1;
        }
    }
    if (inside && !io.WantCaptureMouse && io.MouseWheel != 0) s.cam.zoom(io.MouseWheel);
    if (!io.WantTextInput && s.modal == Scene::Modal::None) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) toggle_play(s);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) do_undo(s);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && s.doc.dirty() && s.clip_ok) do_save(s);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) do_copy_keys(s);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) do_paste_keys(s);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) do_delete_keys(s);
    }
}

}  // namespace

int run_app(const AppOptions& options) {
    Scene scene;
    scene.models_dir = options.models_dir;
    scene.cam.yaw_deg = 30;
    scene.cam.pitch_deg = 15;
    load_rig(scene, options.rig_path);
    if (scene.rig_ok) build_meshes(scene);
    {
        std::string folder = options.library_dir, text;
        if (folder.empty()) {
            const std::string sp = settings_path();
            if (!sp.empty() && read_file(sp, text)) folder = parse_settings(text).library_folder;
        }
        {
            const std::string sp = settings_path();
            std::string game_text;
            if (!sp.empty() && read_file(sp, game_text)) {
                const Settings st = parse_settings(game_text);
                std::snprintf(scene.game_buf, sizeof scene.game_buf, "%s", st.game_folder.c_str());
                std::snprintf(scene.sound_buf, sizeof scene.sound_buf, "%s", st.sound_folder.c_str());
                scene.sound_folder = scene.sound_buf;
                scene.sound_volume = st.sound_volume;
            }
        }
        std::snprintf(scene.lib_buf, sizeof scene.lib_buf, "%s", folder.c_str());
        if (!folder.empty()) scan_library(scene, folder);
    }
    if (!options.clip_path.empty()) {
        load_clip(scene, options.clip_path);
        offer_recovery(scene);
    }
    solve_scene(scene);
    if (scene.rig_ok) frame_scene(scene);

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, GetModuleHandleW(nullptr), nullptr, LoadCursorW(nullptr, IDC_ARROW),
                      nullptr, nullptr, L"KyubuAnimator", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Kyubu Animator", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (!hwnd || !create_device(hwnd)) {
        destroy_device();
        MessageBoxW(nullptr, L"The Animator could not create its window or its Direct3D 9 device.", L"Kyubu Animator",
                    MB_OK | MB_ICONERROR);
        if (hwnd) DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // no imgui.ini beside the program: the layout is not kept yet
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_dev);
    scene.audio.open();   // when it fails, the Sounds panel says why; the rest of the window works the same

    const D3DCOLOR clear = D3DCOLOR_RGBA(38, 40, 46, 255);
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_device_lost) {
            HRESULT hr = g_dev->TestCooperativeLevel();
            if (hr == D3DERR_DEVICELOST) { Sleep(10); continue; }
            if (hr == D3DERR_DEVICENOTRESET) reset_device();
            g_device_lost = false;
        }
        if (g_resize_w != 0 && g_resize_h != 0) {
            g_pp.BackBufferWidth = g_resize_w;
            g_pp.BackBufferHeight = g_resize_h;
            g_resize_w = g_resize_h = 0;
            reset_device();
        }

        if (scene.todo != Scene::After::None) {   // New, Open or Quit, asked for in the last frame: a dialog runs outside a frame
            const Scene::After a = scene.todo;
            scene.todo = Scene::After::None;
            run_after(scene, hwnd, a);
            continue;
        }
        if (g_close_requested && scene.modal == Scene::Modal::None && !held_edit(scene)) {
            g_close_requested = false;
            begin_after(scene, Scene::After::Quit);
        }
        const uint64_t now_ms = GetTickCount64();
        watch_disk(scene, now_ms);
        autosave_tick(scene, now_ms);
        refresh_checks(scene);

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float top = ImGui::GetFrameHeight();   // under the menu bar
        Viewport vp;
        vp.x = kLeftPanelW;
        vp.y = top;
        vp.w = std::max(display.x - kLeftPanelW - kRightPanelW, 1.0f);
        vp.h = std::max(display.y - top - kBottomPanelH, 1.0f);

        {
            const bool was_playing = scene.pb.playing;
            const float before_ms = scene.pb.time_ms;
            scene.pb.advance(ImGui::GetIO().DeltaTime * 1000.0f);
            play_due_sounds(scene, was_playing, before_ms);
        }
        solve_scene(scene);
        draw_menus(hwnd, scene);
        draw_modals(scene);
        draw_parts(scene, top, vp.h);
        draw_timeline(scene, display.y - kBottomPanelH, display.x, kBottomPanelH);
        draw_inspector(scene, display.x - kRightPanelW, top, kRightPanelW, std::min(kKeyPanelH, vp.h));
        draw_curve(scene, display.x - kRightPanelW, top + kKeyPanelH, kRightPanelW, std::max(vp.h - kKeyPanelH, 1.0f));
        draw_view_overlay(scene, vp);
        handle_view_input(scene, vp);
        ImGui::EndFrame();

        g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clear, 1.0f, 0);
        if (g_dev->BeginScene() >= 0) {
            draw_view(scene, vp);
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_dev->EndScene();
        }
        if (g_dev->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST) g_device_lost = true;
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

}  // namespace kanimator
