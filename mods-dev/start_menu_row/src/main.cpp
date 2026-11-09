// start_menu_row: a "Mods" row between Options and Exit on the title screen (add-mod-manager, tasks 2.3 and 2.4, decision D11).
//
// The title screen's rows are not nodes: cube::StartMenuWidget::draw writes each as two text calls (a black 4 px outline,
// then the fill) in the font resource1.dat, at y 30, 80 and 130. The third row is Exit, whose label is the wide string literal
// "Exit" in .rdata that only this draw uses. The mod rewrites that literal to "Mods", so the game's own third row, with its own
// band (y 110 to 140) and cyan hover, is the Mods row. A mid hook at cube::StartMenuWidget::drawEndSite, after that row and with
// the engine's transform for the widget still set (esi is the widget, edi the font), draws "Exit" with the same two calls at y 180
// and sets hover over the band y 160 to 190. The game's click on hover 2 would now quit from the Mods row, so the hook turns hover 2
// into 3 (Mods) and the Exit band into 4: the game ignores both on a click, and this mod's mouse handler acts on them (Exit
// sets cube::GameController.quit_requested, +0x198, as the game's own click does). The hook runs only while the game draws the widget, so on the options panel and in character creation
// nothing is drawn, and a click is accepted only soon after a draw.
//
// Task 2.5 (D9): a click on Mods opens an empty page in the game's style, the way Options replaces the title. The page is
// built once from the game's own nodes with plasma::Node::clone: a blackwidget panel (the kept template), a Back button and a
// Test button (clones of character creation's Back button, whose clones carry no click connection). Everything that touches
// the scene runs in a mid hook at cube::GameController::startMenuShownSite, in the game's update right after it shows the logo
// and the menu again: while the page is open the hook hides both, so the menu is never drawn. The mouse handler only queues
// a click; the hook tests it against the page's buttons. Back closes the page and the title comes back.
//
//   start_menu_row:state   prints the draw and click counters (a replay reads them from the log)
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "kyubu.hpp"

namespace {

kyubu::Mod* g_mod = nullptr;

constexpr uint32_t kHoverOffset = 0x160;   // cube::StartMenuWidget.hover
constexpr uint32_t kQuitOffset = 0x1A0;     // cube::GameController.quit_requested (u8)
constexpr float kRowY = 180.0f;            // the rows are at 30, 80, 130 in the widget's frame
constexpr float kBandTop = 160.0f, kBandBottom = 190.0f;
constexpr DWORD kRecentMs = 500;           // a draw this long ago still counts as "the menu is showing"

// plasma::ScalableFont::drawText: thiscall on the font, 13 stack arguments, popped by the callee (task 2.2).
using DrawTextFn = void(__attribute__((thiscall)) *)(void* font, const void* text, float x, float y, float size, float outline, float glyph_space,
                                                      float line_space, float wrap, const float* fill, const float* outline_colour,
                                                      const float* third_colour, int flags, int snap);
using GetCursorFn = void(__attribute__((thiscall)) *)(void* widget, float* out_xy);
using GetWidthFn = float(__attribute__((thiscall)) *)(void* widget);

DrawTextFn g_draw_text = nullptr;
GetCursorFn g_get_cursor = nullptr;
GetWidthFn g_get_width = nullptr;

// The game's std::wstring (MSVC 2012): 8 wchar_t of buffer, then size and capacity. Four characters fit in the buffer.
struct WString {
    wchar_t buf[8];
    uint32_t size;
    uint32_t capacity;
};
WString g_text = {L"Exit", 4, 7};

volatile uint32_t g_draws = 0;
volatile bool g_open = false;               // the Mods page (task 2.5) is showing
volatile uint32_t g_draws_while_open = 0;  // menu draws while the page is open (should stay 0)
volatile uint32_t g_clicks = 0;        // clicks on Mods
volatile uint32_t g_exit_clicks = 0;
volatile DWORD g_last_draw_tick = 0;
volatile uintptr_t g_widget = 0;
volatile int g_hover_seen = -1;   // the hover this mod set at the last draw (3 or -1)

void KYUBU_CALL draw_site(KyubuCpuContext* ctx, void*) {
    void* widget = reinterpret_cast<void*>(ctx->esi);
    void* font = reinterpret_cast<void*>(ctx->edi);
    if (!widget || !font) return;
    int32_t* hover = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(widget) + kHoverOffset);

