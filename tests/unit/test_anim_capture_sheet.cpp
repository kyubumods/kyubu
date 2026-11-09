// Unit tests of `anim capture`'s words, sheet layout, labels, PNG and pose.json (openspec add-ability-animation, task 7.3;
// mods-dev/anim_capture). No game and no Direct3D.
#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "capture_args.h"
#include "capture_sheet.h"
#include "json.hpp"

using namespace kyubu::anim_capture;

TEST_CASE("anim capture: a clip alone takes the default times and both views") {
    const CaptureArgs a = parse_capture_args("demo:slam");
    REQUIRE(a.ok);
    CHECK(a.clip == "demo:slam");
    CHECK(a.times_ms == std::vector<double>{0, 500, 1000});
    CHECK(a.views == std::vector<int>{0, 1});
}

TEST_CASE("anim capture: times and views") {
    const CaptureArgs a = parse_capture_args("  demo:slam   0,250.5,1e3  left ");
    REQUIRE(a.ok);
    CHECK(a.times_ms == std::vector<double>{0, 250.5, 1000});
    CHECK(a.views == std::vector<int>{1});
    const CaptureArgs b = parse_capture_args("demo:slam 100 left,front");
    REQUIRE(b.ok);
    CHECK(b.views == std::vector<int>{1, 0});
    CHECK(parse_capture_args("demo:slam 5,5").ok);   // a time may repeat
}

TEST_CASE("anim capture: wrong input names what is wrong") {
    for (const char* bad : {"", "   ", "demo:slam 1 front extra"}) {
        const CaptureArgs a = parse_capture_args(bad);
        REQUIRE_MESSAGE(!a.ok, bad);
        CHECK_MESSAGE(a.error == kCaptureUsage, bad);
    }
    for (const char* bad : {"demo:slam abc", "demo:slam -1", "demo:slam 1,,2", "demo:slam 1,", "demo:slam nan", "demo:slam 1,inf"}) {
        const CaptureArgs a = parse_capture_args(bad);
        REQUIRE_MESSAGE(!a.ok, bad);
        CHECK_MESSAGE(!a.error.empty(), bad);
    }
    CHECK(parse_capture_args("demo:slam 0 top").error.find("no view 'top'") != std::string::npos);
    CHECK(parse_capture_args("demo:slam 0 front,front").error.find("twice") != std::string::npos);
    std::string many = "demo:slam 0";
    for (size_t i = 0; i < kMaxTimes; ++i) many += ",1";
    CHECK(parse_capture_args(many).error.find("at most") != std::string::npos);
}

TEST_CASE("anim capture: the crop box is a centred square inside the buffer") {
    CropBox b = crop_box(1280, 720);
    CHECK(b.size == 360);
    CHECK(b.x == 460);
    CHECK(b.y == 180);
    b = crop_box(100, 40);
    CHECK(b.size == 20);
    CHECK(b.x + b.size <= 100);
    CHECK(b.y + b.size <= 40);
    CHECK(crop_box(1, 1).size == 1);
}

TEST_CASE("anim capture: crop turns B,G,R,x into R,G,B") {
    const int w = 8, h = 6;
    std::vector<uint8_t> px(size_t(w) * h * 4, 0);
    // the pixel at (3, 2): blue 10, green 20, red 30
    uint8_t* p = &px[(size_t(2) * w + 3) * 4];
    p[0] = 10;
    p[1] = 20;
    p[2] = 30;
    CropBox box;
    box.x = 2;
    box.y = 1;
    box.size = 4;
    const Image im = crop_bgra(px.data(), w * 4, w, h, box);
    REQUIRE(im.w == 4);
    REQUIRE(im.h == 4);
    const uint8_t* q = im.at(1, 1);
    CHECK(int(q[0]) == 30);
    CHECK(int(q[1]) == 20);
    CHECK(int(q[2]) == 10);
    // a pitch larger than the row is honoured
    std::vector<uint8_t> padded(size_t(w + 2) * 4 * h, 0);
    std::memcpy(&padded[(size_t(2) * (w + 2) + 3) * 4], p, 4);
    const Image im2 = crop_bgra(padded.data(), (w + 2) * 4, w, h, box);
    CHECK(int(im2.at(1, 1)[0]) == 30);
}

