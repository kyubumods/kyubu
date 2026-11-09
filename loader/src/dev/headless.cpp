#define DIRECTINPUT_VERSION 0x0800
#include "headless.h"

#include <dinput.h>

#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>

namespace kyubu::headless {

namespace {
std::atomic<bool> g_on{false};
unsigned char g_keys[256] = {};
unsigned char g_buttons[8] = {};
long g_wheel = 0;
POINT g_cursor{0, 0};
POINT g_cursor_read{0, 0};
bool g_cursor_read_valid = false;
}  // namespace

FlagResult resolve_flag(bool flag_given, bool dev_mode) {
    if (!flag_given) return FlagResult::Off;
    return dev_mode ? FlagResult::On : FlagResult::Refused;
}

void set_enabled(bool on) { g_on = on; }
bool enabled() { return g_on; }

bool drops_message(UINT msg, WPARAM w) {
    switch (msg) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR:
        case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_KILLFOCUS:
            return true;
        case WM_ACTIVATE: return LOWORD(w) == WA_INACTIVE;
        case WM_ACTIVATEAPP: return w == FALSE;
        default: return false;   // WM_SETFOCUS, WM_SIZE and the rest reach the game (the device reset needs them)
    }
}

DWORD window_ex_style(DWORD ex) { return ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW; }

POINT window_origin(int vl, int vt, int vw) { return POINT{vl + vw + 200, vt}; }

void keyboard_state(unsigned char out[256]) { std::memcpy(out, g_keys, 256); }

void fill_mouse(void* data, unsigned cb) {
    if (!data || cb < 16) return;
    long* l = (long*)data;
    l[0] = g_cursor_read_valid ? g_cursor.x - g_cursor_read.x : 0;
    l[1] = g_cursor_read_valid ? g_cursor.y - g_cursor_read.y : 0;
    l[2] = g_wheel;
    g_wheel = 0;
    g_cursor_read = g_cursor;
    g_cursor_read_valid = true;
    unsigned char* b = (unsigned char*)(l + 3);
    unsigned n = std::min<unsigned>(cb - 12, 8);
    std::memcpy(b, g_buttons, n);
}

void set_scan(int dik, bool down) { if (dik >= 0 && dik < 256) g_keys[dik] = down ? 0x80 : 0; }
void set_button(int i, bool down) { if (i >= 0 && i < 8) g_buttons[i] = down ? 0x80 : 0; }
void add_wheel(long d) { g_wheel += d; }

void cursor_reset(int w, int h) { g_cursor = POINT{w / 2, h / 2}; g_cursor_read = g_cursor; g_cursor_read_valid = false; }
void cursor_set(int x, int y) { g_cursor = POINT{x, y}; }
void cursor_warp(int x, int y) { g_cursor = POINT{x, y}; g_cursor_read = g_cursor; }
POINT cursor_get() { return g_cursor; }

