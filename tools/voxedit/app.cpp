// kyubu-voxedit's window (docs/dev-tools.md). The shape is the Animator's: one Win32 window, one Direct3D 9 device, Dear ImGui panels
// over a 3D view drawn with the fixed pipeline. The models are meshed per 16-cell chunk (voxel.h) into managed vertex buffers, so
// an edit makes only the chunks it touched again.
#include "app.h"

#include <d3d9.h>
#include <windows.h>

#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "camera.h"
#include "cubmesh.h"
#include "doc.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "voxel.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace kvox {

namespace {

using kanimator::Camera;
using kanimator::Mat4;
using kanimator::MeshVertex;
using kanimator::Ray;
using kanimator::Viewport;

IDirect3D9* g_d3d = nullptr;
IDirect3DDevice9* g_dev = nullptr;
D3DPRESENT_PARAMETERS g_pp = {};
bool g_device_lost = false;
UINT g_resize_w = 0, g_resize_h = 0;
bool g_close_requested = false;

bool create_device(HWND hwnd) {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return false;
    g_pp = {};
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_pp.EnableAutoDepthStencil = TRUE;
    g_pp.AutoDepthStencilFormat = D3DFMT_D24S8;   // a big model needs the depth precision
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_pp, &g_dev) >= 0) return true;
    g_pp.AutoDepthStencilFormat = D3DFMT_D16;
    return g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_pp, &g_dev) >= 0;
}

void destroy_device() {
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    if (g_d3d) { g_d3d->Release(); g_d3d = nullptr; }
}

void reset_device() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    if (g_dev->Reset(&g_pp) == D3DERR_INVALIDCALL) return;
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
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
        g_close_requested = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Text and files

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(size_t(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

bool ends_with(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != tail[i]) return false;
    return true;
}

std::string file_name(const std::string& path) {
    const size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? path : path.substr(s + 1);
}

// Writes `bytes` to `path + ".tmp"`, then replaces the file with it. The first time a file is overwritten, its old bytes are kept as
// `<file>.bak` (only when no .bak is there yet, so the .bak stays the original).
bool write_model(const std::string& path, const std::string& bytes, std::string& err) {
    const std::wstring w = wide(path), tmp = w + L".tmp", bak = w + L".bak";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        err = "cannot write " + path + ".tmp";
        return false;
    }
    const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) {
        DeleteFileW(tmp.c_str());
        err = "cannot write " + path + ".tmp (disk full?)";
        return false;
    }
    if (GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES)
        CopyFileW(w.c_str(), bak.c_str(), TRUE);
    if (!MoveFileExW(tmp.c_str(), w.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = "cannot replace " + path + " (is it open in another program?)";
        return false;
    }
    return true;
}

