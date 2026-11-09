#include "ui.h"

#include <d3d9.h>
#include <psapi.h>

#include <fstream>

#include "console.h"
#include "events.h"
#include "hooks.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "loader.h"
#include "luart.h"
#include "log.h"
#include "modmanager.h"
#include "registry.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace kyubu {

Ui& Ui::get() {
    static Ui* u = new Ui();
    return *u;
}

void Ui::attach_device(IDirect3DDevice9* dev, HWND hwnd) {
    if (ready_) return;
    hwnd_ = hwnd;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static std::string ini = join_path(L().kyubu_dir, "imgui.ini");
    io.IniFilename = ini.c_str();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(dev);
    ready_ = true;
    KINFO("overlay UI ready (Dear ImGui %s)", IMGUI_VERSION);
}

void Ui::before_reset() {
    if (!ready_) return;
    ImGui_ImplDX9_InvalidateDeviceObjects();
    invalidated_ = true;
}
void Ui::after_reset() {
    if (!ready_) return;
    ImGui_ImplDX9_CreateDeviceObjects();
    invalidated_ = false;
}

bool Ui::live() const { return ready_ && last_frame_ && GetTickCount64() - last_frame_ < 500; }
bool Ui::wants_mouse() const { return live() && (console_open_ || ImGui::GetIO().WantCaptureMouse); }
bool Ui::wants_keyboard() const { return live() && (console_open_ || ImGui::GetIO().WantCaptureKeyboard); }

void Ui::notice(const std::string& text, double seconds) {
    notices_.push_back({text, GetTickCount64() + (uint64_t)(seconds * 1000)});
    KINFO("notice: %s", text.c_str());
}

static bool is_console_key(WPARAM vk) {
    const std::string& k = L().config.console_key;
    if (k.size() >= 2 && (k[0] == 'f' || k[0] == 'F')) {       // "f1" .. "f12"
        int n = atoi(k.c_str() + 1);
        if (n >= 1 && n <= 12) return vk == (WPARAM)(VK_F1 + n - 1);
    }
    if (k == "insert") return vk == VK_INSERT;
    if (k == "home") return vk == VK_HOME;
    return vk == VK_OEM_3;   // "grave": ` ~
}

bool Ui::wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (!ready_) return false;
    if (msg == WM_KEYDOWN && is_console_key(w)) { toggle_console(); return true; }
    // The console key's character arrives after its WM_KEYDOWN, so also after closing: never let it reach the game's chat.
    if (msg == WM_CHAR && (w == '`' || w == '~') && L().config.console_key == "grave") return true;
    if (msg == WM_KEYDOWN && w == VK_F3 && L().dev_mode && (GetKeyState(VK_CONTROL) & 0x8000)) { diag_open_ = !diag_open_; return true; }
    ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
    bool mouse = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
    // Key releases always reach the game: a key held while the overlay opened would otherwise stay down in the
    // controller's key array. A release without a press only clears it.
    bool keyboard = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) && msg != WM_KEYUP && msg != WM_SYSKEYUP;
    if (mouse && wants_mouse()) return true;
    if (keyboard && wants_keyboard()) return true;
    return false;
}

void Ui::render_frame(IDirect3DDevice9* dev) {
    if (!ready_) return;
    uint64_t now = GetTickCount64();
    if (last_frame_) frame_ms_ = frame_ms_ * 0.9 + double(now - last_frame_) * 0.1;
    last_frame_ = now;
    // The game hides the OS cursor in-world and draws its own below the overlay: draw ImGui's cursor on top
    // while the console is open or the pointer is over an overlay window.
    ImGui::GetIO().MouseDrawCursor = console_open_ || ImGui::GetIO().WantCaptureMouse;
    if (invalidated_) after_reset();   // a Reset() that bypassed our hook left the device objects released
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    in_frame_ = true;
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);
    KyubuRenderData rd{dev, vp.Width, vp.Height};
    Events::get().emit("render.overlay", KY_SIDE_CLIENT, &rd, sizeof rd);
    draw_notices();
    if (console_open_) draw_console();
    if (diag_open_) draw_diagnostics();
    in_frame_ = false;
    ImGui::EndFrame();
    ImGui::Render();
    // With nothing to draw the backend would still make a vertex buffer and a state block on the game's device every
    // frame. That device is not thread-safe, and the game's own worker thread creates vertex buffers on it (and does not
    // check the result), so an idle overlay must not touch it.
    ImDrawData* draw = ImGui::GetDrawData();
    if (draw && draw->TotalVtxCount > 0) ImGui_ImplDX9_RenderDrawData(draw);
}