// ---- key table ---------------------------------------------------------------------------------------------------
namespace {
struct KeyRow { const char* name; int vk; int dik; };
const KeyRow kKeys[] = {
    {"space", VK_SPACE, DIK_SPACE}, {"return", VK_RETURN, DIK_RETURN}, {"enter", VK_RETURN, DIK_RETURN},
    {"escape", VK_ESCAPE, DIK_ESCAPE}, {"esc", VK_ESCAPE, DIK_ESCAPE}, {"tab", VK_TAB, DIK_TAB},
    {"backspace", VK_BACK, DIK_BACK}, {"delete", VK_DELETE, DIK_DELETE}, {"insert", VK_INSERT, DIK_INSERT},
    {"home", VK_HOME, DIK_HOME}, {"end", VK_END, DIK_END}, {"prior", VK_PRIOR, DIK_PRIOR}, {"page_up", VK_PRIOR, DIK_PRIOR},
    {"next", VK_NEXT, DIK_NEXT}, {"page_down", VK_NEXT, DIK_NEXT},
    {"up", VK_UP, DIK_UP}, {"down", VK_DOWN, DIK_DOWN}, {"left", VK_LEFT, DIK_LEFT}, {"right", VK_RIGHT, DIK_RIGHT},
    {"shift", VK_SHIFT, DIK_LSHIFT}, {"shift_l", VK_SHIFT, DIK_LSHIFT}, {"shift_r", VK_SHIFT, DIK_RSHIFT},
    {"ctrl", VK_CONTROL, DIK_LCONTROL}, {"control", VK_CONTROL, DIK_LCONTROL}, {"control_l", VK_CONTROL, DIK_LCONTROL},
    {"control_r", VK_CONTROL, DIK_RCONTROL},
    {"alt", VK_MENU, DIK_LMENU}, {"alt_l", VK_MENU, DIK_LMENU}, {"alt_r", VK_MENU, DIK_RMENU},
    {"grave", VK_OEM_3, DIK_GRAVE}, {"minus", VK_OEM_MINUS, DIK_MINUS}, {"equal", VK_OEM_PLUS, DIK_EQUALS},
    {"bracketleft", VK_OEM_4, DIK_LBRACKET}, {"bracketright", VK_OEM_6, DIK_RBRACKET}, {"backslash", VK_OEM_5, DIK_BACKSLASH},
    {"semicolon", VK_OEM_1, DIK_SEMICOLON}, {"apostrophe", VK_OEM_7, DIK_APOSTROPHE}, {"comma", VK_OEM_COMMA, DIK_COMMA},
    {"period", VK_OEM_PERIOD, DIK_PERIOD}, {"slash", VK_OEM_2, DIK_SLASH},
};
const int kDikLetters[26] = {DIK_A, DIK_B, DIK_C, DIK_D, DIK_E, DIK_F, DIK_G, DIK_H, DIK_I, DIK_J, DIK_K, DIK_L, DIK_M,
                             DIK_N, DIK_O, DIK_P, DIK_Q, DIK_R, DIK_S, DIK_T, DIK_U, DIK_V, DIK_W, DIK_X, DIK_Y, DIK_Z};
const int kDikDigits[10] = {DIK_0, DIK_1, DIK_2, DIK_3, DIK_4, DIK_5, DIK_6, DIK_7, DIK_8, DIK_9};
const int kDikF[12] = {DIK_F1, DIK_F2, DIK_F3, DIK_F4, DIK_F5, DIK_F6, DIK_F7, DIK_F8, DIK_F9, DIK_F10, DIK_F11, DIK_F12};
}  // namespace

bool key_lookup(std::string_view name, int& vk, int& dik) {
    std::string n = lower(std::string(name));
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') { vk = 'A' + (n[0] - 'a'); dik = kDikLetters[n[0] - 'a']; return true; }
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') { vk = n[0]; dik = kDikDigits[n[0] - '0']; return true; }
    if (n.size() >= 2 && n.size() <= 3 && n[0] == 'f') {
        bool digits = std::all_of(n.begin() + 1, n.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
        int f = digits ? std::atoi(n.c_str() + 1) : 0;
        if (f >= 1 && f <= 12) { vk = VK_F1 + (f - 1); dik = kDikF[f - 1]; return true; }
        return false;
    }
    for (const auto& r : kKeys)
        if (n == r.name) { vk = r.vk; dik = r.dik; return true; }
    return false;
}

// ---- parser ------------------------------------------------------------------------------------------------------
namespace {
bool parse_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (*end || v < -100000 || v > 100000) return false;
    out = (int)v;
    return true;
}
std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> w;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
        if (j > i) w.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return w;
}
}  // namespace

bool parse_line(std::string_view line, Event& out, std::string& err) {
    std::string t = trim(line);
    Event e;
    if (t.compare(0, 5, "text ") == 0 || t == "text") {   // the rest of the line is the text, spaces kept
        e.kind = EvKind::Text;
        e.text = t.size() > 5 ? t.substr(5) : "";
        if (e.text.empty()) { err = "text needs a string"; return false; }
        out = e;
        return true;
    }
    auto w = words(t);
    if (w.empty()) { err = "empty line"; return false; }
    const std::string cmd = lower(w[0]);
    if (cmd == "down" || cmd == "up") {
        if (w.size() != 2) { err = cmd + " needs one key name"; return false; }
        if (!key_lookup(w[1], e.vk, e.dik)) { err = "unknown key '" + w[1] + "'"; return false; }
        e.kind = cmd == "down" ? EvKind::KeyDown : EvKind::KeyUp;
    } else if (cmd == "button") {
        int b = 0;
        if (w.size() != 3 || (lower(w[1]) != "down" && lower(w[1]) != "up") || !parse_int(w[2], b) || b < 1 || b > 3) {
            err = "usage: button down|up 1|2|3";
            return false;
        }
        e.kind = lower(w[1]) == "down" ? EvKind::ButtonDown : EvKind::ButtonUp;
        e.button = b - 1;
    } else if (cmd == "move") {
        if (w.size() != 3 || !parse_int(w[1], e.x) || !parse_int(w[2], e.y)) { err = "usage: move <x> <y>"; return false; }
        e.kind = EvKind::Move;
    } else if (cmd == "wheel") {
        int n = 0;
        if (w.size() != 3 || (lower(w[1]) != "up" && lower(w[1]) != "down") || !parse_int(w[2], n) || n < 1) {
            err = "usage: wheel up|down <ticks>";
            return false;
        }
        e.kind = EvKind::Wheel;
        e.ticks = lower(w[1]) == "up" ? n : -n;
    } else {
        err = "unknown command '" + w[0] + "'";
        return false;
    }
    out = e;
    return true;
}

