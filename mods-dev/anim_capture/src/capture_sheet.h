// What `anim capture` writes (openspec add-ability-animation, task 7.3; design D11): the sheet's pixels and layout, its PNG, and
// `pose.json`. Pure: no game, no Direct3D and no loader, so a unit test builds it (tests/unit/test_anim_capture.cpp). The layout
// functions are the one source for the Animator's offline sheet (task 6.9), which must look the same.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "miniz.h"

namespace kyubu::anim_capture {

// ---- images: 8 bits per channel, RGB, rows top to bottom
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;   // w * h * 3
    Image() = default;
    Image(int width, int height, uint8_t r = 0, uint8_t g = 0, uint8_t b = 0) : w(width), h(height), rgb(size_t(width) * height * 3) {
        for (size_t i = 0; i < rgb.size(); i += 3) {
            rgb[i] = r;
            rgb[i + 1] = g;
            rgb[i + 2] = b;
        }
    }
    void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t* p = &rgb[(size_t(y) * w + x) * 3];
        p[0] = r;
        p[1] = g;
        p[2] = b;
    }
    const uint8_t* at(int x, int y) const { return &rgb[(size_t(y) * w + x) * 3]; }
};

// The square of the back buffer that holds the character: the player stands at the middle of the screen, so the box is centred and a
// fixed fraction of the shorter side (the game's camera keeps the character that size). Clamped to the buffer.
struct CropBox {
    int x = 0, y = 0, size = 0;
};
inline CropBox crop_box(int width, int height) {
    CropBox b;
    int side = int(std::lround(std::fmin(width, height) * 0.5));
    if (side > width) side = width;
    if (side > height) side = height;
    b.size = side < 1 ? 1 : side;
    b.x = (width - b.size) / 2;
    b.y = (height - b.size) / 2;
    return b;
}

// Reads the box out of a 32 bit B,G,R,x back buffer (Direct3D's X8R8G8B8 and A8R8G8B8 in memory order), `pitch` bytes a row.
inline Image crop_bgra(const uint8_t* px, int pitch, int width, int height, const CropBox& b) {
    Image out(b.size, b.size);
    for (int y = 0; y < b.size; ++y) {
        const int sy = b.y + y;
        if (sy < 0 || sy >= height) continue;
        for (int x = 0; x < b.size; ++x) {
            const int sx = b.x + x;
            if (sx < 0 || sx >= width) continue;
            const uint8_t* s = px + size_t(sy) * pitch + size_t(sx) * 4;
            out.set(x, y, s[2], s[1], s[0]);
        }
    }
    return out;
}

inline Image resize_nearest(const Image& in, int nw, int nh) {
    Image out(nw, nh);
    if (in.w <= 0 || in.h <= 0) return out;
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            const uint8_t* s = in.at(int((int64_t(x) * in.w) / nw), int((int64_t(y) * in.h) / nh));
            out.set(x, y, s[0], s[1], s[2]);
        }
    return out;
}