    float cursor[2] = {-1.0f, -1.0f};
    g_get_cursor(widget, cursor);
    const float width = g_get_width(widget);
    const bool over = cursor[0] >= 0.0f && cursor[0] < width && cursor[1] >= kBandTop && cursor[1] < kBandBottom;
    if (*hover == 2) *hover = 3;   // the game's third row now reads Mods: its click must not quit
    else if (over) *hover = 4;

    static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const float cyan[4] = {0.2f, 1.0f, 1.0f, 1.0f};
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float x = width * 0.5f;
    g_draw_text(font, &g_text, x, kRowY, 18.0f, 4.0f, 0.0f, 0.0f, 0.0f, white, black, zero, 1, 1);   // the outline
    g_draw_text(font, &g_text, x, kRowY, 18.0f, 0.0f, 0.0f, 0.0f, 0.0f, over ? cyan : white, zero, zero, 1, 1);   // the fill

    g_widget = reinterpret_cast<uintptr_t>(widget);
    g_hover_seen = *hover;
    g_last_draw_tick = GetTickCount();
    g_draws = g_draws + 1;
    if (g_open) g_draws_while_open = g_draws_while_open + 1;
}

// --- the page (task 2.5) ------------------------------------------------------------------------------------------------

constexpr uint32_t kStartScene = 0x800874;      // cube::GameController.start_scene
constexpr uint32_t kLogo = 0x800878;            // cube::GameController.logo
constexpr uint32_t kStartMenuNode = 0x800918;   // cube::GameController.start_menu_node
constexpr uint32_t kBlackwidget = 0x8008C8;     // cube::GameController.blackwidget_template
constexpr uint32_t kCreationBack = 0x80099C;    // cube::GameController.creation_back_button
constexpr uint32_t kWindowW = 0x11C, kWindowH = 0x120;
constexpr uint32_t kNodeDisplay = 0x3C, kNodeWidget = 0x40;   // plasma::Node.display, widget
constexpr uint32_t kDisplayKey = 0x68, kDisplayValues = 0x94; // plasma::Display.key, visible_values
constexpr float kFrameW = 600.0f, kFrameH = 300.0f, kButtonW = 200.0f, kButtonH = 40.0f;

using CloneFn = void*(__attribute__((thiscall)) *)(void* node, void* parent);
using SetTextFn = void(__attribute__((thiscall)) *)(void* node, const void* text);
using SetXYFn = void(__attribute__((thiscall)) *)(void* widget, float x, float y, int update);
using GetSizeFn = float(__attribute__((thiscall)) *)(void* widget);
using FindChildFn = void*(__attribute__((thiscall)) *)(void* node, const void* name);

CloneFn g_clone = nullptr;
SetTextFn g_set_text = nullptr;
SetXYFn g_set_position = nullptr, g_set_size = nullptr;
GetSizeFn g_size_w = nullptr, g_size_h = nullptr;
FindChildFn g_find_child = nullptr;

WString g_test_text = {L"Test", 4, 7};
// names longer than 7 characters: the game's std::wstring then points at its text (capacity 8 or more)
struct WStringPtr {
    const wchar_t* ptr;
    uint32_t pad[3];
    uint32_t size;
    uint32_t capacity;
};
WStringPtr g_left_name = {L"leftbutton", {0, 0, 0}, 10, 10};
WStringPtr g_right_name = {L"rightbutton", {0, 0, 0}, 11, 11};
constexpr uint32_t kOptionsPanel = 0x800A00;   // cube::GameController.options_panel