std::vector<Event> parse_input(std::string_view text, std::vector<std::string>* errors) {
    std::vector<Event> evs;
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.remove_prefix(3);
    int n = 0;
    for (auto& raw : split(text, '\n')) {
        ++n;
        std::string line = trim(raw);   // also drops a trailing \r
        if (line.empty() || line[0] == '#') continue;
        Event e;
        std::string err;
        if (parse_line(line, e, err)) evs.push_back(std::move(e));
        else if (errors) errors->push_back("line " + std::to_string(n) + ": " + err);
    }
    return evs;
}

// ---- queue -------------------------------------------------------------------------------------------------------
void InputQueue::push(const std::vector<Event>& ev) { q_.insert(q_.end(), ev.begin(), ev.end()); }

std::vector<Event> InputQueue::next_frame() {
    std::vector<Event> batch;
    bool key_down[256] = {};
    bool btn_down[3] = {};
    size_t i = 0;
    for (; i < q_.size(); ++i) {
        const Event& e = q_[i];
        if (e.kind == EvKind::KeyUp && key_down[e.dik & 255]) break;
        if (e.kind == EvKind::ButtonUp && btn_down[e.button]) break;
        if (e.kind == EvKind::KeyDown) key_down[e.dik & 255] = true;
        if (e.kind == EvKind::ButtonDown) btn_down[e.button] = true;
        batch.push_back(e);
    }
    q_.erase(q_.begin(), q_.begin() + i);
    return batch;
}

void apply_state(const Event& e) {
    switch (e.kind) {
        case EvKind::KeyDown: set_scan(e.dik, true); break;
        case EvKind::KeyUp: set_scan(e.dik, false); break;
        case EvKind::ButtonDown: set_button(e.button, true); break;
        case EvKind::ButtonUp: set_button(e.button, false); break;
        case EvKind::Move: cursor_set(e.x, e.y); break;
        case EvKind::Wheel: add_wheel(120L * e.ticks); break;
        case EvKind::Text: break;
    }
}

std::vector<unsigned> utf8_to_units(std::string_view s) {
    std::vector<unsigned> u;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp = 0;
        size_t len = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > s.size()) break;
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { ++i; continue; }
        i += len;
        if (cp >= 0x10000) { cp -= 0x10000; u.push_back(0xD800 + (cp >> 10)); u.push_back(0xDC00 + (cp & 0x3FF)); }
        else u.push_back(cp);
    }
    return u;
}

size_t read_inbox(const std::string& dir, InputQueue& q, std::vector<std::string>* errors) {
    if (!dir_exists(dir)) return 0;
    std::vector<std::string> names;
    for (auto& n : list_dir(dir))
        if (n.size() > 6 && n[0] != '.' && ends_with(lower(n), ".input")) names.push_back(n);
    std::sort(names.begin(), names.end());
    size_t read = 0;
    for (auto& n : names) {
        std::string path = join_path(dir, n);
        auto data = read_file(path);
        if (!data) continue;
        if (!DeleteFileW(widen(path).c_str())) continue;   // removed first, like .cmd
        std::vector<std::string> errs;
        q.push(parse_input(std::string_view((const char*)data->data(), data->size()), &errs));
        if (errors) for (auto& e : errs) errors->push_back(n + " " + e);
        ++read;
    }
    return read;
}