// ---- text: a 5 x 7 bitmap font for what the labels use (digits, letters in lower case, a few signs). One row of a glyph is 5 bits, the
// left pixel is bit 4. Anything else draws as '?'.
struct Glyph {
    char c;
    uint8_t rows[7];
};
constexpr Glyph kGlyphs[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'a', {0, 0, 0x0E, 0x01, 0x0F, 0x11, 0x0F}},       {'b', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E}},
    {'c', {0, 0, 0x0E, 0x10, 0x10, 0x11, 0x0E}},       {'d', {0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F}},
    {'e', {0, 0, 0x0E, 0x11, 0x1F, 0x10, 0x0E}},       {'f', {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08}},
    {'g', {0, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}},    {'h', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11}},
    {'i', {0x04, 0, 0x0C, 0x04, 0x04, 0x04, 0x0E}},    {'j', {0x02, 0, 0x06, 0x02, 0x02, 0x12, 0x0C}},
    {'k', {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}}, {'l', {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'m', {0, 0, 0x1A, 0x15, 0x15, 0x15, 0x15}},       {'n', {0, 0, 0x16, 0x19, 0x11, 0x11, 0x11}},
    {'o', {0, 0, 0x0E, 0x11, 0x11, 0x11, 0x0E}},       {'p', {0, 0, 0x1E, 0x11, 0x1E, 0x10, 0x10}},
    {'q', {0, 0, 0x0F, 0x11, 0x0F, 0x01, 0x01}},       {'r', {0, 0, 0x16, 0x19, 0x10, 0x10, 0x10}},
    {'s', {0, 0, 0x0F, 0x10, 0x0E, 0x01, 0x1E}},       {'t', {0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06}},
    {'u', {0, 0, 0x11, 0x11, 0x11, 0x13, 0x0D}},       {'v', {0, 0, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'w', {0, 0, 0x11, 0x11, 0x15, 0x15, 0x0A}},       {'x', {0, 0, 0x11, 0x0A, 0x04, 0x0A, 0x11}},
    {'y', {0, 0, 0x11, 0x11, 0x0F, 0x01, 0x0E}},       {'z', {0, 0, 0x1F, 0x02, 0x04, 0x08, 0x1F}},
    {'.', {0, 0, 0, 0, 0, 0x0C, 0x0C}},                {',', {0, 0, 0, 0, 0x0C, 0x04, 0x08}},
    {'-', {0, 0, 0, 0x1F, 0, 0, 0}},                   {':', {0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0}},
    {'=', {0, 0, 0x1F, 0, 0x1F, 0, 0}},                {'_', {0, 0, 0, 0, 0, 0, 0x1F}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}}, {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0, 0x04}},
};

inline const Glyph& glyph_for(char c) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    for (const Glyph& g : kGlyphs)
        if (g.c == c) return g;
    return kGlyphs[sizeof(kGlyphs) / sizeof(kGlyphs[0]) - 1];
}

constexpr int kGlyphW = 5, kGlyphH = 7, kGlyphAdvance = 6;
inline int text_width(const std::string& s, int scale) { return s.empty() ? 0 : int(s.size()) * kGlyphAdvance * scale - scale; }

inline void draw_text(Image& im, int x, int y, const std::string& s, int scale, uint8_t r, uint8_t g, uint8_t b) {
    for (char c : s) {
        const Glyph& gl = glyph_for(c);
        for (int row = 0; row < kGlyphH; ++row)
            for (int col = 0; col < kGlyphW; ++col)
                if (gl.rows[row] & (0x10 >> col))
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx) im.set(x + col * scale + dx, y + row * scale + dy, r, g, b);
        x += kGlyphAdvance * scale;
    }
}

// ---- the sheet: a header line, then one row per time and one column per view; each cell is its label strip over a square picture
constexpr int kCellPx = 256;        // the picture's side
constexpr int kLabelScale = 2;
constexpr int kLabelPad = 4;
constexpr int kLabelH = kGlyphH * kLabelScale + 2 * kLabelPad;
constexpr int kMargin = 6;
constexpr int kHeaderH = kGlyphH * kLabelScale + 2 * kMargin;
constexpr int kCellW = kCellPx;
constexpr int kCellH = kCellPx + kLabelH;

struct SheetSize {
    int w, h;
};
inline SheetSize sheet_size(int rows, int cols) {
    return {kMargin + cols * (kCellW + kMargin), kHeaderH + rows * (kCellH + kMargin)};
}
// The top-left of the label strip of cell (row, col).
inline void cell_origin(int row, int col, int* x, int* y) {
    *x = kMargin + col * (kCellW + kMargin);
    *y = kHeaderH + row * (kCellH + kMargin);
}

struct SheetCell {
    Image picture;        // any size: it is scaled to kCellPx
    std::string label;
};