uintptr_t g_gc_global = 0;
void* g_frame = nullptr;
void* g_back = nullptr;
void* g_test = nullptr;
void* g_left = nullptr;
void* g_right = nullptr;
volatile bool g_open_request = false;
volatile bool g_click_pending = false;
volatile uint32_t g_page_frames = 0;       // updates with the page open
volatile uint32_t g_menu_seen_shown = 0;   // updates where the game had just shown the menu again (it does so every frame)
volatile uint32_t g_back_clicks = 0, g_test_clicks = 0, g_opens = 0, g_left_clicks = 0, g_right_clicks = 0;
volatile int g_build_failed = 0;
bool g_page_ok = false;   // every page symbol resolved and the update site is hooked

int32_t* visible_word(void* node) {
    const uintptr_t display = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(node) + kNodeDisplay);
    if (!display) return nullptr;
    const int32_t key = *reinterpret_cast<int32_t*>(display + kDisplayKey);
    int32_t* values = *reinterpret_cast<int32_t**>(display + kDisplayValues);
    return values ? values + key : nullptr;
}
void set_visible(void* node, bool on) {
    if (int32_t* w = node ? visible_word(node) : nullptr) *w = on ? 1 : 0;
}
void* widget_of(void* node) { return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(node) + kNodeWidget); }
uintptr_t field(uintptr_t gc, uint32_t off) { return *reinterpret_cast<uintptr_t*>(gc + off); }

bool cursor_in(void* node) {
    void* w = node ? widget_of(node) : nullptr;
    if (!w) return false;
    float c[2] = {-1.0f, -1.0f};
    g_get_cursor(w, c);
    return c[0] >= 0.0f && c[1] >= 0.0f && c[0] < g_size_w(w) && c[1] < g_size_h(w);
}

bool build_page(uintptr_t gc) {
    void* scene = reinterpret_cast<void*>(field(gc, kStartScene));
    void* black = reinterpret_cast<void*>(field(gc, kBlackwidget));
    void* back = reinterpret_cast<void*>(field(gc, kCreationBack));
    if (!scene || !black || !back) return false;
    g_frame = g_clone(black, scene);
    g_back = g_clone(back, scene);
    g_test = g_frame ? g_clone(back, g_frame) : nullptr;
    if (!g_frame || !g_back || !g_test || !widget_of(g_frame) || !widget_of(g_back) || !widget_of(g_test)) return false;
    const float w = static_cast<float>(*reinterpret_cast<int32_t*>(gc + kWindowW));
    const float h = static_cast<float>(*reinterpret_cast<int32_t*>(gc + kWindowH));
    g_set_size(widget_of(g_frame), kFrameW, kFrameH, 1);
    g_set_position(widget_of(g_frame), (w - kFrameW) * 0.5f, (h - kFrameH) * 0.5f, 1);
    g_set_position(widget_of(g_back), 20.0f, 20.0f, 1);   // where character creation has its Back
    g_set_text(g_test, &g_test_text);
    g_set_size(widget_of(g_test), kButtonW, kButtonH, 1);
    g_set_position(widget_of(g_test), (kFrameW - kButtonW) * 0.5f, (kFrameH - kButtonH) * 0.5f, 1);
    // a clone copies its source's visible value: character creation's Back is hidden at the title, so its clones start hidden
    const int32_t* tv = visible_word(g_test);
    g_mod->info("start_menu_row: cloned visible values: frame %d, back %d, test %d (source back %d)", visible_word(g_frame) ? *visible_word(g_frame) : -1,
                visible_word(g_back) ? *visible_word(g_back) : -1, tv ? *tv : -1, visible_word(back) ? *visible_word(back) : -1);
    set_visible(g_test, true);   // shown for good: it is inside the frame, which is shown and hidden as a whole

    // the arrow pickers: their templates are gone after start-up, but the options panel holds clones of them by name
    void* opts = reinterpret_cast<void*>(field(gc, kOptionsPanel));
    void* left_src = opts ? g_find_child(opts, &g_left_name) : nullptr;
    void* right_src = opts ? g_find_child(opts, &g_right_name) : nullptr;
    g_mod->info("start_menu_row: options panel %p: leftbutton %p, rightbutton %p", opts, left_src, right_src);
    if (left_src && right_src) {
        g_left = g_clone(left_src, g_frame);
        g_right = g_clone(right_src, g_frame);
    }
    if (g_left && g_right && widget_of(g_left) && widget_of(g_right)) {
        const float lw = g_size_w(widget_of(g_left)), lh = g_size_h(widget_of(g_left));
        const float rw = g_size_w(widget_of(g_right)), rh = g_size_h(widget_of(g_right));
        g_mod->info("start_menu_row: arrow sizes: left %.0f x %.0f, right %.0f x %.0f; visible left %d right %d", lw, lh, rw, rh,
                    visible_word(g_left) ? *visible_word(g_left) : -1, visible_word(g_right) ? *visible_word(g_right) : -1);
        g_set_position(widget_of(g_left), 170.0f - lw * 0.5f, 150.0f - lh * 0.5f, 1);    // centred left of Test
        g_set_position(widget_of(g_right), 430.0f - rw * 0.5f, 150.0f - rh * 0.5f, 1);   // and right of it
        set_visible(g_left, true);
        set_visible(g_right, true);
    } else {
        g_left = g_right = nullptr;
    }
    return true;
}

