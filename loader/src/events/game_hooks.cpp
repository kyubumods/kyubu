#include "game_hooks.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include "channels.h"
#include "headless.h"
#include "events.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "models.h"
#include "modmanager.h"
#include "net_transport.h"
#include "overlay.h"
#include "registry.h"
#include "runtime.h"
#include "symbols.h"
#include "ui.h"
#include "world.h"

namespace kyubu {

namespace {

// ------------------------------------------------------------------ helpers
KyubuSide host_side() { return L().host.host == KY_HOST_SERVER ? KY_SIDE_SERVER : KY_SIDE_CLIENT; }

template <typename F>
bool hook_iat(const char* dll, const char* fn, F* handler, F** next) {
    std::string err;
    if (Hooks::get().hook_import("kyubu", nullptr, dll, fn, (void*)handler, kLoaderPriority, (void**)next, nullptr, &err) != KY_OK) {
        KDEBUG("import hook %s!%s not installed: %s", dll, fn, err.c_str());
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ world tracking
std::string g_world;
void* g_world_handle = nullptr;

std::string idmap_path(const std::string& world) {
    return join_path(join_path(L().game_dir, "Save"), "world_" + world + ".kyubu-idmap.json");
}

void on_world_enter(const std::string& world) {
    g_world = world;
    KINFO("world entered: %s", world.c_str());
    std::string path = idmap_path(world);
    if (auto text = read_file(path)) {
        std::vector<std::pair<std::string, std::string>> moved;
        auto missing = Registries::get().apply_idmap(std::string(text->begin(), text->end()), &moved);
        for (auto& [id, how] : moved) KINFO("id map: %s renumbered %s", id.c_str(), how.c_str());
        if (!moved.empty() || !missing.empty()) Models::get().apply_numbers();
        if (!missing.empty()) {
            std::string list;
            for (auto& m : missing) list += (list.empty() ? "" : ", ") + m;
            KWARN("world %s uses content from missing mod(s): %s (shown as missing-content placeholders)", world.c_str(), list.c_str());
            if (L().host.host == KY_HOST_CLIENT) Ui::get().notice("This world uses content from missing mods: " + list, 12);
        }
        if (!moved.empty()) Events::get().emit_active("registry.remapped");
    }
    write_text(path, Registries::get().idmap_json());
    KINFO("session manifest: %s", session_manifest_json().c_str());
    game::on_world_enter();
    KyubuWorldData d{g_world.c_str()};
    Events::get().emit_active("lifecycle.world_enter", &d, sizeof d);
}

void on_world_exit() {
    if (g_world.empty()) return;
    write_text(idmap_path(g_world), Registries::get().idmap_json());
    KyubuWorldData d{g_world.c_str()};
    Events::get().emit_active("lifecycle.world_exit", &d, sizeof d);
    game::on_world_exit();
    KINFO("world left: %s", g_world.c_str());
    g_world.clear();
    g_world_handle = nullptr;
}

// ------------------------------------------------------------------ frame / simulation
void(__cdecl* g_frame_next)() = nullptr;
IDirect3DDevice9* g_device = nullptr;   // the game's device (set by create_device_hook)

// Once per frame: d3d9 on Windows rewrites the device's Present/Reset slots during Reset(), which removes
// our vtable hooks. Put them back, and finish a reset whose second half bypassed our Reset hook.
void d3d_frame_check() {
    if (!g_device) return;
    if (int n = Hooks::get().repair_vtables())
        KINFO("Direct3D 9 replaced %d hooked device function(s) (device reset); hooks re-applied", n);
    if (Ui::get().needs_recovery() && g_device->TestCooperativeLevel() == D3D_OK) {
        Ui::get().after_reset();
        D3DVIEWPORT9 vp{};
        g_device->GetViewport(&vp);
        KyubuRenderData rd{g_device, vp.Width, vp.Height};
        Events::get().emit("render.device_reset", KY_SIDE_CLIENT, &rd, sizeof rd);
    }
}

void headless_input_frame();
std::vector<std::string> g_pending_shots;   // picture names waiting for the next Present (headless, game thread)

void __cdecl frame_hook() {
    if (headless::enabled()) {
        headless_input_frame();
        static headless::FpsMeter meter;
        double fps = 0, lo = 0;
        if (meter.tick(GetTickCount64(), fps, lo)) KINFO("headless: %.1f fps (min %.1f), frame %llu", fps, lo, meter.total);
    }
    d3d_frame_check();
    runtime_on_frame_begin();
    g_frame_next();
    runtime_on_frame_end();
}

void(__fastcall* g_update_next)(void*, void*, int) = nullptr;
void __fastcall update_hook(void* self, void* /*edx*/, int dt) {
    g_update_next(self, nullptr, dt);
    if (L().mode == KY_MODE_MENU_OR_SINGLEPLAYER && !g_world.empty()) runtime_on_server_tick((double)dt);
}

// ------------------------------------------------------------------ Direct3D 9
using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using CreateDeviceFn = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using D3DCreateFn = IDirect3D9*(WINAPI*)(UINT);
PresentFn g_present_next = nullptr;
ResetFn g_reset_next = nullptr;
CreateDeviceFn g_create_device_next = nullptr;
D3DCreateFn g_d3dcreate_next = nullptr;
HWND g_hwnd = nullptr;

// The proxy's `compress` forwards to the vanilla zlib (design D6). Null falls back to stored blocks.
bool zlib_compress(const unsigned char* in, size_t n, std::vector<unsigned char>& out) {
    using CompressFn = int(__cdecl*)(unsigned char*, unsigned long*, const unsigned char*, unsigned long);
    static CompressFn fn = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE m = GetModuleHandleW(L"zlib1.dll")) fn = (CompressFn)GetProcAddress(m, "compress");
    }
    if (!fn) return false;
    unsigned long cap = (unsigned long)(n + n / 1000 + 64);
    out.resize(cap);
    if (fn(out.data(), &cap, in, (unsigned long)n) != 0) return false;   // Z_OK
    out.resize(cap);
    return true;
}

// Locks a system-memory copy of `src` (the back buffer or the small target) and calls fn(bits, w, h, pitch).
template <class F>
bool with_pixels(IDirect3DDevice9* dev, IDirect3DSurface9* src, F fn) {
    D3DSURFACE_DESC d{};
    if (FAILED(src->GetDesc(&d))) return false;
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return false;
    bool ok = false;
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(dev->GetRenderTargetData(src, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        fn((const unsigned char*)lr.pBits, (int)d.Width, (int)d.Height, (int)lr.Pitch);
        sys->UnlockRect();
        ok = true;
    }
    sys->Release();
    return ok;
}

// Headless (D6): pending .shot files become PNGs; every 4th frame the 64x48 hash goes to kyubu/headless/frame.
void headless_present(IDirect3DDevice9* dev) {
    static unsigned s_present = 0;
    ++s_present;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    if (!g_pending_shots.empty()) {
        const std::string dir = join_path(L().kyubu_dir, "shots");
        make_dirs(dir);
        for (const auto& name : g_pending_shots) {
            const bool ok = with_pixels(dev, bb, [&](const unsigned char* px, int w, int h, int pitch) {
                auto png = headless::png_from_bgra(px, w, h, pitch, &zlib_compress);
                write_file(join_path(dir, name + ".png"), png.data(), png.size());
            });
            if (ok) KINFO("headless: shot %s (present %u)", name.c_str(), s_present);
            else KWARN("headless: shot %s failed (back buffer not readable)", name.c_str());
        }
        g_pending_shots.clear();
    }
    if (s_present % 4 == 0) {
        static IDirect3DSurface9* small = nullptr;
        static IDirect3DDevice9* small_dev = nullptr;
        D3DSURFACE_DESC d{};
        bb->GetDesc(&d);
        if (small && small_dev != dev) { small->Release(); small = nullptr; }   // device was reset: the old target is gone
        if (!small && SUCCEEDED(dev->CreateRenderTarget(64, 48, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &small, nullptr))) small_dev = dev;
        if (small && SUCCEEDED(dev->StretchRect(bb, nullptr, small, nullptr, D3DTEXF_LINEAR))) {
            unsigned hash = 0;
            if (with_pixels(dev, small, [&](const unsigned char* px, int w, int h, int pitch) { hash = headless::frame_hash(px, w, h, pitch); })) {
                char line[64];
                const int n = snprintf(line, sizeof line, "%u %08x\n", s_present, hash);
                const std::string dir = join_path(L().kyubu_dir, "headless");
                make_dirs(dir);
                const std::string tmp = join_path(dir, "frame.tmp");
                if (write_file(tmp, line, (size_t)n)) MoveFileExW(widen(tmp).c_str(), widen(join_path(dir, "frame")).c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
    }
    bb->Release();
}

HRESULT WINAPI present_hook(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    if (headless::enabled()) headless_present(dev);
    if (SUCCEEDED(dev->BeginScene())) {
        if (Ui::get().ready()) Ui::get().render_frame(dev);
        else {
            D3DVIEWPORT9 vp{};
            dev->GetViewport(&vp);
            KyubuRenderData rd{dev, vp.Width, vp.Height};
            Events::get().emit("render.overlay", KY_SIDE_CLIENT, &rd, sizeof rd);
        }
        dev->EndScene();
    }
    return g_present_next(dev, a, b, w, r);
}

HRESULT WINAPI reset_hook(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    KyubuRenderData rd{dev, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0};
    Events::get().emit("render.device_lost", KY_SIDE_CLIENT, &rd, sizeof rd);
    Ui::get().before_reset();
    HRESULT hr = g_reset_next(dev, pp);
    if (SUCCEEDED(hr)) {
        Ui::get().after_reset();
        Events::get().emit("render.device_reset", KY_SIDE_CLIENT, &rd, sizeof rd);
    }
    return hr;
}

HRESULT WINAPI create_device_hook(IDirect3D9* d3d, UINT ad, D3DDEVTYPE t, HWND focus, DWORD flags,
                                  D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HRESULT hr = g_create_device_next(d3d, ad, t, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        IDirect3DDevice9* dev = *out;
        std::string err;
        Hooks::get().hook_vtable("kyubu", dev, 17, (void*)&present_hook, kLoaderPriority, (void**)&g_present_next, nullptr, &err);
        Hooks::get().hook_vtable("kyubu", dev, 16, (void*)&reset_hook, kLoaderPriority, (void**)&g_reset_next, nullptr, &err);
        HWND w = pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus;
        if (!g_hwnd) g_hwnd = w;
        g_device = dev;
        Ui::get().attach_device(dev, w);
        KINFO("Direct3D 9 device created (%ux%u)", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
    }
    return hr;
}

IDirect3D9* WINAPI d3dcreate_hook(UINT sdk) {
    IDirect3D9* d = g_d3dcreate_next(sdk);
    if (d) {
        std::string err;
        Hooks::get().hook_vtable("kyubu", d, 16, (void*)&create_device_hook, kLoaderPriority, (void**)&g_create_device_next, nullptr, &err);
    }
    return d;
}

// ------------------------------------------------------------------ window & input
WNDPROC g_orig_wndproc = nullptr;
bool g_synthetic = false;   // headless: a message we send ourselves passes the drop filter
bool g_block_mouse = false;
using CreateWindowFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowFn g_createwindow_next = nullptr;

LRESULT CALLBACK wndproc_hook(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (headless::enabled() && !g_synthetic && headless::drops_message(m, w)) {   // D5: real input and focus loss never reach the game
        static unsigned dropped = 0;
        if (++dropped == 1 || dropped % 100 == 0) KDEBUG("headless: dropped %u window messages (last 0x%04X)", dropped, (unsigned)m);
        return 0;
    }
    if (Ui::get().wndproc(h, m, w, l)) return m == WM_SETCURSOR ? TRUE : 0;
    bool cancelled = false;
    switch (m) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
            if (Ui::get().wants_keyboard()) break;   // a release the overlay let through for the game only
            KyubuKeyData d{(uint32_t)w, (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) ? 1 : 0, (l & (1 << 30)) ? 1 : 0};
            cancelled = Events::get().emit("input.key", KY_SIDE_CLIENT, &d, sizeof d, true);
            break;
        }
        case WM_CHAR: {
            KyubuCharData d{(uint32_t)w};
            cancelled = Events::get().emit("input.char", KY_SIDE_CLIENT, &d, sizeof d, true);
            break;
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
            int btn = (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP) ? 0 : (m == WM_RBUTTONDOWN || m == WM_RBUTTONUP) ? 1 : 2;
            int down = (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN) ? 1 : 0;
            KyubuMouseButtonData d{btn, down, (short)LOWORD(l), (short)HIWORD(l)};
            cancelled = Events::get().emit("input.mouse_button", KY_SIDE_CLIENT, &d, sizeof d, true);
            if (down) g_block_mouse = cancelled;
            else if (g_block_mouse) { g_block_mouse = false; cancelled = true; }
            break;
        }
        case WM_MOUSEMOVE: {
            static int lx = -1, ly = -1;
            int x = (short)LOWORD(l), y = (short)HIWORD(l);
            KyubuMouseMoveData d{x, y, lx < 0 ? 0 : x - lx, ly < 0 ? 0 : y - ly};
            lx = x; ly = y;
            cancelled = Events::get().emit("input.mouse_move", KY_SIDE_CLIENT, &d, sizeof d, true);
            break;
        }
        case WM_MOUSEWHEEL: {
            KyubuMouseWheelData d{(short)HIWORD(w)};
            cancelled = Events::get().emit("input.mouse_wheel", KY_SIDE_CLIENT, &d, sizeof d, true);
            break;
        }
    }
    if (cancelled) return 0;
    return CallWindowProcW(g_orig_wndproc, h, m, w, l);
}

headless::InputQueue g_input_queue;

void headless_send(UINT m, WPARAM w, LPARAM l) {
    if (!g_hwnd || !g_orig_wndproc) return;
    g_synthetic = true;
    wndproc_hook(g_hwnd, m, w, l);
    g_synthetic = false;
}

// Headless (D4): read the inbox, apply this frame's events to the synthetic state, then send the window messages.
void headless_input_frame() {
    std::vector<std::string> errs;
    headless::read_inbox(join_path(L().kyubu_dir, "inbox"), g_input_queue, &errs);
    for (auto& n : headless::read_shots(join_path(L().kyubu_dir, "inbox"))) g_pending_shots.push_back(n);
    for (auto& e : errs) KWARN("headless input: %s", e.c_str());
    static unsigned s_frame = 0;
    ++s_frame;
    for (const auto& e : g_input_queue.next_frame()) {
        KDEBUG("headless input: frame %u kind %d vk 0x%X dik 0x%X", s_frame, (int)e.kind, (unsigned)e.vk, (unsigned)e.dik);
        headless::apply_state(e);
        const POINT cur = headless::cursor_get();
        const LPARAM pos = MAKELPARAM((short)cur.x, (short)cur.y);
        switch (e.kind) {
            case headless::EvKind::KeyDown:
                headless_send(WM_KEYDOWN, e.vk, 1 | (MapVirtualKeyW(e.vk, MAPVK_VK_TO_VSC) << 16));
                break;
            case headless::EvKind::KeyUp:
                headless_send(WM_KEYUP, e.vk, 1 | (MapVirtualKeyW(e.vk, MAPVK_VK_TO_VSC) << 16) | (1u << 30) | (1u << 31));
                break;
            case headless::EvKind::ButtonDown: {
                static const UINT m[3] = {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN};
                static const WPARAM f[3] = {MK_LBUTTON, MK_RBUTTON, MK_MBUTTON};
                headless_send(m[e.button], f[e.button], pos);
                break;
            }
            case headless::EvKind::ButtonUp: {
                static const UINT m[3] = {WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP};
                headless_send(m[e.button], 0, pos);
                break;
            }
            case headless::EvKind::Move: headless_send(WM_MOUSEMOVE, 0, pos); break;
            case headless::EvKind::Wheel: {
                POINT scr = cur;
                if (g_hwnd) ClientToScreen(g_hwnd, &scr);
                const int n = e.ticks < 0 ? -e.ticks : e.ticks;
                for (int i = 0; i < n; ++i)
                    headless_send(WM_MOUSEWHEEL, MAKEWPARAM(0, (short)(e.ticks < 0 ? -WHEEL_DELTA : WHEEL_DELTA)),
                                  MAKELPARAM((short)scr.x, (short)scr.y));
                break;
            }
            case headless::EvKind::Text:
                for (unsigned u : headless::utf8_to_units(e.text)) headless_send(WM_CHAR, u, 1);
                break;
        }
    }
}

void subclass_window(HWND h) {
    if (!h || g_orig_wndproc) return;
    g_hwnd = h;
    g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)&wndproc_hook);
    if (L().safe_mode) {
        wchar_t title[256];
        GetWindowTextW(h, title, 256);
        SetWindowTextW(h, (std::wstring(title) + L" [Kyubu safe mode]").c_str());
    }
}

HWND WINAPI createwindow_hook(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                              HMENU menu, HINSTANCE inst, LPVOID param) {
    if (headless::enabled() && !parent) ex = headless::window_ex_style(ex);
    HWND r = g_createwindow_next(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (r && !parent && !g_orig_wndproc) subclass_window(r);
    return r;
}

// Headless mode (D3, D5): placement before the first show, and the focus and cursor imports faked.
using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
using GetFocusFn = HWND(WINAPI*)();
using SetFocusFn = HWND(WINAPI*)(HWND);
using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
ShowWindowFn g_showwindow_next = nullptr;
GetFocusFn g_getfocus_next = nullptr;
SetFocusFn g_setfocus_next = nullptr;
GetCursorPosFn g_getcursorpos_next = nullptr;
bool g_headless_placed = false;

BOOL WINAPI showwindow_hook(HWND w, int cmd) {
    if (headless::enabled() && w == g_hwnd && !g_headless_placed) {
        g_headless_placed = true;
        const LONG st = GetWindowLongW(w, GWL_STYLE);
        const DWORD ex = (DWORD)GetWindowLongW(w, GWL_EXSTYLE);
        RECT r{0, 0, headless::kClientWidth, headless::kClientHeight};
        AdjustWindowRectEx(&r, (DWORD)st, FALSE, ex);
        const POINT o = headless::window_origin(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                                                GetSystemMetrics(SM_CXVIRTUALSCREEN));
        SetWindowPos(w, HWND_BOTTOM, o.x, o.y, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_FRAMECHANGED);
        headless::cursor_reset(headless::kClientWidth, headless::kClientHeight);
        KINFO("headless: window placed at %ld,%ld, client %dx%d, no-activate", o.x, o.y, headless::kClientWidth, headless::kClientHeight);
        return g_showwindow_next(w, SW_SHOWNOACTIVATE);
    }
    return g_showwindow_next(w, cmd);
}

HWND WINAPI getfocus_hook() {
    if (headless::enabled() && g_hwnd) return g_hwnd;
    return g_getfocus_next();
}

HWND WINAPI setfocus_hook(HWND w) {
    if (headless::enabled()) return g_hwnd;
    return g_setfocus_next(w);
}

// The virtual cursor as a screen point: the window's client origin plus the client position.
BOOL WINAPI getcursorpos_hook(LPPOINT p) {
    if (!headless::enabled() || !g_hwnd || !p) return g_getcursorpos_next(p);
    POINT c = headless::cursor_get();
    ClientToScreen(g_hwnd, &c);
    *p = c;
    return TRUE;
}

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
SetCursorPosFn g_setcursorpos_next = nullptr;
BOOL WINAPI setcursorpos_hook(int x, int y) {
    if (headless::enabled()) {   // the game re-centres the cursor in the world: move the virtual one, never the user's
        POINT p{x, y};
        if (g_hwnd && ScreenToClient(g_hwnd, &p)) headless::cursor_warp(p.x, p.y);
        return TRUE;
    }
    if (Ui::get().console_open()) return TRUE;   // let the player move the pointer over the console
    return g_setcursorpos_next(x, y);
}

// DirectInput: the game reads the mouse through a DirectInput device (buttons + relative motion).
using DI8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DICreateDeviceFn = HRESULT(WINAPI*)(IDirectInput8W*, REFGUID, LPDIRECTINPUTDEVICE8W*, LPUNKNOWN);
using GetStateFn = HRESULT(WINAPI*)(IDirectInputDevice8W*, DWORD, LPVOID);
DI8CreateFn g_di8_next = nullptr;
DICreateDeviceFn g_dicreate_next = nullptr;
GetStateFn g_getstate_next = nullptr;
using AcquireFn = HRESULT(WINAPI*)(IDirectInputDevice8W*);
AcquireFn g_acquire_next = nullptr;

// The keyboard and the mouse share this vtable slot; the buffer size tells them apart (256 bytes for the keyboard).
HRESULT WINAPI getstate_hook(IDirectInputDevice8W* dev, DWORD cb, LPVOID data) {
    if (headless::enabled() && data) {   // D5: the synthetic state, always filled, whatever the real device says
        if (cb == 256) { headless::keyboard_state((unsigned char*)data); return DI_OK; }
        if (cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2)) { headless::fill_mouse(data, cb); return DI_OK; }
    }
    HRESULT hr = g_getstate_next(dev, cb, data);
    static std::vector<DWORD> seen;
    if (std::find(seen.begin(), seen.end(), cb) == seen.end()) { seen.push_back(cb); KINFO("DirectInput GetDeviceState: %u-byte state seen", (unsigned)cb); }
    if (FAILED(hr) || !data) return hr;
    if (cb == 256) {
        // WinMain() copies the movement and action keys from this state: hide it while the overlay has the keyboard.
        if (Ui::get().wants_keyboard()) memset(data, 0, cb);
    } else if ((cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2)) && (Ui::get().wants_mouse() || g_block_mouse)) {
        auto st = (DIMOUSESTATE*)data;
        memset(st->rgbButtons, 0, cb - offsetof(DIMOUSESTATE, rgbButtons));   // 4 buttons, or 8 for DIMOUSESTATE2
        if (Ui::get().console_open()) st->lX = st->lY = st->lZ = 0;
    }
    return hr;
}

HRESULT WINAPI acquire_hook(IDirectInputDevice8W* dev) {
    if (headless::enabled()) return DI_OK;
    return g_acquire_next(dev);
}

HRESULT WINAPI dicreate_hook(IDirectInput8W* di, REFGUID g, LPDIRECTINPUTDEVICE8W* out, LPUNKNOWN u) {
    HRESULT hr = g_dicreate_next(di, g, out, u);
    if (SUCCEEDED(hr) && out && *out) {
        std::string err;
        Hooks::get().hook_vtable("kyubu", *out, 9, (void*)&getstate_hook, kLoaderPriority, (void**)&g_getstate_next, nullptr, &err);
        if (headless::enabled())   // Acquire (slot 7): the user's real device is never taken
            Hooks::get().hook_vtable("kyubu", *out, 7, (void*)&acquire_hook, kLoaderPriority, (void**)&g_acquire_next, nullptr, &err);
    }
    return hr;
}

HRESULT WINAPI di8create_hook(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out, LPUNKNOWN u) {
    HRESULT hr = g_di8_next(inst, ver, iid, out, u);
    if (SUCCEEDED(hr) && out && *out) {
        std::string err;
        Hooks::get().hook_vtable("kyubu", *out, 3, (void*)&dicreate_hook, kLoaderPriority, (void**)&g_dicreate_next, nullptr, &err);
    }
    return hr;
}

// ------------------------------------------------------------------ network
using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using SendFn = int(WSAAPI*)(SOCKET, const char*, int, int);
using RecvFn = int(WSAAPI*)(SOCKET, char*, int, int);
using CloseFn = int(WSAAPI*)(SOCKET);
using ListenFn = int(WSAAPI*)(SOCKET, int);
using AcceptFn = SOCKET(WSAAPI*)(SOCKET, sockaddr*, int*);
ConnectFn g_connect_next = nullptr;
SendFn g_send_next = nullptr;
RecvFn g_recv_next = nullptr;
CloseFn g_close_next = nullptr;
ListenFn g_listen_next = nullptr;
AcceptFn g_accept_next = nullptr;
SOCKET g_game_socket = INVALID_SOCKET;
std::vector<SOCKET> g_clients;
DWORD g_main_thread = 0;

std::string addr_text(const sockaddr* a, uint16_t* port) {
    if (!a || a->sa_family != AF_INET) return "?";
    auto in = (const sockaddr_in*)a;
    const uint8_t* b = (const uint8_t*)&in->sin_addr;
    if (port) *port = ntohs(in->sin_port);
    return fmt("%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

int WSAAPI connect_hook(SOCKET s, const sockaddr* a, int len) {
    // Vanilla-server safety: content mods that need a Kyubu server block the connection up front.
    std::vector<std::string> needs;
    for (Mod* m : ModManager::get().ordered())
        if (m->active() && m->manifest.requires_server) needs.push_back(m->id);
    if (!needs.empty() && !Channels::get().remote_supports_channels()) {
        std::string list;
        for (auto& n : needs) list += (list.empty() ? "" : ", ") + n;
        KWARN("connection blocked: mod(s) %s require a Kyubu server and this server is not Kyubu-compatible", list.c_str());
        Ui::get().notice("Cannot join: " + list + " need(s) a Kyubu server.", 15);
        std::wstring msg = L"Kyubu: this server is not a Kyubu server, but these enabled mods need one:\n\n" + widen(list) +
                           L"\n\nDisable them in kyubu/config.toml to join vanilla servers.";
        MessageBoxW(g_hwnd, msg.c_str(), L"Kyubu", MB_OK | MB_ICONWARNING);
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    int r = g_connect_next(s, a, len);
    if (r == 0 || WSAGetLastError() == WSAEWOULDBLOCK) {
        g_game_socket = s;
        NetTransport::get().reset();   // a new server: nothing queued for the old one
        if (!g_world.empty()) on_world_exit();
        L().mode = KY_MODE_REMOTE_CLIENT;
        uint16_t port = 0;
        std::string at = addr_text(a, &port);
        KINFO("connected to %s:%u (multiplayer: client side only)", at.c_str(), port);
        KyubuNetConnData d{(uintptr_t)s, at.c_str(), port};
        Events::get().emit("net.connect", KY_SIDE_CLIENT, &d, sizeof d, false, GetCurrentThreadId() != g_main_thread);
        g_world = "remote:" + at;
        game::on_world_enter();
        KyubuWorldData wd{g_world.c_str()};
        Events::get().emit("lifecycle.world_enter", KY_SIDE_CLIENT, &wd, sizeof wd);
    }
    return r;
}

bool is_game_socket(SOCKET s) {
    if (L().host.host == KY_HOST_CLIENT) return s == g_game_socket;
    return std::find(g_clients.begin(), g_clients.end(), s) != g_clients.end();
}

int WSAAPI send_hook(SOCKET s, const char* buf, int len, int flags) {
    if (len > 0 && is_game_socket(s) && Events::get().has_subscribers("net.send_raw", host_side())) {
        std::vector<uint8_t> copy((const uint8_t*)buf, (const uint8_t*)buf + len);
        uint32_t mlen = (uint32_t)len;
        KyubuNetData d{(uintptr_t)s, copy.data(), (uint32_t)len, copy.data(), &mlen};
        Events::get().emit("net.send_raw", host_side(), &d, sizeof d, false, GetCurrentThreadId() != g_main_thread);
        if (mlen == 0) return len;   // dropped by a mod
        if (mlen > copy.size()) mlen = (uint32_t)copy.size();
        int r = g_send_next(s, (const char*)copy.data(), (int)mlen, flags);
        return r == (int)mlen ? len : r;
    }
    return g_send_next(s, buf, len, flags);
}

int WSAAPI recv_hook(SOCKET s, char* buf, int len, int flags) {
    int r = g_recv_next(s, buf, len, flags);
    if (r > 0 && is_game_socket(s) && Events::get().has_subscribers("net.recv_raw", host_side())) {
        KyubuNetData d{(uintptr_t)s, (const uint8_t*)buf, (uint32_t)r, nullptr, nullptr};
        Events::get().emit("net.recv_raw", host_side(), &d, sizeof d, false, GetCurrentThreadId() != g_main_thread);
    }
    return r;
}

int WSAAPI close_hook(SOCKET s) {
    if (L().host.host == KY_HOST_CLIENT && s == g_game_socket) {
        KyubuNetConnData d{(uintptr_t)s, "", 0};
        Events::get().emit("net.disconnect", KY_SIDE_CLIENT, &d, sizeof d, false, GetCurrentThreadId() != g_main_thread);
        if (!g_world.empty()) {
            KyubuWorldData wd{g_world.c_str()};
            Events::get().emit("lifecycle.world_exit", KY_SIDE_CLIENT, &wd, sizeof wd);
            game::on_world_exit();
            g_world.clear();
        }
        g_game_socket = INVALID_SOCKET;
        NetTransport::get().reset();
        L().mode = KY_MODE_MENU_OR_SINGLEPLAYER;
        KINFO("disconnected from server");
    } else if (L().host.host == KY_HOST_SERVER) {
        auto it = std::find(g_clients.begin(), g_clients.end(), s);
        if (it != g_clients.end()) {
            g_clients.erase(it);
            NetTransport::get().forget_socket((uintptr_t)s);
            KyubuNetConnData d{(uintptr_t)s, "", 0};
            Events::get().emit("net.disconnect", KY_SIDE_SERVER, &d, sizeof d, false, true);
        }
    }
    return g_close_next(s);
}

int WSAAPI listen_hook(SOCKET s, int backlog) {
    int r = g_listen_next(s, backlog);
    if (r == 0) runtime_on_game_ready();
    return r;
}

SOCKET WSAAPI accept_hook(SOCKET s, sockaddr* a, int* len) {
    SOCKET c = g_accept_next(s, a, len);
    if (c != INVALID_SOCKET) {
        g_clients.push_back(c);
        uint16_t port = 0;
        // The game may pass no address buffer to accept(); ask the socket for its peer instead.
        sockaddr_in peer{};
        int plen = sizeof peer;
        const sockaddr* pa = a && a->sa_family == AF_INET ? a
                             : (getpeername(c, (sockaddr*)&peer, &plen) == 0 ? (const sockaddr*)&peer : nullptr);
        std::string at = addr_text(pa, &port);
        KyubuNetConnData d{(uintptr_t)c, at.c_str(), port};
        Events::get().emit("net.connect", KY_SIDE_SERVER, &d, sizeof d, false, true);
    }
    return c;
}

// ------------------------------------------------------------------ exit
using ExitProcessFn = void(WINAPI*)(UINT);
ExitProcessFn g_exit_next = nullptr;
void WINAPI exit_hook(UINT code) {
    static LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) {
        if (!g_world.empty() && L().mode == KY_MODE_MENU_OR_SINGLEPLAYER) on_world_exit();
        runtime_shutdown();
    }
    g_exit_next(code);
}

}  // namespace

// ------------------------------------------------------------------ called by the asset overlay's DB hooks
void on_database_opened(void* obj, const std::string& path, bool ok) {
    KyubuDbOpenData d{path.c_str()};
    Events::get().emit("asset.db_open", host_side(), &d, sizeof d);
    std::string p = lower(path);
    for (auto& c : p) if (c == '\\') c = '/';
    if (ends_with(p, "data1.db")) Models::get().on_data1_opened();
    auto pos = p.find("save/world_");
    if (ok && pos != std::string::npos && ends_with(p, ".db")) {
        std::string world = path.substr(pos + 11, path.size() - pos - 11 - 3);
        void* handle = *(void**)((char*)obj + 4);
        if (world != g_world) {
            if (!g_world.empty()) on_world_exit();
            g_world_handle = handle;
            on_world_enter(world);
        } else {
            g_world_handle = handle;
        }
    }
}

void on_sqlite_closed(void* handle) {
    if (handle && handle == g_world_handle) on_world_exit();
}

const std::string& current_world() { return g_world; }
bool world_loaded() { return !g_world.empty(); }
HWND game_window() { return g_hwnd; }
bool on_main_thread() { return g_main_thread != 0 && GetCurrentThreadId() == g_main_thread; }

void install_common_hooks() {
    g_main_thread = GetCurrentThreadId();
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto ep = (uintptr_t)GetProcAddress(k32, "ExitProcess");
    std::string err;
    if (ep && Hooks::get().hook_function("kyubu", ep, (void*)&exit_hook, kLoaderPriority, (void**)&g_exit_next, nullptr, &err) != KY_OK)
        KWARN("cannot hook ExitProcess (%s): lifecycle.shutdown will not be emitted", err.c_str());
    hook_iat("WS2_32.dll", "send", &send_hook, &g_send_next);
    hook_iat("WS2_32.dll", "recv", &recv_hook, &g_recv_next);
    hook_iat("WS2_32.dll", "closesocket", &close_hook, &g_close_next);
}

void install_client_hooks() {
    hook_sym("Game::frame", &frame_hook, &g_frame_next);
    if (!hook_sym("GameController::update", &update_hook, &g_update_next))
        Events::get().set_unavailable("sim.tick", "GameController::update is not a verified symbol on this build");
    hook_iat("d3d9.dll", "Direct3DCreate9", &d3dcreate_hook, &g_d3dcreate_next);
    hook_iat("USER32.dll", "CreateWindowExW", &createwindow_hook, &g_createwindow_next);
    hook_iat("DINPUT8.dll", "DirectInput8Create", &di8create_hook, &g_di8_next);
    hook_iat("USER32.dll", "SetCursorPos", &setcursorpos_hook, &g_setcursorpos_next);
    if (headless::enabled()) {
        hook_iat("USER32.dll", "ShowWindow", &showwindow_hook, &g_showwindow_next);
        hook_iat("USER32.dll", "GetFocus", &getfocus_hook, &g_getfocus_next);
        hook_iat("USER32.dll", "SetFocus", &setfocus_hook, &g_setfocus_next);
        hook_iat("USER32.dll", "GetCursorPos", &getcursorpos_hook, &g_getcursorpos_next);
    }
    hook_iat("WS2_32.dll", "connect", &connect_hook, &g_connect_next);
}

void install_server_hooks() {
    hook_iat("WS2_32.dll", "listen", &listen_hook, &g_listen_next);
    hook_iat("WS2_32.dll", "accept", &accept_hook, &g_accept_next);
    // sim.tick: the game API's cube::World::update detour emits it, once per tick (game/world.cpp).
}

}  // namespace kyubu