std::string pick_path(HWND owner, bool save) {
    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = save ? L"Cube World models (*.cub)\0*.cub\0"
                           : L"Models and rigs (*.cub, *.rig.toml)\0*.cub;*.rig.toml\0Cube World models (*.cub)\0*.cub\0"
                             L"Rigs (*.rig.toml)\0*.rig.toml\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = DWORD(sizeof buf / sizeof buf[0]);
    ofn.lpstrTitle = save ? L"Save the model as" : L"Open a model or a rig";
    ofn.lpstrDefExt = save ? L"cub" : nullptr;
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST |
                (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return std::string();
    return utf8(buf);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The editor's state

enum class Tool { Add, Remove, Paint, Pick, Fill, Box };
const char* const kToolNames[] = {"Add", "Remove", "Paint", "Pick colour", "Fill", "Box"};
const char* const kToolKeys[] = {"1", "2", "3", "4", "5", "6"};

enum class Pending { None, Open, New, Quit };

struct ChunkBuf {
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT tris = 0;
};
struct PieceGfx {
    int n[3] = {0, 0, 0};
    std::vector<ChunkBuf> chunks;
};

constexpr float kLeftW = 300;
constexpr float kRightW = 240;
constexpr float kStatusH = 26;
constexpr float kClickSlopPx = 4;

struct Editor {
    Doc doc;
    std::string models_dir;
    std::vector<PieceGfx> gfx;

    Camera cam;
    Tool tool = Tool::Add;
    int brush = 1;
    Shape shape = Shape::Cube;
    bool mirror = false;
    bool fill_all = false;               // Fill: every voxel of the colour, not only the connected ones
    Op box_op = Op::Add;
    float colour[3] = {0.8f, 0.2f, 0.2f};
    std::vector<Rgb> recent;

    bool show_grid = true;
    bool dim_others = true;
    bool hide_others = false;

    // the pointer
    bool hover_ok = false;
    Hit hover;
    bool pressing = false;               // a stroke of Add, Remove or Paint: the left button is held
    bool locked = false;                 // the stroke's layer is chosen (at its first voxel)
    int stroke_count = 0;                // voxels the stroke changed
    bool orbit_first = false;            // the first frame of a turn: its pointer delta may be a jump
    int orbit_button = -1;               // 1 right, 2 middle, 0 Alt + left
    bool box_started = false;
    Cell box_a;

    // what the panels show of the active piece, made again when it changes
    uint64_t info_rev = ~0ull;
    int info_piece = -1;
    int info_size[3] = {0, 0, 0};
    size_t info_count = 0;
    std::vector<ColourCount> palette;

    std::string status;
    Pending pending = Pending::None;     // waits for the unsaved-changes question
    Pending todo = Pending::None;        // runs before the next frame (a file dialog must not open inside one)
    bool asking = false;                 // the unsaved-changes question is up
    bool ask_unsaved = false;            // open it in this frame
    bool ask_new = false;
    bool want_save = false, want_save_as = false;
    int new_size[3] = {32, 32, 32};
};

Rgb current_colour(const Editor& e) {
    auto ch = [](float v) { return uint8_t(std::clamp(int(std::lround(v * 255.0f)), 0, 255)); };
    return solid_colour({ch(e.colour[0]), ch(e.colour[1]), ch(e.colour[2])});
}

void set_colour(Editor& e, Rgb c) {
    e.colour[0] = c.r / 255.0f;
    e.colour[1] = c.g / 255.0f;
    e.colour[2] = c.b / 255.0f;
}

void remember_colour(Editor& e, Rgb c) {
    e.recent.erase(std::remove(e.recent.begin(), e.recent.end(), c), e.recent.end());
    e.recent.insert(e.recent.begin(), c);
    if (e.recent.size() > 16) e.recent.resize(16);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Meshes on the device

void release_chunk(ChunkBuf& c) {
    if (c.vb) c.vb->Release();
    c = ChunkBuf();
}

void release_gfx(Editor& e) {
    for (PieceGfx& g : e.gfx)
        for (ChunkBuf& c : g.chunks) release_chunk(c);
    e.gfx.clear();
}

void upload_chunk(ChunkBuf& c, const Mesh& m) {
    release_chunk(c);
    if (m.empty()) return;
    const UINT bytes = UINT(m.tris.size() * sizeof(MeshVertex));
    if (g_dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &c.vb, nullptr) < 0) {
        c.vb = nullptr;
        return;
    }
    void* dst = nullptr;
    if (c.vb->Lock(0, bytes, &dst, 0) < 0) {
        release_chunk(c);
        return;
    }
    std::memcpy(dst, m.tris.data(), bytes);
    c.vb->Unlock();
    c.tris = UINT(m.triangle_count());
}

void sync_gfx(Editor& e) {
    Doc& d = e.doc;
    if (e.gfx.size() != d.pieces.size()) {
        release_gfx(e);
        e.gfx.resize(d.pieces.size());
        for (Piece& p : d.pieces) p.remesh_all = true;
    }
    static const Slice kAll;
    for (size_t i = 0; i < d.pieces.size(); ++i) {
        Piece& p = d.pieces[i];
        PieceGfx& g = e.gfx[i];
        const Slice& sl = int(i) == d.active ? d.slice : kAll;
        const int n[3] = {chunk_count(p.grid, 0), chunk_count(p.grid, 1), chunk_count(p.grid, 2)};
        const auto chunk = [&](uint32_t k, int& cx, int& cy, int& cz) {
            cx = int(k % uint32_t(n[0]));
            cy = int((k / uint32_t(n[0])) % uint32_t(n[1]));
            cz = int(k / uint32_t(n[0] * n[1]));
        };
        if (p.remesh_all || n[0] != g.n[0] || n[1] != g.n[1] || n[2] != g.n[2]) {
            for (ChunkBuf& c : g.chunks) release_chunk(c);
            std::copy(n, n + 3, g.n);
            g.chunks.assign(size_t(n[0]) * n[1] * n[2], ChunkBuf());
            for (uint32_t k = 0; k < g.chunks.size(); ++k) {
                int cx, cy, cz;
                chunk(k, cx, cy, cz);
                upload_chunk(g.chunks[k], mesh_chunk(p.grid, sl, cx, cy, cz));
            }
            p.remesh_all = false;
            p.dirty_chunks.clear();
        } else if (!p.dirty_chunks.empty()) {
            std::sort(p.dirty_chunks.begin(), p.dirty_chunks.end());
            p.dirty_chunks.erase(std::unique(p.dirty_chunks.begin(), p.dirty_chunks.end()), p.dirty_chunks.end());
            for (uint32_t k : p.dirty_chunks) {
                if (k >= g.chunks.size()) continue;
                int cx, cy, cz;
                chunk(k, cx, cy, cz);
                upload_chunk(g.chunks[k], mesh_chunk(p.grid, sl, cx, cy, cz));
            }
            p.dirty_chunks.clear();
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Opening and saving

Vec3 apply_affine(const Affine& a, Vec3 p) {
    const float in[3] = {p.x, p.y, p.z};
    float out[3];
    a.apply(in, out);
    return {out[0], out[1], out[2]};
}

Vec3 turn_only(const Affine& a, Vec3 v) {
    return {a.r[0][0] * v.x + a.r[0][1] * v.y + a.r[0][2] * v.z, a.r[1][0] * v.x + a.r[1][1] * v.y + a.r[1][2] * v.z,
            a.r[2][0] * v.x + a.r[2][1] * v.y + a.r[2][2] * v.z};
}

void frame_all(Editor& e) {
    std::vector<Vec3> pts;
    for (const Piece& p : e.doc.pieces) {
        const float h[3] = {p.grid.size[0] * 0.5f, p.grid.size[1] * 0.5f, p.grid.size[2] * 0.5f};
        for (int c = 0; c < 8; ++c)
            pts.push_back(apply_affine(p.world, {c & 1 ? h[0] : -h[0], c & 2 ? h[1] : -h[1], c & 4 ? h[2] : -h[2]}));
    }
    const kanimator::Sphere s = kanimator::bounding_sphere(pts.data(), pts.size());
    e.cam.frame(s.centre, std::max(s.radius, 4.0f));
}

void frame_active(Editor& e) {
    const Piece* p = e.doc.act();
    if (!p) return frame_all(e);
    std::vector<Vec3> pts;
    const float h[3] = {p->grid.size[0] * 0.5f, p->grid.size[1] * 0.5f, p->grid.size[2] * 0.5f};
    for (int c = 0; c < 8; ++c)
        pts.push_back(apply_affine(p->world, {c & 1 ? h[0] : -h[0], c & 2 ? h[1] : -h[1], c & 4 ? h[2] : -h[2]}));
    const kanimator::Sphere s = kanimator::bounding_sphere(pts.data(), pts.size());
    e.cam.frame(s.centre, std::max(s.radius, 4.0f));
}

void after_open(Editor& e) {
    release_gfx(e);
    e.box_started = false;
    e.info_rev = ~0ull;
    e.info_piece = -1;
    frame_all(e);
}

bool open_path(Editor& e, const std::string& path) {
    Doc d;
    std::string err;
    const bool rig = ends_with(path, ".toml");
    const bool ok = rig ? d.open_rig(path, e.models_dir, err) : d.open_cub(path, err);
    if (!ok) {
        e.status = "Could not open " + file_name(path) + ": " + err;
        return false;
    }
    e.doc = std::move(d);
    after_open(e);
    if (rig) {
        size_t editable = 0;
        for (const Piece& p : e.doc.pieces) editable += p.editable;
        e.status = "Opened " + file_name(path) + ": " + std::to_string(editable) + " models";
        if (!e.doc.notes.empty()) e.status += " (" + std::to_string(e.doc.notes.size()) + " could not be read: see Pieces)";
    } else {
        e.status = "Opened " + file_name(path);
    }
    return true;
}

// Saves every changed model. A model with no file yet (a new one) asks for a name. False when one could not be saved.
bool save_all(Editor& e, HWND hwnd, bool save_as) {
    Doc& d = e.doc;
    int saved = 0;
    for (size_t i = 0; i < d.pieces.size(); ++i) {
        Piece& p = d.pieces[i];
        if (!p.editable) continue;
        const bool wanted = save_as ? int(i) == d.active : p.dirty();
        if (!wanted) continue;
        if (p.grid.count() == 0) {
            e.status = p.name + " has no voxel left: not saved (a .cub must hold one)";
            return false;
        }
        std::string path = p.path;
        if (path.empty() || save_as) {
            path = pick_path(hwnd, true);
            if (path.empty()) {
                e.status = "Not saved";
                return false;
            }
        }
        std::string err;
        if (!write_model(path, cub_bytes(p.grid), err)) {
            e.status = "Not saved: " + err;
            return false;
        }
        if (!d.rig_mode) {
            p.path = path;
            p.name = file_name(path);
        }
        d.mark_saved(int(i));
        ++saved;
    }
    e.status = saved == 0 ? "Nothing to save" : saved == 1 ? "Saved 1 model" : "Saved " + std::to_string(saved) + " models";
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Tools

Ray local_ray(const Piece& p, const Ray& world) {
    Ray r;
    r.origin = apply_affine(p.inv, world.origin);
    r.dir = turn_only(p.inv, world.dir);
    return r;
}

// The cells the tool would touch at the pointer (in the active piece), and the centre cell; false when none.
bool tool_cells(const Editor& e, std::vector<Cell>& cells, Cell& centre) {
    cells.clear();
    if (!e.hover_ok) return false;
    const Hit& h = e.hover;
    const Piece* p = const_cast<Doc&>(e.doc).act();
    if (!p) return false;
    switch (e.tool) {
    case Tool::Add:
        if (!h.add_cell(centre)) return false;
        cells = brush_cells(centre, e.brush, e.shape);
        return true;
    case Tool::Remove:
    case Tool::Paint:
        if (!h.solid) return false;
        centre = h.cell;
        cells = brush_cells(centre, e.brush, e.shape);
        return true;
    case Tool::Pick:
    case Tool::Fill:
        if (!h.solid) return false;
        centre = h.cell;
        cells.push_back(centre);
        return true;
    case Tool::Box: {
        Cell c;
        if (e.box_op == Op::Add ? !h.add_cell(c) : !h.solid) return false;
        if (e.box_op != Op::Add) c = h.cell;
        centre = c;
        cells = e.box_started ? box_cells(e.box_a, c) : std::vector<Cell>{c};
        return true;
    }
    }
    return false;
}

const char* op_word(Op op) { return op == Op::Add ? "add" : op == Op::Remove ? "remove" : "paint"; }

void drag_tool(Editor& e);

void begin_tool(Editor& e) {
    Doc& d = e.doc;
    if (!d.act()) return;
    if (e.tool == Tool::Add || e.tool == Tool::Remove || e.tool == Tool::Paint) {
        // A stroke for as long as the button is held; it may start off the model and reach it later.
        d.begin_stroke();
        e.pressing = true;
        e.locked = false;
        e.stroke_count = 0;
        drag_tool(e);
        return;
    }
    std::vector<Cell> cells;
    Cell centre;
    if (!tool_cells(e, cells, centre)) return;
    const Rgb col = current_colour(e);
    switch (e.tool) {
    case Tool::Pick:
        set_colour(e, d.act()->grid.get(centre));
        remember_colour(e, current_colour(e));
        return;
    case Tool::Fill: {
        const Grid& g = d.act()->grid;
        const Rgb from = g.get(centre);
        std::vector<Cell> region = e.fill_all ? cells_of_colour(g, d.slice, from) : connected_cells(g, d.slice, centre);
        d.begin_stroke();
        const int n = d.apply(region, Op::Paint, col, e.mirror);
        d.end_stroke("fill");
        remember_colour(e, col);
        e.status = "Filled " + std::to_string(n) + " voxels";
        return;
    }
    case Tool::Box:
        if (!e.box_started) {
            e.box_started = true;
            e.box_a = centre;
            e.status = "Box: click the opposite corner (Esc cancels)";
            return;
        }
        d.begin_stroke();
        {
            const int n = d.apply(cells, e.box_op, col, e.mirror);
            d.end_stroke(std::string("box ") + op_word(e.box_op));
            e.status = "Box: " + std::to_string(n) + " voxels changed";
        }
        e.box_started = false;
        if (e.box_op != Op::Remove) remember_colour(e, col);
        return;
    default:
        break;
    }
}

void drag_tool(Editor& e) {
    if (!e.pressing) return;
    std::vector<Cell> cells;
    Cell centre;
    if (!tool_cells(e, cells, centre)) return;
    if (!e.locked) {
        // A one-voxel Add or Remove stays on the layer of its first voxel, so a drag neither piles voxels up toward the camera
        // nor digs a hole.
        e.locked = true;
        const Hit& h = e.hover;
        const int axis = h.normal[0] ? 0 : h.normal[1] ? 1 : h.normal[2] ? 2 : -1;
        const int v[3] = {centre.x, centre.y, centre.z};
        if (axis >= 0 && e.brush == 1 && e.tool != Tool::Paint) e.doc.lock_plane(axis, v[axis]);
    }
    const Op op = e.tool == Tool::Add ? Op::Add : e.tool == Tool::Remove ? Op::Remove : Op::Paint;
    e.stroke_count += e.doc.apply(cells, op, current_colour(e), e.mirror);
}

void end_tool(Editor& e) {
    if (!e.pressing) return;
    e.pressing = false;
    const char* what = e.tool == Tool::Add ? "add" : e.tool == Tool::Remove ? "remove" : "paint";
    e.doc.end_stroke(what);
    if (e.stroke_count == 0) return;
    const char* done = e.tool == Tool::Add ? "Added " : e.tool == Tool::Remove ? "Removed " : "Painted ";
    e.status = done + std::to_string(e.stroke_count) + (e.stroke_count == 1 ? " voxel" : " voxels");
    if (e.tool != Tool::Remove) remember_colour(e, current_colour(e));
}

// The piece under the pointer (any piece, ignoring the slice), or -1.
int pick_piece(const Editor& e, const Ray& world) {
    int best = -1;
    float best_t = 1e30f;
    static const Slice kAll;
    for (size_t i = 0; i < e.doc.pieces.size(); ++i) {
        const Piece& p = e.doc.pieces[i];
        if (!p.editable) continue;
        if (e.hide_others && int(i) != e.doc.active) continue;
        const Ray r = local_ray(p, world);
        const Hit h = raycast(p.grid, kAll, r.origin, r.dir);
        if (h.solid && h.t < best_t) {
            best_t = h.t;
            best = int(i);
        }
    }
    return best;
}

void do_undo(Editor& e) {
    const std::string what = e.doc.undo_name();
    e.status = e.doc.undo() ? "Undid " + what : "Nothing to undo";
}

void do_redo(Editor& e) {
    const std::string what = e.doc.redo_name();
    e.status = e.doc.redo() ? "Redid " + what : "Nothing to redo";
}

void reset_slice(Editor& e) {
    Piece* p = e.doc.act();
    if (!p) return;
    e.doc.slice.on = false;
    for (int a = 0; a < 3; ++a) {
        e.doc.slice.lo[a] = 0;
        e.doc.slice.hi[a] = p->grid.size[a] - 1;
    }
    p->remesh_all = true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Drawing

D3DMATRIX to_d3d(const Mat4& m) {
    Mat4 t = kanimator::transposed(m);
    D3DMATRIX d;
    std::memcpy(&d, t.m, sizeof d);
    return d;
}

void add_line(std::vector<MeshVertex>& v, Vec3 a, Vec3 b, uint32_t c) {
    v.push_back({a.x, a.y, a.z, c});
    v.push_back({b.x, b.y, b.z, c});
}

void add_box(std::vector<MeshVertex>& v, Vec3 lo, Vec3 hi, uint32_t c) {
    const Vec3 p[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                       {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    static const int kE[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& ed : kE) add_line(v, p[ed[0]], p[ed[1]], c);
}

// The box around cells, in the model's frame, a hair larger than the cells so its lines are not hidden in the faces.
void cells_box(const Grid& g, const std::vector<Cell>& cells, Vec3& lo, Vec3& hi) {
    Cell a = cells.front(), b = cells.front();
    for (const Cell& c : cells) {
        a = {std::min(a.x, c.x), std::min(a.y, c.y), std::min(a.z, c.z)};
        b = {std::max(b.x, c.x), std::max(b.y, c.y), std::max(b.z, c.z)};
    }
    const float k = 0.02f;
    lo = g.centre_of(a) - Vec3{0.5f + k, 0.5f + k, 0.5f + k};
    hi = g.centre_of(b) + Vec3{0.5f + k, 0.5f + k, 0.5f + k};
}

void draw_lines(const std::vector<MeshVertex>& v, const Affine* world, bool on_top) {
    if (v.empty()) return;
    D3DMATRIX m = to_d3d(world ? kanimator::mat4_from_rt(world->r, world->t) : Mat4{});
    g_dev->SetTransform(D3DTS_WORLD, &m);
    if (on_top) g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->DrawPrimitiveUP(D3DPT_LINELIST, UINT(v.size() / 2), v.data(), sizeof(MeshVertex));
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
}

void draw_view(Editor& e, const Viewport& vp) {
    D3DVIEWPORT9 dv;
    dv.X = DWORD(std::max(vp.x, 0.0f));
    dv.Y = DWORD(std::max(vp.y, 0.0f));
    dv.Width = DWORD(std::max(vp.w, 1.0f));
    dv.Height = DWORD(std::max(vp.h, 1.0f));
    dv.MinZ = 0;
    dv.MaxZ = 1;
    g_dev->SetViewport(&dv);
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_RGBA(38, 40, 46, 255), 1.0f, 0);

    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    g_dev->SetTexture(0, nullptr);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
    g_dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);

    D3DMATRIX view = to_d3d(kanimator::view_matrix(e.cam));
    D3DMATRIX proj = to_d3d(kanimator::projection_matrix(e.cam, vp.h > 0 ? vp.w / vp.h : 1.0f));
    g_dev->SetTransform(D3DTS_VIEW, &view);
    g_dev->SetTransform(D3DTS_PROJECTION, &proj);

    Doc& d = e.doc;
    if (e.show_grid) {
        float reach = 32;
        for (const Piece& p : d.pieces)
            reach = std::max({reach, std::fabs(p.world.t[0]) + p.grid.size[0], std::fabs(p.world.t[1]) + p.grid.size[1]});
        reach = std::ceil(reach / 8) * 8;
        std::vector<MeshVertex> lines;
        for (float g = -reach; g <= reach; g += 8) {
            const uint32_t c = g == 0 ? 0xFF6A6D75 : 0xFF45484F;
            add_line(lines, {g, -reach, 0}, {g, reach, 0}, c);
            add_line(lines, {-reach, g, 0}, {reach, g, 0}, c);
        }
        draw_lines(lines, nullptr, false);
    }

    for (size_t i = 0; i < d.pieces.size() && i < e.gfx.size(); ++i) {
        const bool is_active = int(i) == d.active;
        if (!is_active && e.hide_others) continue;
        const Piece& p = d.pieces[i];
        D3DMATRIX world = to_d3d(kanimator::mat4_from_rt(p.world.r, p.world.t));
        g_dev->SetTransform(D3DTS_WORLD, &world);
        const bool dim = d.rig_mode && !is_active && e.dim_others;
        g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, dim ? 0xFF7A7A7A : 0xFFFFFFFF);
        for (const ChunkBuf& c : e.gfx[i].chunks) {
            if (!c.vb) continue;
            g_dev->SetStreamSource(0, c.vb, 0, sizeof(MeshVertex));
            g_dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, c.tris);
        }
    }
    g_dev->SetStreamSource(0, nullptr, 0, 0);
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);

    const Piece* a = d.act();
    if (a) {
        std::vector<MeshVertex> lines;
        // the active piece's outline in a rig, and the slice
        const float h[3] = {a->grid.size[0] * 0.5f, a->grid.size[1] * 0.5f, a->grid.size[2] * 0.5f};
        if (d.rig_mode) add_box(lines, {-h[0], -h[1], -h[2]}, {h[0], h[1], h[2]}, 0xFF5A6070);
        if (d.slice.on) {
            std::vector<Cell> two = {{d.slice.lo[0], d.slice.lo[1], d.slice.lo[2]}, {d.slice.hi[0], d.slice.hi[1], d.slice.hi[2]}};
            Vec3 lo, hi;
            cells_box(a->grid, two, lo, hi);
            add_box(lines, lo, hi, 0xFFE0A040);
        }
        draw_lines(lines, &a->world, false);
        // what the tool would touch
        std::vector<Cell> cells;
        Cell centre;
        lines.clear();
        if (!e.pressing && tool_cells(e, cells, centre) && !cells.empty()) {
            uint32_t c = 0xFFFFFFFF;
            switch (e.tool) {
            case Tool::Add: c = 0xFF60E070; break;
            case Tool::Remove: c = 0xFFF05050; break;
            case Tool::Paint: c = 0xFFFFE040; break;
            case Tool::Box: c = 0xFF50D0F0; break;
            default: c = 0xFFFFFFFF; break;
            }
            Vec3 lo, hi;
            cells_box(a->grid, cells, lo, hi);
            add_box(lines, lo, hi, c);
            draw_lines(lines, &a->world, true);
        }
    }
    if (e.mirror) {
        // the mirror plane, x = 0 of the world
        float y = 32, z = 32;
        for (const Piece& p : d.pieces) {
            y = std::max(y, std::fabs(p.world.t[1]) + p.grid.size[1] * 0.6f);
            z = std::max(z, p.world.t[2] + p.grid.size[2] * 0.6f);
        }
        std::vector<MeshVertex> lines;
        const uint32_t c = 0xFFB070F0;
        add_line(lines, {0, -y, 0}, {0, y, 0}, c);
        add_line(lines, {0, y, 0}, {0, y, z}, c);
        add_line(lines, {0, y, z}, {0, -y, z}, c);
        add_line(lines, {0, -y, z}, {0, -y, 0}, c);
        draw_lines(lines, nullptr, false);
    }

    D3DVIEWPORT9 full = dv;
    full.X = 0;
    full.Y = 0;
    full.Width = DWORD(ImGui::GetIO().DisplaySize.x);
    full.Height = DWORD(ImGui::GetIO().DisplaySize.y);
    g_dev->SetViewport(&full);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Panels

void refresh_info(Editor& e) {
    const Piece* p = e.doc.act();
    if (!p) {
        e.palette.clear();
        e.info_count = 0;
        return;
    }
    if (e.info_piece == e.doc.active && e.info_rev == p->rev && std::equal(e.info_size, e.info_size + 3, p->grid.size)) return;
    if (e.doc.in_stroke()) return;   // once the stroke ends
    e.info_piece = e.doc.active;
    e.info_rev = p->rev;
    std::copy(p->grid.size, p->grid.size + 3, e.info_size);
    e.info_count = p->grid.count();
    e.palette = colour_counts(p->grid, 64);
}

void swatches(Editor& e, const std::vector<Rgb>& colours, const char* id) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float sz = 22, gap = ImGui::GetStyle().ItemSpacing.x;
    const int per_row = std::max(1, int((w + gap) / (sz + gap)));
    for (size_t i = 0; i < colours.size(); ++i) {
        const Rgb c = colours[i];
        ImGui::PushID(int(i));
        ImGui::PushID(id);
        char tip[32];
        std::snprintf(tip, sizeof tip, "##sw");
        if (ImGui::ColorButton(tip, ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1), ImGuiColorEditFlags_NoTooltip, ImVec2(sz, sz)))
            set_colour(e, c);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d, %d, %d", c.r, c.g, c.b);
        ImGui::PopID();
        ImGui::PopID();
        if ((i + 1) % per_row != 0 && i + 1 < colours.size()) ImGui::SameLine();
    }
}

void draw_left(Editor& e, float top, float height) {
    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(ImVec2(kLeftW, height));
    ImGui::Begin("##left", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    Doc& d = e.doc;
    Piece* a = d.act();
    if (a) {
        ImGui::TextUnformatted(a->name.c_str());
        ImGui::TextDisabled("%d x %d x %d, %zu voxels%s", e.info_size[0], e.info_size[1], e.info_size[2], e.info_count,
                            a->dirty() ? ", unsaved" : "");
    } else {
        ImGui::TextDisabled("Nothing open: File > Open (Ctrl+O)");
    }

    ImGui::SeparatorText("Tool");
    for (int t = 0; t < 6; ++t) {
        char label[48];
        std::snprintf(label, sizeof label, "%s  (%s)", kToolNames[t], kToolKeys[t]);
        if (ImGui::RadioButton(label, e.tool == Tool(t))) {
            e.tool = Tool(t);
            e.box_started = false;
        }
        if (t % 2 == 0) ImGui::SameLine(kLeftW * 0.5f);
    }
    if (e.tool == Tool::Add || e.tool == Tool::Remove || e.tool == Tool::Paint) {
        ImGui::SetNextItemWidth(150);
        ImGui::SliderInt("Brush size  [ ]", &e.brush, 1, 32);
        int sh = int(e.shape);
        ImGui::RadioButton("Cube", &sh, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Sphere", &sh, 1);
        e.shape = Shape(sh);
        if (e.brush == 1) ImGui::TextDisabled(e.tool == Tool::Paint ? "Drag to paint." : "A drag stays on the first layer.");
    } else if (e.tool == Tool::Fill) {
        ImGui::Checkbox("Every voxel of that colour", &e.fill_all);
        ImGui::TextDisabled(e.fill_all ? "Recolours the colour clicked, everywhere." : "Recolours the touching voxels of one colour.");
    } else if (e.tool == Tool::Box) {
        int op = int(e.box_op);
        ImGui::RadioButton("Fill box", &op, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Clear box", &op, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Paint box", &op, 2);
        if (Op(op) != e.box_op) e.box_started = false;
        e.box_op = Op(op);
        ImGui::TextDisabled(e.box_started ? "Click the opposite corner (Esc cancels)." : "Click one corner, then the other.");
    } else {
        ImGui::TextDisabled("Click a voxel to take its colour.");
    }
    ImGui::Checkbox("Mirror across x = 0  (M)", &e.mirror);

    ImGui::SeparatorText("Colour");
    ImGui::SetNextItemWidth(-1);
    ImGui::ColorEdit3("##colour", e.colour, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB);
    ImGui::TextDisabled("Shift+click a voxel: take its colour");
    if (!e.recent.empty()) {
        ImGui::TextUnformatted("Recent");
        swatches(e, e.recent, "recent");
    }
    if (!e.palette.empty()) {
        ImGui::TextUnformatted("In this model (most used first)");
        std::vector<Rgb> cs;
        for (const ColourCount& c : e.palette) cs.push_back(c.c);
        swatches(e, cs, "palette");
    }

    ImGui::SeparatorText("Slice");
    if (a) {
        Slice& sl = d.slice;
        Slice before = sl;
        ImGui::Checkbox("Show only a part  (PgUp/PgDn: top layer)", &sl.on);
        if (sl.on) {
            const char* names[3] = {"x", "y", "z"};
            for (int k = 0; k < 3; ++k) {
                ImGui::SetNextItemWidth(200);
                ImGui::DragIntRange2(names[k], &sl.lo[k], &sl.hi[k], 0.25f, 0, a->grid.size[k] - 1);
            }
            if (ImGui::Button("One z layer")) sl.lo[2] = sl.hi[2];
            ImGui::SameLine();
            if (ImGui::Button("Left half")) {
                sl.hi[0] = a->grid.size[0] / 2 - 1;
                sl.lo[0] = 0;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset")) reset_slice(e);
            sl.clamp_to(a->grid);
        }
        if (std::memcmp(&before, &sl, sizeof sl) != 0) a->remesh_all = true;
    }

    ImGui::SeparatorText("View");
    if (ImGui::Button("Front")) { e.cam.yaw_deg = 0; e.cam.pitch_deg = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Back")) { e.cam.yaw_deg = 180; e.cam.pitch_deg = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Left")) { e.cam.yaw_deg = -90; e.cam.pitch_deg = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Right")) { e.cam.yaw_deg = 90; e.cam.pitch_deg = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Top")) { e.cam.pitch_deg = kanimator::kMaxPitchDeg; }
    if (ImGui::Button("Frame all")) frame_all(e);
    if (d.rig_mode) {
        ImGui::SameLine();
        if (ImGui::Button("Frame piece  (F)")) frame_active(e);
    }
    ImGui::Checkbox("Ground grid", &e.show_grid);
    if (d.rig_mode) {
        ImGui::Checkbox("Dim the other pieces", &e.dim_others);
        ImGui::Checkbox("Hide the other pieces  (H)", &e.hide_others);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Left: use the tool   Right drag: turn");
    ImGui::TextDisabled("Middle drag: move   Wheel: zoom");
    if (d.rig_mode) ImGui::TextDisabled("Ctrl+click: edit the piece clicked");
    ImGui::End();
}

void draw_right(Editor& e, float x, float top, float height) {
    Doc& d = e.doc;
    ImGui::SetNextWindowPos(ImVec2(x, top));
    ImGui::SetNextWindowSize(ImVec2(kRightW, height));
    ImGui::Begin("Pieces", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::TextDisabled("%s", file_name(d.rig_path).c_str());
    for (size_t i = 0; i < d.pieces.size(); ++i) {
        const Piece& p = d.pieces[i];
        char label[96];
        std::snprintf(label, sizeof label, "%s%s%s", p.name.c_str(), p.dirty() ? " *" : "", p.editable ? "" : "  (box)");
        if (!p.editable) ImGui::BeginDisabled();
        if (ImGui::Selectable(label, int(i) == d.active) && d.set_active(int(i))) e.box_started = false;
        if (!p.editable) ImGui::EndDisabled();
    }
    if (!d.notes.empty()) {
        ImGui::SeparatorText("Not loaded");
        for (const std::string& n : d.notes) ImGui::TextWrapped("%s", n.c_str());
    }
    ImGui::End();
}

void draw_status(Editor& e, float y, float w) {
    ImGui::SetNextWindowPos(ImVec2(0, y));
    ImGui::SetNextWindowSize(ImVec2(w, kStatusH));
    ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const Piece* a = e.doc.act();
    if (a && e.hover_ok && e.hover.solid) {
        const Rgb c = a->grid.get(e.hover.cell);
        ImGui::Text("voxel %d, %d, %d   colour %d, %d, %d", e.hover.cell.x, e.hover.cell.y, e.hover.cell.z, c.r, c.g, c.b);
    } else if (a && e.hover_ok) {
        ImGui::Text("floor %d, %d, %d", e.hover.cell.x, e.hover.cell.y, e.hover.cell.z);
    } else {
        ImGui::TextDisabled("-");
    }
    ImGui::SameLine(330);
    ImGui::TextUnformatted(e.status.c_str());
    ImGui::End();
}

void draw_menus(Editor& e) {
    if (!ImGui::BeginMainMenuBar()) return;
    Doc& d = e.doc;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Ctrl+O")) e.pending = Pending::Open;
        if (ImGui::MenuItem("New model...")) e.pending = Pending::New;
        ImGui::Separator();
        if (ImGui::MenuItem(d.rig_mode ? "Save changed models" : "Save", "Ctrl+S", false, d.any_dirty())) e.want_save = true;
        if (ImGui::MenuItem("Save as...", nullptr, false, !d.rig_mode && d.act())) e.want_save_as = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) g_close_requested = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        std::string u = "Undo " + d.undo_name(), r = "Redo " + d.redo_name();
        if (ImGui::MenuItem(u.c_str(), "Ctrl+Z", false, d.can_undo())) do_undo(e);
        if (ImGui::MenuItem(r.c_str(), "Ctrl+Y", false, d.can_redo())) do_redo(e);
        ImGui::Separator();
        if (ImGui::MenuItem("Trim empty borders", nullptr, false, d.act() != nullptr))
            e.status = d.trim_active() ? "Trimmed the empty borders (the same on both sides, so it stays in place)" : "Nothing to trim";
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void draw_modals(Editor& e, HWND hwnd, bool& quit) {
    if (e.ask_unsaved) {
        ImGui::OpenPopup("Unsaved changes");
        e.ask_unsaved = false;
    }
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Some models have unsaved changes.");
        bool go = false;
        if (ImGui::Button("Save")) {
            if (save_all(e, hwnd, false)) go = true;
            ImGui::CloseCurrentPopup();
            e.asking = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            go = true;
            ImGui::CloseCurrentPopup();
            e.asking = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            e.pending = Pending::None;
            ImGui::CloseCurrentPopup();
            e.asking = false;
        }
        if (go) {
            if (e.pending == Pending::Quit) quit = true;
            else e.todo = e.pending;
            e.pending = Pending::None;
        }
        ImGui::EndPopup();
    }
    if (e.ask_new) {
        ImGui::OpenPopup("New model");
        e.ask_new = false;
    }
    if (ImGui::BeginPopupModal("New model", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputInt3("Size x, y, z", e.new_size);
        for (int& s : e.new_size) s = std::clamp(s, 1, kMaxSize);
        if (ImGui::Button("Make")) {
            e.doc.new_model(e.new_size[0], e.new_size[1], e.new_size[2]);
            after_open(e);
            e.status = "A new empty model: click the floor to add voxels";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Input in the view

// Alt as the keyboard has it now. ImGui's flag can stay set when the key's release went to another window.
bool alt_held() { return (GetKeyState(VK_MENU) & 0x8000) != 0; }

void handle_view(Editor& e, const Viewport& vp) {
    ImGuiIO& io = ImGui::GetIO();
    const bool inside = io.MousePos.x >= vp.x && io.MousePos.y >= vp.y && io.MousePos.x < vp.x + vp.w && io.MousePos.y < vp.y + vp.h;
    const bool free_pointer = inside && !io.WantCaptureMouse;
    Doc& d = e.doc;
    Piece* a = d.act();
    const Ray world_ray = kanimator::ray_through_pixel(e.cam, vp, io.MousePos.x, io.MousePos.y);

    e.hover_ok = false;
    if (a && (free_pointer || e.pressing) && e.orbit_button < 0) {
        const Ray r = local_ray(*a, world_ray);
        e.hover = raycast(a->grid, d.slice, r.origin, r.dir);
        e.hover_ok = e.hover.any();
    }

    // turning and moving the view
    if (e.orbit_button < 0 && free_pointer && !e.pressing) {
        if (ImGui::IsMouseClicked(1)) e.orbit_button = 1;
        else if (ImGui::IsMouseClicked(2)) e.orbit_button = 2;
        else if (ImGui::IsMouseClicked(0) && alt_held()) e.orbit_button = 0;
        e.orbit_first = e.orbit_button >= 0;
    }
    if (e.orbit_button >= 0) {
        if (e.orbit_first) {
            e.orbit_first = false;   // the press frame: its delta may be the pointer arriving
        } else if (ImGui::IsMouseDown(e.orbit_button)) {
            const bool pan = e.orbit_button == 2 || (e.orbit_button == 1 && io.KeyShift);
            if (pan) e.cam.pan(io.MouseDelta.x, io.MouseDelta.y, vp.h);
            else e.cam.orbit(io.MouseDelta.x, io.MouseDelta.y);
        } else {
            e.orbit_button = -1;
        }
    }
    if (free_pointer && io.MouseWheel != 0) e.cam.zoom(io.MouseWheel);

    // the tool
    if (e.orbit_button < 0 && free_pointer && !e.pressing && ImGui::IsMouseClicked(0)) {
        if (io.KeyCtrl && d.rig_mode) {
            const int p = pick_piece(e, world_ray);
            if (p >= 0 && d.set_active(p)) {
                e.box_started = false;
                e.status = "Editing " + d.pieces[p].name;
            }
        } else if (io.KeyShift) {
            if (a && e.hover_ok && e.hover.solid) {
                set_colour(e, a->grid.get(e.hover.cell));
                remember_colour(e, current_colour(e));
            }
        } else {
            begin_tool(e);
        }
    }
    if (e.pressing) {
        if (!ImGui::IsMouseDown(0)) end_tool(e);
        else if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) drag_tool(e);
    }
}

void handle_keys(Editor& e) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;
    Doc& d = e.doc;
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? do_redo(e) : do_undo(e);
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) do_redo(e);
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) e.want_save = true;
        if (ImGui::IsKeyPressed(ImGuiKey_O, false)) e.pending = Pending::Open;
        return;
    }
    if (e.pressing) return;
    const ImGuiKey nums[6] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6};
    for (int t = 0; t < 6; ++t)
        if (ImGui::IsKeyPressed(nums[t], false)) {
            e.tool = Tool(t);
            e.box_started = false;
        }
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) e.mirror = !e.mirror;
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) e.hide_others = !e.hide_others;
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) d.rig_mode ? frame_active(e) : frame_all(e);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) e.brush = std::max(1, e.brush - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) e.brush = std::min(32, e.brush + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && e.box_started) {
        e.box_started = false;
        e.status = "Box cancelled";
    }
    Piece* a = d.act();
    if (a && (ImGui::IsKeyPressed(ImGuiKey_PageDown) || ImGui::IsKeyPressed(ImGuiKey_PageUp))) {
        Slice& sl = d.slice;
        if (!sl.on) {
            sl.on = true;
            for (int k = 0; k < 3; ++k) {
                sl.lo[k] = 0;
                sl.hi[k] = a->grid.size[k] - 1;
            }
        }
        sl.hi[2] += ImGui::IsKeyPressed(ImGuiKey_PageUp) ? 1 : -1;
        sl.hi[2] = std::clamp(sl.hi[2], 0, a->grid.size[2] - 1);
        sl.lo[2] = std::min(sl.lo[2], sl.hi[2]);
        a->remesh_all = true;
    }
}