TEST_CASE("anim capture: text draws lit pixels and different words differ") {
    Image a(80, 12), b(80, 12);
    draw_text(a, 1, 1, "250ms front", 1, 255, 255, 255);
    draw_text(b, 1, 1, "250ms left", 1, 255, 255, 255);
    size_t lit_a = 0;
    for (size_t i = 0; i < a.rgb.size(); i += 3) lit_a += a.rgb[i] == 255;
    CHECK(lit_a > 30);
    CHECK(a.rgb != b.rgb);
    CHECK(text_width("ab", 2) == 2 * 6 * 2 - 2);
    // an unknown character draws as '?', upper case as lower case
    Image q(8, 8), u(8, 8), l(8, 8);
    draw_text(q, 0, 0, "?", 1, 255, 255, 255);
    draw_text(u, 0, 0, "#", 1, 255, 255, 255);
    CHECK(q.rgb == u.rgb);
    draw_text(l, 0, 0, "f", 1, 255, 255, 255);
    Image up(8, 8);
    draw_text(up, 0, 0, "F", 1, 255, 255, 255);
    CHECK(l.rgb == up.rgb);
}

TEST_CASE("anim capture: the sheet has the size of its layout and places each cell") {
    std::vector<SheetCell> cells;
    for (int i = 0; i < 6; ++i) cells.push_back({Image(10, 10, uint8_t(10 * (i + 1)), 0, 0), "0ms front"});
    const Image sheet = make_sheet("capture demo:slam", cells, 2);
    const SheetSize sz = sheet_size(3, 2);
    CHECK(sheet.w == sz.w);
    CHECK(sheet.h == sz.h);
    for (int i = 0; i < 6; ++i) {
        int x, y;
        cell_origin(i / 2, i % 2, &x, &y);
        // the middle of the picture, below the label strip, has that cell's red
        const uint8_t* mid = sheet.at(x + kCellPx / 2, y + kLabelH + kCellPx / 2);
        CHECK_MESSAGE(int(mid[0]) == 10 * (i + 1), "cell " << i);
        // and the label strip holds some white text
        bool white = false;
        for (int px = 0; px < kCellW && !white; ++px)
            for (int py = 0; py < kLabelH && !white; ++py) white = sheet.at(x + px, y + py)[0] == 255 && sheet.at(x + px, y + py)[2] == 255;
        CHECK_MESSAGE(white, "label of cell " << i);
    }
    // the header has text of its own colour
    bool header = false;
    for (int px = 0; px < sheet.w && !header; ++px)
        for (int py = 0; py < kHeaderH && !header; ++py) header = sheet.at(px, py)[0] == 230;
    CHECK(header);
}

TEST_CASE("anim capture: the PNG decodes to the sheet's pixels") {
    Image im(5, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x) im.set(x, y, uint8_t(x * 40), uint8_t(y * 80), uint8_t(x + y));
    const std::vector<uint8_t> png = encode_png(im);
    REQUIRE(png.size() > 40);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(std::memcmp(png.data(), sig, 8) == 0);
    auto be32 = [&](size_t at) { return (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) | (uint32_t(png[at + 2]) << 8) | png[at + 3]; };
    // the chunks: IHDR first, then IDAT(s), then IEND
    REQUIRE(std::memcmp(&png[12], "IHDR", 4) == 0);
    CHECK(be32(16) == 5);
    CHECK(be32(20) == 3);
    CHECK(int(png[24]) == 8);    // bit depth
    CHECK(int(png[25]) == 2);    // colour type: RGB
    std::vector<uint8_t> z;
    size_t at = 8;
    bool end = false;
    while (at + 12 <= png.size()) {
        const uint32_t len = be32(at);
        if (std::memcmp(&png[at + 4], "IDAT", 4) == 0) z.insert(z.end(), png.begin() + long(at + 8), png.begin() + long(at + 8 + len));
        if (std::memcmp(&png[at + 4], "IEND", 4) == 0) end = true;
        at += 12 + len;
    }
    CHECK(end);
    size_t raw_len = 0;
    void* raw = tinfl_decompress_mem_to_heap(z.data(), z.size(), &raw_len, TINFL_FLAG_PARSE_ZLIB_HEADER);
    REQUIRE(raw != nullptr);
    REQUIRE(raw_len == size_t(3) * (1 + 5 * 3));
    const uint8_t* r = static_cast<const uint8_t*>(raw);
    for (int y = 0; y < 3; ++y) {
        CHECK(int(r[y * 16]) == 0);   // filter type: none
        for (int x = 0; x < 5; ++x) {
            const uint8_t* px = r + y * 16 + 1 + x * 3;
            CHECK(int(px[0]) == x * 40);
            CHECK(int(px[1]) == y * 80);
            CHECK(int(px[2]) == x + y);
        }
    }
    mz_free(raw);
    CHECK(encode_png(Image()).empty());
}