void KYUBU_CALL shown_site(KyubuCpuContext*, void*) {
    uint32_t gc32 = 0;
    const uintptr_t global = g_gc_global;
    if (!global || g_mod->api()->mem_read(global, &gc32, sizeof gc32) != KY_OK || !gc32) return;
    const uintptr_t gc = gc32;
    void* scene = reinterpret_cast<void*>(field(gc, kStartScene));
    int32_t* scene_shown = scene ? visible_word(scene) : nullptr;
    if (!scene_shown) return;

    if (g_open_request) {
        g_open_request = false;
        if (!g_frame && !g_build_failed) {
            if (!build_page(gc)) {
                g_build_failed = 1;
                g_mod->warn("start_menu_row: could not build the page (frame %p, back %p, test %p)", g_frame, g_back, g_test);
            } else {
                g_mod->info("start_menu_row: page built (frame %p, back %p, test %p)", g_frame, g_back, g_test);
            }
        }
        if (g_frame && !g_build_failed && *scene_shown) {
            g_open = true;
            g_opens = g_opens + 1;
            g_draws_while_open = 0;
            g_mod->info("start_menu_row: page opened");
        }
    }
    if (g_open && !*scene_shown) g_open = false;   // the start scene went away (should not happen from the page)

    if (g_open && g_click_pending) {
        g_click_pending = false;
        if (cursor_in(g_back)) {
            g_back_clicks = g_back_clicks + 1;
            g_open = false;
            g_mod->info("start_menu_row: Back clicked, page closed");
        } else if (cursor_in(g_test)) {
            g_test_clicks = g_test_clicks + 1;
            g_mod->info("start_menu_row: Test clicked");
        } else if (cursor_in(g_left)) {
            g_left_clicks = g_left_clicks + 1;
            g_mod->info("start_menu_row: left arrow clicked");
        } else if (cursor_in(g_right)) {
            g_right_clicks = g_right_clicks + 1;
            g_mod->info("start_menu_row: right arrow clicked");
        }
    }
    g_click_pending = false;

    void* menu = reinterpret_cast<void*>(field(gc, kStartMenuNode));
    void* logo = reinterpret_cast<void*>(field(gc, kLogo));
    if (g_open) {
        if (int32_t* m = menu ? visible_word(menu) : nullptr) {
            if (*m) g_menu_seen_shown = g_menu_seen_shown + 1;
        }
        set_visible(menu, false);
        set_visible(logo, false);
        g_page_frames = g_page_frames + 1;
    }
    set_visible(g_frame, g_open);
    set_visible(g_back, g_open);
}

