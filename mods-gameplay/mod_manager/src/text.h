// Text for the Mods page (design D10): the game's font through plasma::ScalableFont, the width of a string, and a
// string cut to fit a width. The font is the player's own copy, read from the running game; nothing of it is stored here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "style.h"

namespace text {

// The game's std::wstring (MSVC 2012): 8 wchar_t of buffer or, past 7 characters, a pointer in the first four bytes; then size
// and capacity. `GameString` points at the caller's std::wstring, which must outlive the use.
struct GameString {
    union {
        wchar_t buf[8];
        const wchar_t* ptr;
    };
    uint32_t size;
    uint32_t capacity;
    GameString(const wchar_t* s, size_t n) {
        std::memset(buf, 0, sizeof buf);
        size = static_cast<uint32_t>(n);
        if (n < 8) {
            std::memcpy(buf, s, n * sizeof(wchar_t));
            capacity = 7;
        } else {
            ptr = s;
            capacity = static_cast<uint32_t>(n);
        }
    }
};

// plasma::ScalableFont, thiscall. measureText has six stack arguments and `ret 0x18`.
using DrawTextFn = void(__attribute__((thiscall)) *)(void* font, const void* text, float x, float y, float size, float outline, float glyph_space,
                                                      float line_space, float wrap, const float* fill, const float* outline_colour,
                                                      const float* third_colour, int flags, int snap);
using MeasureTextFn = void(__attribute__((thiscall)) *)(void* font, const void* text, float* out_min, float* out_max, uint32_t align_flags,
                                                         int32_t start, int32_t whole_line);

constexpr uint32_t kFontBaseSize = 0xE0;   // float: the size the glyph advances are in; drawText scales by size / this
constexpr float kTitleSize = 24.0f;        // design D10
constexpr float kBodySize = 18.0f;

struct Font {
    void* font = nullptr;
    DrawTextFn draw = nullptr;
    MeasureTextFn measure = nullptr;

    // Width in pixels of one line at `size`.
    float width(const wchar_t* s, size_t n, float size) const {
        if (!font || !measure || n == 0) return 0.0f;
        GameString g(s, n);
        float lo[2] = {0, 0}, hi[2] = {0, 0};
        measure(font, &g, lo, hi, 0, -1, 1);
        const float base = *reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(font) + kFontBaseSize);
        return base > 0.0f ? hi[0] * size / base : 0.0f;
    }
    float width(const std::wstring& s, float size) const { return width(s.data(), s.size(), size); }

    // The game draws text twice: a black outline, then the fill. `x` is the left end at flags 0, the middle at flags 1.
    void draw_line(const wchar_t* s, size_t n, float x, float y, float size, unsigned rgb, int flags = 0) const {
        if (!font || !draw || n == 0) return;
        const float outline[4] = {0, 0, 0, 1}, zero[4] = {0, 0, 0, 0};
        const float fill[4] = {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, 1.0f};
        GameString g(s, n);
        draw(font, &g, x, y, size, 4.0f, 0.0f, 0.0f, 0.0f, outline, outline, zero, flags, 1);   // the outline
        draw(font, &g, x, y, size, 0.0f, 0.0f, 0.0f, 0.0f, fill, zero, zero, flags, 1);         // the fill
    }
};

// The longest prefix of `s` that fits in `max_width` with "..." after it, or `s` whole when it fits. `width_of(wstring)` gives
// a width; it is monotone enough that a binary search on the length finds the cut.
template <typename WidthFn>
std::wstring fit(const std::wstring& s, float max_width, WidthFn width_of) {
    if (width_of(s) <= max_width) return s;
    const std::wstring dots = L"...";
    if (width_of(dots) > max_width) return L"";
    size_t lo = 0, hi = s.size();   // the prefix of length lo fits; hi does not
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (width_of(s.substr(0, mid) + dots) <= max_width) lo = mid;
        else hi = mid;
    }
    return s.substr(0, lo) + dots;
}

// `s` broken into lines no wider than `max_width` at word gaps (a word wider than the line is cut with fit()). Newlines in `s` end a line.
template <typename WidthFn>
std::vector<std::wstring> wrap(const std::wstring& s, float max_width, WidthFn width_of) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = s.size();
        const std::wstring para = s.substr(pos, nl - pos);
        std::wstring line;
        size_t at = 0;
        while (at < para.size()) {
            size_t sp = para.find(L' ', at);
            if (sp == std::wstring::npos) sp = para.size();
            const std::wstring word = para.substr(at, sp - at);
            at = sp + 1;
            if (word.empty()) continue;
            const std::wstring tryline = line.empty() ? word : line + L" " + word;
            if (line.empty() || width_of(tryline) <= max_width) {
                line = tryline;
            } else {
                out.push_back(line);
                line = word;
            }
            if (width_of(line) > max_width) {   // one word wider than the line
                out.push_back(fit(line, max_width, width_of));
                line.clear();
            }
        }
        if (!line.empty() || para.empty()) out.push_back(line);
        if (nl == s.size()) break;
        pos = nl + 1;
    }
    return out;
}

// wrap(), but in at most `max_lines` lines: what does not fit is cut from the last line, which ends in "...".
template <typename WidthFn>
std::vector<std::wstring> wrap_capped(const std::wstring& s, float max_width, size_t max_lines, WidthFn width_of) {
    std::vector<std::wstring> out = wrap(s, max_width, width_of);
    if (max_lines == 0 || out.size() <= max_lines) return out;
    std::wstring rest = out[max_lines - 1];
    for (size_t i = max_lines; i < out.size(); ++i) rest += L" " + out[i];
    out.resize(max_lines);
    out.back() = fit(rest, max_width, width_of);   // rest is wider than the line, since the next word did not fit
    return out;
}

}  // namespace text