// `cells` is row-major, `cols` per row. A header names what the sheet is (a capture of the game, or the mimic's, task 6.9).
inline Image make_sheet(const std::string& header, const std::vector<SheetCell>& cells, int cols) {
    const int rows = cols > 0 ? int((cells.size() + cols - 1) / cols) : 0;
    const SheetSize sz = sheet_size(rows < 1 ? 1 : rows, cols < 1 ? 1 : cols);
    Image sheet(sz.w, sz.h, 24, 24, 28);
    draw_text(sheet, kMargin, kMargin, header, kLabelScale, 230, 230, 120);
    for (size_t i = 0; i < cells.size(); ++i) {
        int x, y;
        cell_origin(int(i) / cols, int(i) % cols, &x, &y);
        for (int py = 0; py < kLabelH; ++py)
            for (int px = 0; px < kCellW; ++px) sheet.set(x + px, y + py, 52, 52, 64);
        draw_text(sheet, x + kLabelPad, y + kLabelPad, cells[i].label, kLabelScale, 255, 255, 255);
        const Image pic = resize_nearest(cells[i].picture, kCellPx, kCellPx);
        for (int py = 0; py < kCellPx; ++py)
            for (int px = 0; px < kCellPx; ++px) {
                const uint8_t* s = pic.at(px, py);
                sheet.set(x + px, y + kLabelH + py, s[0], s[1], s[2]);
            }
    }
    return sheet;
}

// The PNG file's bytes; empty when the encoder fails.
inline std::vector<uint8_t> encode_png(const Image& im) {
    std::vector<uint8_t> out;
    if (im.w <= 0 || im.h <= 0) return out;
    size_t len = 0;
    void* p = tdefl_write_image_to_png_file_in_memory_ex(im.rgb.data(), im.w, im.h, 3, &len, 6, MZ_FALSE);
    if (!p) return out;
    out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + len);
    mz_free(p);
    return out;
}

// ---- the game's numbers: the draw record (mapping tasks 2.3 and 7.2; creatures atlas, "Posing")
constexpr size_t kDrawRecordSize = 0x890;
constexpr size_t kRecModeTime = 0x04;        // u32, milliseconds
constexpr size_t kRecPoseId = 0x08;          // u32, 0..73
constexpr size_t kMatrixBase = 0xEC;         // 17 matrices of 16 floats, after the draw returns
constexpr int kMatrixCount = 17;

// The model slots of the draw record (task 2.3, read at run time by probe `rig-read`, 7.2): each holds a `Model*`, a float's worth of
// bits, that is an entry of the model table (ModelLoader at GameController + 0x2F8: the vector's begin at +4, its end at +8, entries of
// four bytes). The entry's index is the loader slot, the number the private `catalog.csv` of the pose library maps to a file name.
struct NamedOffset {
    const char* name;
    int offset;
};
constexpr NamedOffset kModelSlots[] = {
    {"head", 0xBC},      {"hair", 0xC0},     {"body", 0xC4},     {"shoulder2", 0xC8}, {"hands", 0xCC},     {"wings", 0xD0},
    {"tail", 0xD4},      {"feet", 0xD8},     {"weapon_b", 0xDC}, {"weapon_a", 0xE0},  {"shoulders", 0xE4}, {"neck", 0xE8},
};
constexpr int kModelSlotCount = int(sizeof(kModelSlots) / sizeof(kModelSlots[0]));
// The appearance ids of the creature (int16 each, at Creature + offset; the creatures atlas page): the loader slot the body part asked
// for, which is what the model slot resolves to unless equipment replaced it.
constexpr NamedOffset kAppearance[] = {
    {"head", 0x8C}, {"hair", 0x8E}, {"hands", 0x90}, {"feet", 0x92}, {"body", 0x94}, {"tail", 0x96}, {"shoulder2", 0x98}, {"wings", 0x9A},
};
constexpr int kAppearanceCount = int(sizeof(kAppearance) / sizeof(kAppearance[0]));
constexpr int kModelEmpty = -1;        // the slot holds no model
constexpr int kModelNotInTable = -2;   // the slot holds a pointer that is not an entry of the table

