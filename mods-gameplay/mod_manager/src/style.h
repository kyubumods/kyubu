// The look of the Mods page (design D10): colours and sizes measured on native captures of the game's Options and
// New Character screens. Colours are 0xRRGGBB; alpha is 0..1.
#pragma once

namespace style {

// Frames (`blackwidget`) and plates (`button`) at rest: black at 2/3 opacity. Measured on two backgrounds,
// sky (43,157,255) -> (14,52,84) and water (11,80,234) -> (4,26,77): each channel is 0.333 of the background.
constexpr float kFillAlpha = 2.0f / 3.0f;
constexpr unsigned kFillRgb = 0x000000;

// Edge: a white line 2 px thick with a black line 3 px thick outside it; corners are stepped (one pixel cut).
constexpr unsigned kEdgeRgb = 0xFFFFFF;
constexpr unsigned kEdgeOutlineRgb = 0x000000;
constexpr int kEdgePx = 2;
constexpr int kEdgeOutlinePx = 3;

// A hovered plate (APPLY / CANCEL on Options): opaque vertical grey gradient, 139 at the top to 72 at the bottom.
constexpr unsigned kHoverTopRgb = 0x8B8B8B;
constexpr unsigned kHoverBottomRgb = 0x484848;

// Text: white with a black outline; the hovered row and the selected list row turn cyan (0.2, 1, 1).
constexpr unsigned kTextRgb = 0xFFFFFF;
constexpr unsigned kTextOutlineRgb = 0x000000;
constexpr unsigned kTextHoverRgb = 0x33FFFF;

// Not in the game: kept from D10's placeholders.
constexpr unsigned kTextSecondaryRgb = 0x9DB1C9;
constexpr unsigned kVerifiedRgb = 0x6CCB5F;
constexpr unsigned kWarningRgb = 0xE8B341;
constexpr unsigned kErrorRgb = 0xE0564B;

// Sizes in pixels. The Options rows are 30 px apart on a 1030 x 800 window (Wine) and on a 2495 x 1417 window
// (native): the game does not scale its screens with the window height, so these are fixed.
constexpr int kRowPitch = 30;
constexpr int kPadding = 12;
constexpr int kRowGap = 8;

}  // namespace style
