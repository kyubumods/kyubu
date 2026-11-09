// The Mods row on the title screen (design D11). The game's third row is the string literal "Exit" in .rdata; the mod
// rewrites it to "Mods", so the game draws, hovers and clicks that row itself (hover 2). This header places the moved Exit row.
#pragma once

namespace row {

constexpr float kWidgetFrameW = 200.0f;   // the start menu widget is 200 wide, at ((W - 200) / 2, H / 2 + 50) in the window
constexpr int kShortWindowH = 480;        // below this the Exit row at widget y 180 would leave the window
constexpr float kExitY = 180.0f;          // the game's rows are at 30, 80 and 130
constexpr float kCornerMargin = 10.0f;    // window px from the left and the bottom edge in the corner layout
constexpr float kCornerBandW = 70.0f;

// Where the moved Exit row is drawn, in the widget's frame, and the band in which the cursor hovers it.
struct Layout {
    bool corner = false;   // short window: Exit goes to the bottom-left corner of the window
    float x = 0, y = 0;    // text anchor; flags 1 centres on x, flags 0 starts at x
    int flags = 1;
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;   // hover band: x0 <= cx < x1, y0 <= cy < y1
};

inline Layout exit_layout(int window_w, int window_h, float widget_w) {
    Layout l;
    if (window_h >= kShortWindowH) {
        l.x = widget_w * 0.5f; l.y = kExitY; l.flags = 1;
        l.x0 = 0.0f; l.x1 = widget_w; l.y0 = kExitY - 20.0f; l.y1 = kExitY + 10.0f;
        return l;
    }
    const float ox = (static_cast<float>(window_w) - widget_w) * 0.5f;
    const float oy = static_cast<float>(window_h) * 0.5f + 50.0f;
    l.corner = true; l.flags = 0;
    l.x = kCornerMargin - ox;
    l.y = static_cast<float>(window_h) - 30.0f - oy;
    l.x0 = l.x - 4.0f; l.x1 = l.x + kCornerBandW; l.y0 = l.y - 20.0f; l.y1 = l.y + 10.0f;
    return l;
}

inline bool inside(const Layout& l, float cx, float cy) { return cx >= l.x0 && cx < l.x1 && cy >= l.y0 && cy < l.y1; }

}  // namespace row