// Where a part's vectors sit in the record: three floats each; `pos` -1 for a part with no position, `pos_n` the floats the game reads
// of it (the root has x and y only). The same table as the pose provider's, in the order of anim::Part.
struct PartVec {
    const char* name;
    int pos, pos_n, rot;
};
constexpr PartVec kPartVecs[] = {
    {"root", 0xAC, 2, 0x60}, {"body", 0x6C, 3, 0x54}, {"head", 0x78, 3, 0x84},
    {"hand_r", 0x18, 3, 0x30}, {"hand_l", 0x0C, 3, 0x24}, {"tail", -1, 0, 0x90},
};
constexpr int kPartVecCount = int(sizeof(kPartVecs) / sizeof(kPartVecs[0]));

struct FrameData {
    int index = 0;
    double t_ms = 0;
    std::string view;
    float body_yaw = 0;
    uint32_t pose_id = 0, mode_time_ms = 0;
    float pos[kPartVecCount][3] = {};
    float rot[kPartVecCount][3] = {};
    bool has_matrices = false;
    float mat[kMatrixCount][16] = {};
    bool has_models = false;                     // `anim rig` only
    int32_t model[kModelSlotCount] = {};         // the table index, kModelEmpty or kModelNotInTable
    int16_t appearance[kAppearanceCount] = {};
};

// The vectors as the join site sees them (so after the clip's tracks were written); `record` is the draw record.
inline void read_vectors(const void* record, FrameData* f) {
    const uint8_t* r = static_cast<const uint8_t*>(record);
    std::memcpy(&f->mode_time_ms, r + kRecModeTime, 4);
    std::memcpy(&f->pose_id, r + kRecPoseId, 4);
    for (int i = 0; i < kPartVecCount; ++i) {
        if (kPartVecs[i].pos >= 0) std::memcpy(f->pos[i], r + kPartVecs[i].pos, kPartVecs[i].pos_n * 4);
        std::memcpy(f->rot[i], r + kPartVecs[i].rot, 12);
    }
}
// The matrices the draw left, after it returned.
inline void read_matrices(const void* record, FrameData* f) {
    std::memcpy(f->mat, static_cast<const uint8_t*>(record) + kMatrixBase, sizeof f->mat);
    f->has_matrices = true;
}

// The model slots of the record resolved against the model table (`table`, `n` entries of Model*). `creature_appearance` is the creature's
// data from kAppearance[0].offset on (at least 16 bytes).
inline void read_model_slots(const void* record, const uint32_t* table, size_t n, FrameData* f) {
    const uint8_t* r = static_cast<const uint8_t*>(record);
    for (int i = 0; i < kModelSlotCount; ++i) {
        uint32_t ptr = 0;
        std::memcpy(&ptr, r + kModelSlots[i].offset, 4);
        int32_t at = kModelEmpty;
        if (ptr != 0) {
            at = kModelNotInTable;
            for (size_t k = 0; k < n; ++k)
                if (table[k] == ptr) {
                    at = int32_t(k);
                    break;
                }
        }
        f->model[i] = at;
    }
    f->has_models = true;
}
inline void read_appearance(const void* creature_appearance, FrameData* f) {
    const uint8_t* c = static_cast<const uint8_t*>(creature_appearance);
    for (int i = 0; i < kAppearanceCount; ++i) std::memcpy(&f->appearance[i], c + (kAppearance[i].offset - kAppearance[0].offset), 2);
}

namespace detail {
inline void num(std::string& o, double v) {
    if (!std::isfinite(v)) {
        o += "null";
        return;
    }
    char b[40];
    std::snprintf(b, sizeof b, "%.6g", v);
    o += b;
}
inline void arr(std::string& o, const float* v, int n) {
    o += '[';
    for (int i = 0; i < n; ++i) {
        if (i) o += ", ";
        num(o, v[i]);
    }
    o += ']';
}
inline void str(std::string& o, const std::string& s) {
    o += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += c;
        }
    }
    o += '"';
}
}  // namespace detail

constexpr const char* kPoseJsonFormat = "kyubu-capture/1";