void set_title(HWND hwnd, const Editor& e) {
    std::string t = "Kyubu VoxEdit";
    const Doc& d = e.doc;
    if (d.rig_mode) t += " - " + file_name(d.rig_path);
    else if (!d.pieces.empty()) t += " - " + d.pieces[0].name;
    if (d.any_dirty()) t += " *";
    static std::string last;
    if (t == last) return;
    last = t;
    SetWindowTextW(hwnd, wide(t).c_str());
}

}  // namespace

int run_app(const AppOptions& options) {
    Editor e;
    e.models_dir = options.models_dir;
    e.cam.yaw_deg = 30;
    e.cam.pitch_deg = 20;

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, GetModuleHandleW(nullptr), nullptr, LoadCursorW(nullptr, IDC_ARROW),
                      nullptr, nullptr, L"KyubuVoxEdit", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Kyubu VoxEdit", WS_OVERLAPPEDWINDOW, 80, 60, 1400, 880, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (!hwnd || !create_device(hwnd)) {
        destroy_device();
        MessageBoxW(nullptr, L"VoxEdit could not create its window or its Direct3D 9 device.", L"Kyubu VoxEdit", MB_OK | MB_ICONERROR);
        if (hwnd) DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_dev);

    if (!options.path.empty()) open_path(e, options.path);
    else e.status = "File > Open (Ctrl+O) a .cub model or a .rig.toml";

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
            const HRESULT hr = g_dev->TestCooperativeLevel();
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

        // Asked for in the last frame. A question about unsaved changes comes first; the dialogs run outside a frame.
        if ((e.want_save || e.want_save_as) && !e.doc.in_stroke()) {
            const bool as = e.want_save_as;
            e.want_save = e.want_save_as = false;
            save_all(e, hwnd, as);
            continue;   // a file dialog may have run
        }
        if (g_close_requested) {
            g_close_requested = false;
            if (!e.asking) e.pending = Pending::Quit;
        }
        if (e.pending != Pending::None && !e.asking && !e.doc.in_stroke()) {
            if (e.doc.any_dirty()) {
                e.ask_unsaved = true;
                e.asking = true;
            } else {
                if (e.pending == Pending::Quit) break;
                e.todo = e.pending;
                e.pending = Pending::None;
            }
        }
        if (e.todo != Pending::None) {
            const Pending t = e.todo;
            e.todo = Pending::None;
            if (t == Pending::Open) {
                const std::string p = pick_path(hwnd, false);
                if (!p.empty()) open_path(e, p);
            } else if (t == Pending::New) {
                e.ask_new = true;
            }
            continue;
        }

        sync_gfx(e);
        refresh_info(e);
        set_title(hwnd, e);

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float top = ImGui::GetFrameHeight();
        const float right = e.doc.rig_mode ? kRightW : 0;
        Viewport vp;
        vp.x = kLeftW;
        vp.y = top;
        vp.w = std::max(display.x - kLeftW - right, 1.0f);
        vp.h = std::max(display.y - top - kStatusH, 1.0f);

        bool quit = false;
        draw_menus(e);
        draw_left(e, top, vp.h);
        if (e.doc.rig_mode) draw_right(e, display.x - kRightW, top, vp.h);
        draw_status(e, display.y - kStatusH, display.x);
        draw_modals(e, hwnd, quit);
        handle_view(e, vp);
        handle_keys(e);
        ImGui::EndFrame();
        if (quit) break;

        sync_gfx(e);   // the edits of this frame show in it
        g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_RGBA(30, 31, 36, 255), 1.0f, 0);
        if (g_dev->BeginScene() >= 0) {
            draw_view(e, vp);
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_dev->EndScene();
        }
        if (g_dev->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST) g_device_lost = true;
    }

    release_gfx(e);
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

}  // namespace kvox