// ---- Screenshots and the frame hash ---------------------------------------------------------------------------------
unsigned crc32_of(const unsigned char* p, size_t n, unsigned crc) {
    static unsigned table[256];
    static bool init = false;
    if (!init) {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

unsigned adler32_of(const unsigned char* p, size_t n) {
    unsigned a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

bool stored_zlib(const unsigned char* in, size_t n, std::vector<unsigned char>& out) {
    out.clear();
    out.push_back(0x78);
    out.push_back(0x01);
    size_t pos = 0;
    do {
        const size_t len = std::min<size_t>(65535, n - pos);
        const bool last = pos + len >= n;
        out.push_back(last ? 1 : 0);
        out.push_back((unsigned char)(len & 0xFF));
        out.push_back((unsigned char)(len >> 8));
        out.push_back((unsigned char)(~len & 0xFF));
        out.push_back((unsigned char)((~len >> 8) & 0xFF));
        out.insert(out.end(), in + pos, in + pos + len);
        pos += len;
    } while (pos < n);
    const unsigned ad = adler32_of(in, n);
    for (int s = 24; s >= 0; s -= 8) out.push_back((unsigned char)(ad >> s));
    return true;
}

namespace {
void put32(std::vector<unsigned char>& v, unsigned x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back((unsigned char)(x >> s));
}
void chunk(std::vector<unsigned char>& png, const char* type, const std::vector<unsigned char>& data) {
    put32(png, (unsigned)data.size());
    const size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    put32(png, crc32_of(png.data() + start, png.size() - start));
}
}  // namespace

std::vector<unsigned char> png_from_bgra(const unsigned char* bgra, int w, int h, int pitch, DeflateFn deflate) {
    std::vector<unsigned char> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);   // filter: none
        const unsigned char* row = bgra + (size_t)y * pitch;
        for (int x = 0; x < w; ++x) {
            raw.push_back(row[x * 4 + 2]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 0]);
        }
    }
    std::vector<unsigned char> z;
    if (!deflate || !deflate(raw.data(), raw.size(), z) || z.empty()) stored_zlib(raw.data(), raw.size(), z);
    std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<unsigned char> ihdr;
    put32(ihdr, (unsigned)w);
    put32(ihdr, (unsigned)h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});   // 8 bit, RGB, deflate, no filter set, no interlace
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

unsigned frame_hash(const unsigned char* bgra, int w, int h, int pitch) {
    unsigned hash = 2166136261u;
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = bgra + (size_t)y * pitch;
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) hash = (hash ^ row[x * 4 + c]) * 16777619u;
    }
    return hash;
}

std::string shot_name(std::string_view content, std::string_view fallback) {
    auto clean = [](std::string_view s) {
        const size_t nlpos = s.find_first_of("\r\n");
        if (nlpos != std::string_view::npos) s = s.substr(0, nlpos);
        std::string r;
        for (char c : s)
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
                r += c;
        if (r.size() > 80) r.resize(80);
        while (!r.empty() && r[0] == '.') r.erase(0, 1);   // no hidden files, no ".."
        return r;
    };
    std::string r = clean(content);
    if (r.empty()) r = clean(fallback);
    if (r.empty()) r = "shot";
    return r;
}

std::vector<std::string> read_shots(const std::string& dir) {
    std::vector<std::string> out;
    if (!dir_exists(dir)) return out;
    std::vector<std::string> names;
    for (auto& n : list_dir(dir))
        if (n.size() > 5 && n[0] != '.' && ends_with(lower(n), ".shot")) names.push_back(n);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        std::string path = join_path(dir, n);
        auto data = read_file(path);
        if (!data) continue;
        if (!DeleteFileW(widen(path).c_str())) continue;
        out.push_back(shot_name(std::string_view((const char*)data->data(), data->size()), n.substr(0, n.size() - 5)));
    }
    return out;
}

bool FpsMeter::tick(unsigned long long now_ms, double& fps, double& min_fps, unsigned long long period_ms) {
    ++total;
    if (!started) { started = true; start_ms = last_ms = now_ms; frames = 0; worst_gap = 0; return false; }
    ++frames;
    const unsigned long long gap = now_ms - last_ms;
    if (gap > worst_gap) worst_gap = gap;
    last_ms = now_ms;
    const unsigned long long span = now_ms - start_ms;
    if (span < period_ms) return false;
    fps = span ? frames * 1000.0 / (double)span : 0.0;
    min_fps = worst_gap ? 1000.0 / (double)worst_gap : fps;
    start_ms = now_ms; frames = 0; worst_gap = 0;
    return true;
}

}  // namespace kyubu::headless
