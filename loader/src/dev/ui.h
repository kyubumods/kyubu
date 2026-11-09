// Dear ImGui overlay (client): console, diagnostics panel, notices and the mod UI API.
#pragma once
#include "common.h"

struct IDirect3DDevice9;
struct ImGuiInputTextCallbackData;

namespace kyubu {

class Ui {
public:
    static Ui& get();

    void attach_device(IDirect3DDevice9* dev, HWND hwnd);
    bool ready() const { return ready_; }
    void before_reset();
    void after_reset();
    // True after before_reset() until the device objects were created again.
    bool needs_recovery() const { return invalidated_; }
    // The overlay drew a frame recently. Input is only captured while it does, so a stalled overlay
    // (for example after a device reset that bypassed our hooks) can never swallow the game's input.
    bool live() const;

    // Called from the Present hook: builds the ImGui frame, emits render.overlay, renders.
    void render_frame(IDirect3DDevice9* dev);
    // WndProc filter: returns true when the overlay consumed the message.
    bool wndproc(HWND h, UINT msg, WPARAM w, LPARAM l);

    bool frame_active() const { return in_frame_; }
    bool wants_mouse() const;
    bool wants_keyboard() const;
    bool console_open() const { return console_open_; }
    void toggle_console() { console_open_ = !console_open_; }
    void notice(const std::string& text, double seconds = 8.0);

private:
    void draw_console();
    void draw_diagnostics();
    void draw_notices();
    void draw_suggestions(float x, float y, float width);
    void load_command_history();
    void remember_command(const std::string& cmd);
    static int input_callback(ImGuiInputTextCallbackData* data);
    void on_history_key(ImGuiInputTextCallbackData* data, bool up);
    void on_tab(ImGuiInputTextCallbackData* data);

    bool ready_ = false;
    bool in_frame_ = false;
    bool console_open_ = false;
    bool diag_open_ = false;
    HWND hwnd_ = nullptr;
    char input_[512] = {};
    bool scroll_to_bottom_ = true;
    // Commands typed in the console, oldest first (kept in <kyubu>/console_history.txt between runs). Up and Down walk it;
    // history_pos_ is -1 while the line is the one being typed, which history_draft_ keeps while walking.
    std::vector<std::string> command_history_;
    bool history_loaded_ = false;
    int history_pos_ = -1;
    std::string history_draft_;
    struct Notice { std::string text; uint64_t until; };
    std::vector<Notice> notices_;
    double frame_ms_ = 0;
    uint64_t last_frame_ = 0;
    bool invalidated_ = false;
};

}  // namespace kyubu