void KYUBU_CALL cmd_state(const char*, KyubuPrintFn print, void* ctx, void*) {
    char line[360];
    std::snprintf(line, sizeof line,
                  "start_menu_row: draws=%u clicks=%u exit_clicks=%u hover=%d last_draw_ms_ago=%lu page_open=%d opens=%u page_frames=%u "
                  "draws_while_open=%u menu_reshown=%u back_clicks=%u test_clicks=%u left_clicks=%u right_clicks=%u build_failed=%d",
                  static_cast<unsigned>(g_draws), static_cast<unsigned>(g_clicks), static_cast<unsigned>(g_exit_clicks), static_cast<int>(g_hover_seen),
                  static_cast<unsigned long>(GetTickCount() - g_last_draw_tick), g_open ? 1 : 0, static_cast<unsigned>(g_opens),
                  static_cast<unsigned>(g_page_frames), static_cast<unsigned>(g_draws_while_open), static_cast<unsigned>(g_menu_seen_shown),
                  static_cast<unsigned>(g_back_clicks), static_cast<unsigned>(g_test_clicks), static_cast<unsigned>(g_left_clicks),
                  static_cast<unsigned>(g_right_clicks), static_cast<int>(g_build_failed));
    print(ctx, line);
    g_mod->info("%s", line);
}

// task 3.6: game_restart from the console; the replay calls it at the start menu
void KYUBU_CALL cmd_restart(const char*, KyubuPrintFn print, void* ctx, void*) {
    const KyubuResult r = g_mod->api()->game_restart(g_mod->handle());
    char line[96];
    std::snprintf(line, sizeof line, "start_menu_row: game_restart returned %d", static_cast<int>(r));
    print(ctx, line);
    g_mod->info("%s", line);
}

}  // namespace