void Ui::draw_notices() {
    uint64_t now = GetTickCount64();
    notices_.erase(std::remove_if(notices_.begin(), notices_.end(), [&](const Notice& n) { return n.until < now; }),
                   notices_.end());
    Loader& l = L();
    std::vector<std::string> lines;
    if (l.safe_mode) lines.push_back("Kyubu SAFE MODE - mods are disabled (" + l.safe_reason + ")");
    for (auto& n : notices_) lines.push_back(n.text);
    if (lines.empty()) return;
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##kyubu_notices", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    for (auto& s : lines) ImGui::TextUnformatted(s.c_str());
    ImGui::End();
}

// ---- console: history (Up/Down), command-name completion (Tab) and the suggestion box

static constexpr size_t kHistoryKept = 200;

static std::string history_path() { return L().kyubu_dir + "/console_history.txt"; }

void Ui::load_command_history() {
    history_loaded_ = true;
    std::ifstream f(history_path());
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) command_history_.push_back(line);
    }
    if (command_history_.size() > kHistoryKept) command_history_.erase(command_history_.begin(), command_history_.end() - kHistoryKept);
}

void Ui::remember_command(const std::string& cmd) {
    history_pos_ = -1;
    if (cmd.empty() || (!command_history_.empty() && command_history_.back() == cmd)) return;
    command_history_.push_back(cmd);
    if (command_history_.size() > kHistoryKept) command_history_.erase(command_history_.begin(), command_history_.end() - kHistoryKept);
    std::ofstream f(history_path(), std::ios::trunc);   // a few hundred short lines: rewritten whole
    for (auto& c : command_history_) f << c << '\n';
}

// Up goes to the previous command, Down to the next one and, past the newest, back to the line that was being typed.
void Ui::on_history_key(ImGuiInputTextCallbackData* data, bool up) {
    if (command_history_.empty()) return;
    const int last = (int)command_history_.size() - 1;
    if (up) {
        if (history_pos_ == -1) { history_draft_ = data->Buf; history_pos_ = last; }
        else if (history_pos_ > 0) --history_pos_;
    } else {
        if (history_pos_ == -1) return;
        if (++history_pos_ > last) history_pos_ = -1;
    }
    const std::string& text = history_pos_ == -1 ? history_draft_ : command_history_[history_pos_];
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, text.c_str());
}

// Tab completes the command name under the cursor (the first word only). One match fills it in; several fill in what they have
// in common and list the matches in the console.
void Ui::on_tab(ImGuiInputTextCallbackData* data) {
    std::string text(data->Buf, data->BufTextLen);
    const size_t word_end = text.find(' ');
    if (word_end != std::string::npos && (size_t)data->CursorPos > word_end) return;   // the cursor is in the arguments
    const std::string token = text.substr(0, word_end == std::string::npos ? text.size() : word_end);
    auto matches = Console::get().complete(token);
    if (matches.empty()) return;
    std::string fill = matches.size() == 1 ? matches[0].name : Console::common_prefix(matches);
    if (matches.size() > 1 && fill.size() <= token.size()) {
        std::string line = "  ";
        for (auto& m : matches) line += m.name + "  ";
        Console::get().append(line);
        scroll_to_bottom_ = true;
        return;
    }
    if (matches.size() == 1 && word_end == std::string::npos) fill += ' ';
    data->DeleteChars(0, (int)token.size());
    data->InsertChars(0, fill.c_str());
}

int Ui::input_callback(ImGuiInputTextCallbackData* data) {
    Ui* ui = (Ui*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) ui->on_history_key(data, data->EventKey == ImGuiKey_UpArrow);
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) ui->on_tab(data);
    return 0;
}