// pose.json (documented in docs/dev-tools.md). `kind` is "capture", "rig", "bake" or "bake-replay": the frames are the same, and
// `models` and `appearance` come with the rig's.
inline std::string pose_json(const std::string& kind, const std::string& clip, const std::string& build,
                             const std::vector<FrameData>& frames) {
    std::string o = "{\n  \"format\": ";
    detail::str(o, kPoseJsonFormat);
    o += ",\n  \"kind\": ";
    detail::str(o, kind);
    o += ",\n  \"clip\": ";
    detail::str(o, clip);
    o += ",\n  \"build\": ";
    detail::str(o, build);
    o += ",\n  \"frames\": [";
    for (size_t k = 0; k < frames.size(); ++k) {
        const FrameData& f = frames[k];
        o += k ? ",\n    {" : "\n    {";
        o += "\"index\": " + std::to_string(f.index) + ", \"t_ms\": ";
        detail::num(o, f.t_ms);
        o += ", \"view\": ";
        detail::str(o, f.view);
        o += ", \"body_yaw\": ";
        detail::num(o, f.body_yaw);
        o += ", \"pose_id\": " + std::to_string(f.pose_id) + ", \"mode_time_ms\": " + std::to_string(f.mode_time_ms);
        o += ",\n     \"vectors\": {";
        for (int i = 0; i < kPartVecCount; ++i) {
            if (i) o += ", ";
            detail::str(o, kPartVecs[i].name);
            o += ": {";
            if (kPartVecs[i].pos >= 0) {
                o += "\"pos\": ";
                detail::arr(o, f.pos[i], kPartVecs[i].pos_n);
                o += ", ";
            }
            o += "\"rot\": ";
            detail::arr(o, f.rot[i], 3);
            o += '}';
        }
        o += "}";
        if (f.has_models) {
            o += ",\n     \"models\": {";
            bool first = true;
            for (int i = 0; i < kModelSlotCount; ++i) {
                if (f.model[i] == kModelEmpty) continue;
                o += first ? "" : ", ";
                first = false;
                detail::str(o, kModelSlots[i].name);
                o += ": ";
                o += f.model[i] >= 0 ? std::to_string(f.model[i]) : "null";
            }
            o += "},\n     \"appearance\": {";
            for (int i = 0; i < kAppearanceCount; ++i) {
                if (i) o += ", ";
                detail::str(o, kAppearance[i].name);
                o += ": " + std::to_string(f.appearance[i]);
            }
            o += "}";
        }
        if (f.has_matrices) {
            o += ",\n     \"matrices\": [";
            for (int m = 0; m < kMatrixCount; ++m) {
                if (m) o += ", ";
                detail::arr(o, f.mat[m], 16);
            }
            o += ']';
        }
        o += '}';
    }
    o += "\n  ]\n}\n";
    return o;
}

// ---- a bake (task 7.4): the draws of a pose, thinned to keys, replayed as a `set` clip, and compared
// Which frames become keys: the first, each later one at least `min_gap_ms` (less half a millisecond, so 60 draws a second give 30 keys a
// second and not 25) after the last kept, and always the last. `f` is in time order.
inline std::vector<size_t> thin_frames(const std::vector<FrameData>& f, double min_gap_ms) {
    std::vector<size_t> kept;
    for (size_t i = 0; i < f.size(); ++i) {
        if (kept.empty() || f[i].t_ms - f[kept.back()].t_ms >= min_gap_ms - 0.5) kept.push_back(i);
    }
    if (!f.empty() && kept.back() != f.size() - 1) {
        // the last frame closes the clip; a kept frame too close to it gives way
        if (kept.size() > 1 && f.back().t_ms - f[kept.back()].t_ms < min_gap_ms - 0.5) kept.pop_back();
        kept.push_back(f.size() - 1);
    }
    return kept;
}