struct StartMenuRow {
    explicit StartMenuRow(kyubu::Mod& m) : mod(m) {
        if (m.side() != KY_SIDE_CLIENT) return;
        g_mod = &m;
        const uintptr_t site = m.symbol("cube::StartMenuWidget::drawEndSite");
        const uintptr_t draw_text = m.symbol("plasma::ScalableFont::drawText");
        const uintptr_t get_cursor = m.symbol("plasma::Widget::getCursor");
        const uintptr_t get_width = m.symbol("plasma::Widget::getWidth");
        if (!site || !draw_text || !get_cursor || !get_width) {
            m.warn("start_menu_row: a start menu symbol is missing (site %p, drawText %p, getCursor %p, getWidth %p): no row", reinterpret_cast<void*>(site),
                   reinterpret_cast<void*>(draw_text), reinterpret_cast<void*>(get_cursor), reinterpret_cast<void*>(get_width));
            return;
        }
        g_draw_text = reinterpret_cast<DrawTextFn>(draw_text);
        g_get_cursor = reinterpret_cast<GetCursorFn>(get_cursor);
        g_get_width = reinterpret_cast<GetWidthFn>(get_width);

        // the game's "Exit" label becomes "Mods" (the exe may be relocated: go through the RVA)
        const uintptr_t literal = m.api()->rva_to_addr(0x71A204 - 0x400000);
        static const wchar_t kExit[5] = {L'E', L'x', L'i', L't', 0};
        static const wchar_t kMods[5] = {L'M', L'o', L'd', L's', 0};
        wchar_t now[5] = {};
        if (!literal || m.api()->mem_read(literal, now, sizeof now) != KY_OK || std::memcmp(now, kExit, sizeof now) != 0) {
            m.warn("start_menu_row: the Exit label is not where the map says: no row");
            return;
        }
        KyubuId id = 0;
        const KyubuResult r = m.api()->hook_mid(m.handle(), site, &draw_site, nullptr, &id);
        if (r != KY_OK) {
            m.warn("start_menu_row: could not hook the draw's end site (result %d): no row", static_cast<int>(r));
            return;
        }
        KyubuId patch_id = 0;
        if (m.api()->patch_bytes(m.handle(), literal, kMods, sizeof kMods, &patch_id) != KY_OK) {
            m.api()->unhook(m.handle(), id);
            m.warn("start_menu_row: could not rewrite the Exit label: no row");
            return;
        }
        m.on("input.mouse_button", [this](KyubuEvent& ev) {
            const KyubuMouseButtonData* d = kyubu::Mod::data<KyubuMouseButtonData>(ev);
            if (!d || d->button != 0 || !d->down) return;
            if (g_open) {   // the page's buttons are tested in the update hook
                g_click_pending = true;
                return;
            }
            // the menu is showing when the draw ran a moment ago; hover is the widget's own word
            if (GetTickCount() - g_last_draw_tick > kRecentMs || !g_widget) return;
            int32_t hover = -1;
            if (mod.api()->mem_read(g_widget + kHoverOffset, &hover, sizeof hover) != KY_OK) return;
            if (hover == 3) {
                g_clicks = g_clicks + 1;
                if (g_page_ok) g_open_request = true;
                mod.info("start_menu_row: click on Mods at %d,%d (draws %u)", d->x, d->y, static_cast<unsigned>(g_draws));
            } else if (hover == 4) {
                g_exit_clicks = g_exit_clicks + 1;
                mod.info("start_menu_row: click on Exit at %d,%d: quit requested", d->x, d->y);
                uint32_t gc = 0;
                const uintptr_t global = mod.symbol("cube::game_controller");
                const uint8_t one = 1;
                if (global && mod.api()->mem_read(global, &gc, sizeof gc) == KY_OK && gc) mod.api()->mem_write(mod.handle(), gc + kQuitOffset, &one, 1);
            }
        });
        // the page (task 2.5): optional; without its symbols the row still works and a click only logs
        g_gc_global = m.symbol("cube::game_controller");
        const uintptr_t shown = m.symbol("cube::GameController::startMenuShownSite");
        g_clone = reinterpret_cast<CloneFn>(m.symbol("plasma::Node::clone"));
        g_set_text = reinterpret_cast<SetTextFn>(m.symbol("plasma::Node::setText"));
        g_set_position = reinterpret_cast<SetXYFn>(m.symbol("plasma::Widget::setPosition"));
        g_set_size = reinterpret_cast<SetXYFn>(m.symbol("plasma::Widget::setSize"));
        g_size_w = reinterpret_cast<GetSizeFn>(m.symbol("plasma::Widget::getSizeWidth"));
        g_size_h = reinterpret_cast<GetSizeFn>(m.symbol("plasma::Widget::getSizeHeight"));
        g_find_child = reinterpret_cast<FindChildFn>(m.symbol("plasma::Node::findChild"));
        if (!g_gc_global || !shown || !g_clone || !g_set_text || !g_set_position || !g_set_size || !g_size_w || !g_size_h || !g_find_child) {
            m.warn("start_menu_row: a page symbol is missing (site %p, clone %p, setText %p, setPosition %p, setSize %p, size %p/%p): no page",
                   reinterpret_cast<void*>(shown), reinterpret_cast<void*>(g_clone), reinterpret_cast<void*>(g_set_text), reinterpret_cast<void*>(g_set_position),
                   reinterpret_cast<void*>(g_set_size), reinterpret_cast<void*>(g_size_w), reinterpret_cast<void*>(g_size_h));
        } else {
            KyubuId shown_id = 0;
            const KyubuResult rs = m.api()->hook_mid(m.handle(), shown, &shown_site, nullptr, &shown_id);
            if (rs != KY_OK) m.warn("start_menu_row: could not hook the update site (result %d): no page", static_cast<int>(rs));
            else g_page_ok = true;
        }
        if (m.api()->register_command(m.handle(), "start_menu_row:state", "start_menu_row:state: print the draw and click counters of the Mods row", cmd_state,
                                      nullptr) != KY_OK)
            m.warn("start_menu_row: could not register the state command");
        if (m.api()->register_command(m.handle(), "start_menu_row:restart", "start_menu_row:restart: call game_restart (task 3.6)", cmd_restart, nullptr) != KY_OK)
            m.warn("start_menu_row: could not register the restart command");
        m.info("start_menu_row: loaded, row hooked at %p, page %s", reinterpret_cast<void*>(site), g_page_ok ? "ready" : "off");
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("start_menu_row", KY_SIDE_CLIENT, StartMenuRow)