TEST_CASE("anim capture: vectors and matrices are read from the draw record's offsets") {
    std::vector<uint8_t> rec(kDrawRecordSize, 0);
    auto put = [&](size_t at, float a, float b, float c) {
        const float v[3] = {a, b, c};
        std::memcpy(&rec[at], v, 12);
    };
    const uint32_t time_ms = 345, pose = 12;
    std::memcpy(&rec[kRecModeTime], &time_ms, 4);
    std::memcpy(&rec[kRecPoseId], &pose, 4);
    put(0xAC, 7, 8, 99);       // root position: x and y only are read
    put(0x60, 0, 0, 30);       // root euler
    put(0x78, 1, 2, 3);        // head position
    put(0x84, 10, 20, 30);     // head euler
    put(0x24, 5, 6, 7);        // hand A euler = hand_l
    put(0x90, 1, 1, 1);        // tail euler
    for (int m = 0; m < kMatrixCount; ++m) {
        float mat[16];
        for (int i = 0; i < 16; ++i) mat[i] = float(m * 100 + i);
        std::memcpy(&rec[kMatrixBase + size_t(m) * 0x40], mat, sizeof mat);
    }
    FrameData f;
    read_vectors(rec.data(), &f);
    CHECK(f.pose_id == 12);
    CHECK(f.mode_time_ms == 345);
    CHECK(f.pos[0][0] == 7);
    CHECK(f.pos[0][1] == 8);
    CHECK(f.pos[0][2] == 0);   // the third float of the root's position is not read
    CHECK(f.rot[0][2] == 30);
    CHECK(f.pos[2][1] == 2);
    CHECK(f.rot[2][0] == 10);
    CHECK(f.rot[4][2] == 7);
    CHECK(f.rot[5][1] == 1);
    CHECK(!f.has_matrices);
    read_matrices(rec.data(), &f);
    CHECK(f.has_matrices);
    CHECK(f.mat[0][0] == 0);
    CHECK(f.mat[16][15] == 1615);
    CHECK(f.mat[9][3] == 903);
    // the largest matrix ends inside the record
    CHECK(kMatrixBase + size_t(kMatrixCount) * 0x40 <= kDrawRecordSize);
}

TEST_CASE("anim capture: pose.json has one frame per time with vectors and matrices") {
    std::vector<FrameData> frames;
    for (int i = 0; i < 3; ++i) {
        FrameData f;
        f.index = i;
        f.t_ms = 250.0 * i;
        f.view = i % 2 ? "left" : "front";
        f.body_yaw = i % 2 ? 90.0f : 180.0f;
        f.pose_id = 5;
        f.mode_time_ms = 100 + i;
        f.pos[2][1] = 1.5f + float(i);
        f.rot[4][0] = 45.0f;
        f.has_matrices = i != 1;
        f.mat[3][12] = 2.5f;
        frames.push_back(f);
    }
    frames[2].rot[1][1] = std::nanf("");
    const std::string text = pose_json("capture", "demo:slam", "cube-0x51D2CE9C", frames);
    const auto j = nlohmann::json::parse(text, nullptr, false);
    REQUIRE(!j.is_discarded());
    CHECK(j["format"] == "kyubu-capture/1");
    CHECK(j["kind"] == "capture");
    CHECK(j["clip"] == "demo:slam");
    CHECK(j["build"] == "cube-0x51D2CE9C");
    REQUIRE(j["frames"].size() == 3);
    const auto& f2 = j["frames"][2];
    CHECK(f2["index"] == 2);
    CHECK(f2["t_ms"] == 500);
    CHECK(f2["view"] == "front");
    CHECK(f2["body_yaw"] == 180);
    CHECK(f2["vectors"]["head"]["pos"][1] == 3.5);
    CHECK(f2["vectors"]["hand_l"]["rot"][0] == 45);
    CHECK(f2["vectors"]["root"]["pos"].size() == 2);
    CHECK(f2["vectors"]["tail"].contains("rot"));
    CHECK(!f2["vectors"]["tail"].contains("pos"));
    CHECK(f2["vectors"]["body"]["rot"][1].is_null());   // a value that is not finite is null, so the file stays JSON
    REQUIRE(f2["matrices"].size() == kMatrixCount);
    CHECK(f2["matrices"][3].size() == 16);
    CHECK(f2["matrices"][3][12] == 2.5);
    CHECK(!j["frames"][1].contains("matrices"));
}

TEST_CASE("anim capture: the label names the time and the view") {
    CHECK(frame_label(500, "front") == "500ms front");
    CHECK(frame_label(12.5, "left") == "12.5ms left");
    // a label of the longest default time and view fits a cell at the label's scale
    CHECK(text_width(frame_label(1000, "front"), kLabelScale) <= kCellW - 2 * kLabelPad);
}