// The clip text that plays the kept frames back: a `set` track for each part's `pos` (not the tail's) and `rot`, a key per kept frame at
// t_ms / duration. `*duration_ms` is the last kept time. `with_tail` false leaves out the tail's track (a creature with no tail model: the
// loader would log that it skips it). Returns "" when there are fewer than two kept frames or the last time is 0.
inline std::string bake_clip_text(const std::vector<FrameData>& f, const std::vector<size_t>& kept, double* duration_ms, bool with_tail = true) {
    if (kept.size() < 2) return "";
    const double d = f[kept.back()].t_ms;
    if (!(d > 0) || !std::isfinite(d)) return "";
    *duration_ms = d;
    char b[200];
    std::snprintf(b, sizeof b, "# baked by anim_capture (anim bake): every key is a draw of the game's own pose\nduration = %.8g\nend = \"hold\"\n", d);
    std::string o = b;
    auto value = [](float v) { return std::isfinite(v) ? double(v) : 0.0; };
    const char* channel[2] = {"pos", "rot"};
    for (int i = 0; i < kPartVecCount; ++i) {
        if (!with_tail && kPartVecs[i].pos < 0) continue;   // the tail is the only part with no position
        for (int c = 0; c < 2; ++c) {
            if (c == 0 && kPartVecs[i].pos < 0) continue;
            o += "\n[[track]]\npart = \"";
            o += kPartVecs[i].name;
            o += "\"\nchannel = \"";
            o += channel[c];
            o += "\"\nmode = \"set\"\nkeys = [\n";
            for (size_t k : kept) {
                const FrameData& fr = f[k];
                const float* v = c == 0 ? fr.pos[i] : fr.rot[i];
                const int have = c == 0 ? kPartVecs[i].pos_n : 3;   // the root's pos has x and y only
                std::snprintf(b, sizeof b, "  { t = %.8g, v = [%.8g, %.8g, %.8g] },\n", fr.t_ms / d, value(have > 0 ? v[0] : 0),
                              value(have > 1 ? v[1] : 0), value(have > 2 ? v[2] : 0));
                o += b;
            }
            o += "]\n";
        }
    }
    return o;
}

// The vectors of `f` (in time order, at least one frame) at time `t_ms`: linear between the draws, held outside them.
inline void interp_vectors(const std::vector<FrameData>& f, double t_ms, FrameData* out) {
    size_t hi = 0;
    while (hi < f.size() && f[hi].t_ms < t_ms) ++hi;
    if (hi == 0 || hi >= f.size()) {
        *out = hi == 0 ? f.front() : f.back();
        out->t_ms = t_ms;
        return;
    }
    const FrameData& a = f[hi - 1];
    const FrameData& b = f[hi];
    *out = a;
    out->t_ms = t_ms;
    const double span = b.t_ms - a.t_ms;
    const float u = span > 0 ? float((t_ms - a.t_ms) / span) : 1.0f;
    for (int i = 0; i < kPartVecCount; ++i)
        for (int k = 0; k < 3; ++k) {
            out->pos[i][k] = a.pos[i][k] + (b.pos[i][k] - a.pos[i][k]) * u;
            out->rot[i][k] = a.rot[i][k] + (b.rot[i][k] - a.rot[i][k]) * u;
        }
}

// The largest differences between vectors, kept over a run: `pos` in voxels (the root's in world units), `rot` in degrees.
struct VectorDiff {
    double max_pos = 0, max_rot = 0;
    int pos_part = -1, rot_part = -1;   // indexes of kPartVecs
    int frames = 0;
};
inline void accumulate_diff(const FrameData& a, const FrameData& b, VectorDiff* d) {
    ++d->frames;
    for (int i = 0; i < kPartVecCount; ++i) {
        for (int k = 0; k < kPartVecs[i].pos_n; ++k) {
            const double e = std::fabs(double(a.pos[i][k]) - double(b.pos[i][k]));
            if (e > d->max_pos || !std::isfinite(e)) {
                d->max_pos = e;
                d->pos_part = i;
            }
        }
        for (int k = 0; k < 3; ++k) {
            const double e = std::fabs(double(a.rot[i][k]) - double(b.rot[i][k]));
            if (e > d->max_rot || !std::isfinite(e)) {
                d->max_rot = e;
                d->rot_part = i;
            }
        }
    }
}

// The label of one cell: the time and the view (22 characters at most fit a cell; the yaw is in pose.json).
inline std::string frame_label(double t_ms, const std::string& view) {
    char b[96];
    std::snprintf(b, sizeof b, "%gms %s", t_ms, view.c_str());
    return b;
}

}  // namespace kyubu::anim_capture
