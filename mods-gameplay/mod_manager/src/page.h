// The Mods page shell (design D9): where each piece sits for a window size, and the page's small state machine (tabs,
// the Actions menu, the confirmation frame, BACK and Escape). No game calls here, so unit tests cover it. Rectangles are in window
// pixels; the glue in main.cpp places the cloned nodes with them and draws the text over them.
#pragma once

#include <algorithm>

namespace page {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool has(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    float right() const { return x + w; }
    float bottom() const { return y + h; }
};

constexpr float kMargin = 20.0f;      // window edge to the frame; BACK sits at (20, 20) like character creation's
constexpr float kPad = 12.0f;         // frame edge to its content
constexpr float kGap = 12.0f;
constexpr float kTextIndent = 30.0f;   // a frame's edge to the text inside it
// The game's plates are at least 67 px tall: setSize with a smaller height reads back 67 (replay mod-page run 2).
constexpr float kPlateH = 67.0f;
constexpr float kTabW = 150.0f, kTabH = 62.0f;   // frames, not plates (a plate is 67): a frame's corners make it about 62 at the least
constexpr float kSubW = 170.0f;   // MODS and MODPACKS, the sub-tabs of INSTALL
constexpr float kBarH = kPlateH + 16.0f;   // the change bar
constexpr float kButtonH = kPlateH;
constexpr float kDiscardW = 170.0f, kApplyW = 300.0f;
constexpr float kActionsW = 150.0f, kActionsH = 62.0f;
constexpr float kMenuW = 240.0f, kMenuRowH = 30.0f, kMenuPad = 8.0f;
constexpr float kSearchH = 56.0f;      // the search box is a frame (a frame is about 56 px at the least)
constexpr float kFilterH = 30.0f, kCountH = 26.0f;
constexpr float kRowH = 32.0f;         // one row of the list
constexpr float kFolderH = 30.0f;      // the Open mods folder line under the rows
constexpr float kConfirmW = 460.0f, kConfirmH = 190.0f, kConfirmButtonW = 130.0f;

struct Layout {
    Rect back;                   // given
    Rect frame;                  // the whole page below BACK
    Rect tab[2];                 // MY MODS, INSTALL
    Rect sub[2];                 // MODS, MODPACKS: right of the tabs, shown on INSTALL only
    Rect list, details;          // the two columns
    Rect bar, discard, apply;    // the change bar and its buttons
    Rect folder;                 // the Open mods folder text, last line of the list column
    Rect search, filter, count, rows;   // the list column: the search frame, the filter picker, the count line, the mod rows
    Rect info;                   // the details column under the header: description, dependencies
    Rect actions;                // right of the item's name in the details header
    Rect menu;                   // the Actions menu under it
    Rect confirm, yes, no;       // the confirmation frame, centred in the window
    float title_x = 0, title_y = 0;   // MODS, centred on x, y the text anchor
    bool tall_enough = true;     // false when the frame is too short for the bar and a list row
};

// `name_w` is the width of the selected item's name (the Actions button follows it); `menu_rows` the number of rows of the menu.
inline Layout compute(int window_w, int window_h, float back_w, float back_h, float name_w, int menu_rows) {
    Layout l;
    const float W = static_cast<float>(window_w), H = static_cast<float>(window_h);
    l.back = {kMargin, kMargin, back_w, back_h};
    const float top = kMargin + back_h + kGap;
    l.frame = {kMargin, top, std::max(0.0f, W - 2 * kMargin), std::max(0.0f, H - top - kMargin)};
    l.title_x = W * 0.5f;
    l.title_y = kMargin + back_h * 0.5f + 8.0f;   // the text's anchor is its baseline: centre it on BACK

    const Rect in = {l.frame.x + kPad, l.frame.y + kPad, std::max(0.0f, l.frame.w - 2 * kPad), std::max(0.0f, l.frame.h - 2 * kPad)};
    l.tab[0] = {in.x, in.y, kTabW, kTabH};
    l.tab[1] = {in.x + kTabW + kGap, in.y, kTabW, kTabH};
    l.sub[0] = {l.tab[1].right() + 3 * kGap, in.y, kSubW, kTabH};
    l.sub[1] = {l.sub[0].right() + kGap, in.y, kSubW, kTabH};
    l.bar = {in.x, in.bottom() - kBarH, in.w, kBarH};
    l.apply = {l.bar.right() - kGap - kApplyW, l.bar.y + (kBarH - kButtonH) * 0.5f, kApplyW, kButtonH};
    l.discard = {l.apply.x - kGap - kDiscardW, l.apply.y, kDiscardW, kButtonH};

    const float body_y = in.y + kTabH + kGap;
    const float body_h = std::max(0.0f, l.bar.y - kGap - body_y);
    const float list_w = std::clamp(in.w * 0.43f, 260.0f, 560.0f);
    l.list = {in.x, body_y, list_w, body_h};
    l.details = {in.x + list_w + kGap, body_y, std::max(0.0f, in.w - list_w - kGap), body_h};
    l.tall_enough = body_h >= 120.0f;

    const float lx = l.list.x + kPad, lw = std::max(0.0f, l.list.w - 2 * kPad);
    l.search = {lx, l.list.y + kPad, lw, kSearchH};
    l.filter = {lx, l.search.bottom() + 8.0f, lw, kFilterH};
    l.count = {lx, l.filter.bottom(), lw, kCountH};
    const float rows_y = l.count.bottom() + 4.0f;
    l.rows = {lx, rows_y, lw, std::max(0.0f, l.list.bottom() - kPad - kFolderH - rows_y)};
    l.folder = {lx, l.list.bottom() - kPad - kFolderH, lw, kFolderH};
    l.info = {l.details.x + kTextIndent, l.details.y + kPad + kActionsH + 10.0f, std::max(0.0f, l.details.w - kTextIndent - kPad),
              std::max(0.0f, l.details.bottom() - kPad - (l.details.y + kPad + kActionsH + 10.0f))};

    l.actions = {l.details.x + kTextIndent + name_w + 28.0f, l.details.y + kPad, kActionsW, kActionsH};
    // keep the button inside the column: a very long name pushes it left of its place rather than out of the frame
    l.actions.x = std::min(l.actions.x, l.details.right() - kPad - kActionsW - 150.0f);
    const float menu_h = static_cast<float>(menu_rows) * kMenuRowH + 2 * kMenuPad;
    l.menu = {l.actions.x, l.actions.bottom() + 4.0f, kMenuW, menu_h};

    l.confirm = {(W - kConfirmW) * 0.5f, (H - kConfirmH) * 0.5f, kConfirmW, kConfirmH};
    l.yes = {l.confirm.x + kConfirmW * 0.5f - kGap * 0.5f - kConfirmButtonW - 10.0f, l.confirm.bottom() - kPad - kButtonH, kConfirmButtonW, kButtonH};
    l.no = {l.confirm.x + kConfirmW * 0.5f + kGap * 0.5f + 10.0f, l.yes.y, kConfirmButtonW, kButtonH};
    return l;
}

// The row of the Actions menu under a point, or -1.
inline int menu_row_at(const Layout& l, int rows, float px, float py) {
    if (!l.menu.has(px, py)) return -1;
    const int r = static_cast<int>((py - l.menu.y - kMenuPad) / kMenuRowH);
    return (py - l.menu.y - kMenuPad >= 0.0f && r >= 0 && r < rows) ? r : -1;
}

// How many whole mod rows fit in the list, and the row under a point (-1 for none); `first` is the index of the top row.
inline int rows_visible(const Layout& l) { return std::max(0, static_cast<int>(l.rows.h / kRowH)); }
inline int row_at(const Layout& l, float px, float py, int first, int count) {
    if (!l.rows.has(px, py)) return -1;
    const int r = static_cast<int>((py - l.rows.y) / kRowH);
    if (r >= rows_visible(l) || first + r >= count) return -1;
    return first + r;
}

// The page's state. Every action only changes this; the glue shows it.
struct State {
    bool open = false;
    int tab = 0;            // 0 MY MODS, 1 INSTALL
    int sub = 0;            // INSTALL's sub-tab: 0 MODS, 1 MODPACKS
    bool menu = false;      // the Actions menu
    bool typing = false;    // the search box has the keyboard
    bool confirm = false;   // "discard the changes?"
    int pending = 0;        // collected changes other than options (sections 6 to 8 add them)
    int options = 0;        // option values that differ from the ones in use, counted by the settings forms
    int changes() const { return pending + options; }

