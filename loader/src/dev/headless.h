// Headless mode (add-native-replay, design D3 + D5): the game runs in a window nobody sees and takes no input
// from the real keyboard or mouse. Dev mode only. This file holds the rules (pure, unit-tested) and the state the
// hooks in events/game_hooks.cpp use, and the input queue that fills the synthetic state.
#pragma once


#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace kyubu::headless {

// --kyubu-headless is honoured only in dev mode; otherwise it is refused (and logged by the caller).
enum class FlagResult { Off, On, Refused };
FlagResult resolve_flag(bool flag_given, bool dev_mode);

// Set once at boot from resolve_flag. All hooks test enabled() first.
void set_enabled(bool on);
bool enabled();

// Window procedure filter (D5): true = drop the message. Real keyboard and mouse messages, and focus loss.
// Synthetic messages never pass here: they call the original window procedure directly.
bool drops_message(UINT msg, WPARAM w);

// Window style for the game window (D3, placement b): no-activate tool window.
DWORD window_ex_style(DWORD ex);
// Top-left of the window: right of the virtual screen, so no monitor shows it.
POINT window_origin(int virtual_left, int virtual_top, int virtual_width);
constexpr int kClientWidth = 1024;
constexpr int kClientHeight = 768;

// Synthetic DirectInput state. Keyboard: 256 bytes indexed by DIK_ scan code. Mouse: buttons and relative motion.
void keyboard_state(unsigned char out[256]);
// Fills a DIMOUSESTATE (cb 16) or DIMOUSESTATE2 (cb 20): lX/lY = virtual cursor change since the last read, lZ = wheel since the last read.
void fill_mouse(void* data, unsigned cb);
void set_scan(int dik, bool down);        // used by the input queue
void set_button(int index, bool down);    // used by the input queue
void add_wheel(long delta);               // used by the input queue

// Virtual cursor, in client coordinates. Starts at the middle of the client area.
void cursor_reset(int client_w, int client_h);
void cursor_set(int x, int y);
// SetCursorPos from the game: moves the cursor with no mouse motion (a warp is no DirectInput delta; without this the re-centre at
// world entry would turn the camera by the click position).
void cursor_warp(int x, int y);
POINT cursor_get();

// ---- Input files (design D4) -------------------------------------------------------------------------------------
// One line per event: down <key> | up <key> | button down|up 1|2|3 | move <x> <y> | wheel up|down <ticks> | text <utf-8>.
// Blank lines and lines starting with # are skipped. Key names are xdotool's, case-insensitive.
enum class EvKind { KeyDown, KeyUp, ButtonDown, ButtonUp, Move, Wheel, Text };
struct Event {
    EvKind kind = EvKind::KeyDown;
    int vk = 0, dik = 0;     // key events
    int button = 0;          // 0 left, 1 right, 2 middle
    int x = 0, y = 0;        // move, client coordinates
    int ticks = 0;           // wheel: positive = up (away from the user)
    std::string text;        // text: UTF-8
};
// xdotool key name -> virtual-key code (window messages) and DirectInput scan code (the 256-byte state).
bool key_lookup(std::string_view name, int& vk, int& dik);
// false and err set when the line is bad. out is valid only on true.
bool parse_line(std::string_view line, Event& out, std::string& err);
// Every good line, in order; each bad line adds "line N: reason" to errors (may be null).
std::vector<Event> parse_input(std::string_view text, std::vector<std::string>* errors);

// The one-frame rule: events wait here; next_frame() returns what this frame applies. An `up` whose `down` is in the same
// batch ends the batch (it and everything after it wait for the next frame), so each press is seen by at least one frame.
class InputQueue {
public:
    void push(const std::vector<Event>& ev);
    std::vector<Event> next_frame();
    size_t size() const { return q_.size(); }
private:
    std::vector<Event> q_;
};

// Applies one event: updates the synthetic state (scan code, button, wheel, virtual cursor). The caller then sends the window message.
void apply_state(const Event& e);

// UTF-8 to UTF-16 code units (WM_CHAR payload); bad bytes are skipped.
std::vector<unsigned> utf8_to_units(std::string_view s);

// Dev mode, game thread, every frame: reads and removes the *.input files in `inbox_dir` (sorted by name) into the queue.
// A bad line is logged by the caller through `on_error`. Returns the number of files read.
size_t read_inbox(const std::string& inbox_dir, InputQueue& q, std::vector<std::string>* errors);

// ---- Screenshots and the frame hash (design D6) ------------------------------------------------------------------
// zlib stream writer: returns false on failure. The loader passes the proxy's forwarded `compress`; stored_zlib is the fallback.
using DeflateFn = bool (*)(const unsigned char* in, size_t n, std::vector<unsigned char>& out);
bool stored_zlib(const unsigned char* in, size_t n, std::vector<unsigned char>& out);   // zlib stream of stored blocks
unsigned crc32_of(const unsigned char* p, size_t n, unsigned crc = 0);
unsigned adler32_of(const unsigned char* p, size_t n);
// 8-bit RGB PNG. `bgra` is a locked D3D surface (B,G,R,X per pixel), `pitch` its row size in bytes. `deflate` may be null (stored).
std::vector<unsigned char> png_from_bgra(const unsigned char* bgra, int w, int h, int pitch, DeflateFn deflate);
// FNV-1a over the B,G,R bytes of each pixel (the X byte is ignored).
unsigned frame_hash(const unsigned char* bgra, int w, int h, int pitch);
// A shot name from a .shot file: first line, trimmed, only [A-Za-z0-9._-] kept, at most 80 characters; empty -> `fallback`.
std::string shot_name(std::string_view content, std::string_view fallback);
// Dev mode, game thread: reads and removes the *.shot files in `inbox_dir` (sorted by name); returns the picture names.
std::vector<std::string> read_shots(const std::string& inbox_dir);

// D7: frame counter. tick() is called once per frame with a millisecond clock; every `period_ms` it reports the average rate
// over the period and the lowest instant rate (1000 / longest frame gap), then starts a new period.
struct FpsMeter {
    unsigned long long start_ms = 0, last_ms = 0, frames = 0, worst_gap = 0;
    bool started = false;
    unsigned long long total = 0;   // frames since the first tick
    bool tick(unsigned long long now_ms, double& fps, double& min_fps, unsigned long long period_ms = 5000);
};

}  // namespace kyubu::headless