// The box above the input: the commands the first word could become, with their help; once the word is a whole command
// name followed by a space, that command's usage line.
void Ui::draw_suggestions(float x, float y, float width) {
    std::string text = input_;
    if (text.empty()) return;
    const size_t sp = text.find(' ');
    const std::string token = text.substr(0, sp);
    auto matches = Console::get().complete(token);
    if (matches.empty()) return;
    if (sp != std::string::npos) {   // the arguments are being typed: only the command's own usage
        matches.erase(std::remove_if(matches.begin(), matches.end(), [&](const Console::Completion& m) { return m.name != token; }), matches.end());
        if (matches.empty()) return;
    } else if (matches.size() == 1 && matches[0].name == token) {
        return;   // nothing more to offer until a space is typed
    }
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(width, 260));
    ImGui::SetNextWindowBgAlpha(0.92f);
    // A tooltip window, not a plain one: ImGui keeps those above every other window. A plain window sinks behind the
    // console as soon as a click raises the console.
    ImGui::BeginTooltip();
    const size_t shown = std::min<size_t>(matches.size(), 8);
    for (size_t i = 0; i < shown; ++i) {
        ImGui::TextUnformatted(matches[i].name.c_str());
        if (!matches[i].help.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", matches[i].help.c_str());
        }
    }
    if (matches.size() > shown) ImGui::TextDisabled("... %u more (Tab lists them)", (unsigned)(matches.size() - shown));
    else if (sp == std::string::npos) ImGui::TextDisabled("Tab completes, Up/Down walk the history");
    ImGui::EndTooltip();
}

void Ui::draw_console() {
    ImGui::SetNextWindowSize(ImVec2(760, 380), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Kyubu console", &console_open_)) { ImGui::End(); return; }
    float footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##scroll", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    for (auto& l : Console::get().history()) ImGui::TextUnformatted(l.c_str());
    if (scroll_to_bottom_ || ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    scroll_to_bottom_ = false;
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::PushItemWidth(-1);
    if (!history_loaded_) load_command_history();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion;
    const bool entered = ImGui::InputText("##input", input_, sizeof input_, flags, &Ui::input_callback, this);
    const ImVec2 box_min = ImGui::GetItemRectMin();
    const float box_width = ImGui::GetItemRectSize().x;
    if (entered) {
        std::string cmd = trim(input_);
        input_[0] = 0;
        remember_command(cmd);
        Console::get().execute(cmd, nullptr);
        scroll_to_bottom_ = true;
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::PopItemWidth();
    ImGui::End();
    draw_suggestions(box_min.x, box_min.y - 4.0f, box_width);
}

void Ui::draw_diagnostics() {
    ImGui::SetNextWindowSize(ImVec2(520, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Kyubu diagnostics", &diag_open_)) { ImGui::End(); return; }
    ImGui::Text("Frame time: %.2f ms (%.0f FPS)", frame_ms_, frame_ms_ > 0 ? 1000.0 / frame_ms_ : 0.0);
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    ImGui::Text("Process working set: %.1f MB", pmc.WorkingSetSize / 1048576.0);
    ImGui::Text("Lua memory: client %u KB, server %u KB", (unsigned)LuaRuntime::get().memory_kb(KY_SIDE_CLIENT),
                (unsigned)LuaRuntime::get().memory_kb(KY_SIDE_SERVER));
    if (ImGui::BeginTable("mods", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("mod");
        ImGui::TableSetupColumn("state");
        ImGui::TableSetupColumn("ms/frame");
        ImGui::TableSetupColumn("hooks");
        ImGui::TableHeadersRow();
        for (auto& m : ModManager::get().all()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m->id.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mod_state_name(m->state));
            ImGui::TableNextColumn(); ImGui::Text("%.3f", m->ms_last_frame);
            ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)Hooks::get().count(m->id));
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    for (auto& n : Registries::get().names()) {
        uint32_t c = 0;
        Registries::get().count(n, &c);
        ImGui::Text("%-16s %u entries", n.c_str(), c);
    }
    ImGui::End();
}

}  // namespace kyubu