    void open_page() { open = true; tab = 0; menu = false; confirm = false; typing = false; }
    // BACK, and Escape once the menu and the frame are closed: ask first when changes are collected.
    void request_close() {
        if (changes() > 0) { confirm = true; menu = false; typing = false; }
        else { open = false; menu = false; typing = false; }
    }
    void back_click() {
        if (confirm) return;
        request_close();
    }
    // Escape closes the topmost thing only: the confirmation, else the menu, else the search box's focus, else the page (D9).
    void escape() {
        if (confirm) confirm = false;
        else if (menu) menu = false;
        else if (typing) typing = false;
        else request_close();
    }
    void tab_click(int t) {
        if (confirm) return;
        tab = t;
        menu = false;
        typing = false;
    }
    void sub_click(int t) {
        if (confirm || tab != 1) return;
        sub = t;
        menu = false;
        typing = false;
    }
    bool packs() const { return tab == 1 && sub == 1; }
    void actions_click() {
        if (confirm) return;
        menu = !menu;
        typing = false;
    }
    // A click on a menu row or outside the menu closes it; the row's action is the caller's.
    void menu_done() { menu = false; }
    void confirm_yes() { confirm = false; pending = 0; open = false; menu = false; typing = false; }
    void confirm_no() { confirm = false; }
    // One text for the change bar's button: options apply at once, everything else needs a restart (D2, D14). 6.4 uses it.
    static const wchar_t* apply_label(bool restart_needed) { return restart_needed ? L"APPLY & RESTART" : L"APPLY"; }
};

}  // namespace page
