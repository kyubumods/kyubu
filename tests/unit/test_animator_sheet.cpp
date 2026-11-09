// Unit tests of the Animator's offline sheet (openspec add-ability-animation task 6.9): the rasteriser, the header, the layout and the
// labels of the sheet (one source with `anim capture`), and the `--sheet` command. No game, no window.
#include "doctest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "animation_eval.h"
#include "animation_load.h"
#include "capture_args.h"
#include "capture_sheet.h"
#include "sheet.h"

using namespace kanimator;
namespace fs = std::filesystem;
namespace cap = kyubu::anim_capture;

namespace {

const std::string kStandIn = KYUBU_SOURCE_DIR "/tools/animator/rigs/standin.rig.toml";
const std::string kExample = KYUBU_SOURCE_DIR "/tests/fixtures/kanim/design_example.anim.toml";

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.good());
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

anim::Clip parse_ok(const std::string& text) {
    anim::ClipLoad c = anim::parse_clip(text, "t");
    for (const anim::ClipError& e : c.errors) MESSAGE(anim::format_error(e));
    REQUIRE(c.ok);
    return c.clip;
}

struct Loaded {
    Rig rig;
    std::vector<Mesh> meshes;
};

Loaded stand_in() {
    Loaded l;
    RigLoad r = parse_rig(slurp(kStandIn), "standin.rig.toml");
    REQUIRE(r.ok);
    l.rig = r.rig;
    for (size_t i = 0; i < l.rig.nodes.size(); ++i) {
        std::string note;
        l.meshes.push_back(node_mesh(l.rig, i, "", note));
        CHECK(note.empty());   // the stand-in rig has boxes only
    }
    return l;
}

Affine at(float x, float y, float z) {
    Affine a;
    a.t[0] = x;
    a.t[1] = y;
    a.t[2] = z;
    return a;
}

Camera front_camera() {
    Camera c;
    c.frame({0, 0, 0}, 30);
    c.set_preset(ViewPreset::Front);
    return c;
}

const uint8_t kBg[3] = {58, 64, 78};

bool is_bg(const cap::Image& im, int x, int y) {
    const uint8_t* p = im.at(x, y);
    return p[0] == kBg[0] && p[1] == kBg[1] && p[2] == kBg[2];
}

// The picture's pixels of cell (row, col) of a sheet, as a string for comparison.
std::string cell_pixels(const cap::Image& sheet, int row, int col) {
    int x, y;
    cap::cell_origin(row, col, &x, &y);
    std::string out;
    for (int py = 0; py < cap::kCellPx; ++py)
        for (int px = 0; px < cap::kCellPx; ++px) {
            const uint8_t* p = sheet.at(x + px, y + cap::kLabelH + py);
            out.append(reinterpret_cast<const char*>(p), 3);
        }
    return out;
}

SheetRequest request_for(const Loaded& l, const anim::Clip& clip, std::vector<float> fractions) {
    SheetRequest q;
    q.rig = &l.rig;
    q.meshes = &l.meshes;
    q.clip = &clip;
    q.clip_name = "design_example";
    q.fractions = std::move(fractions);
    return q;
}

std::string read_stream(std::FILE* f) {
    std::rewind(f);
    std::string s;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    return s;
}

}  // namespace

TEST_CASE("sheet: a box is drawn where the camera looks, and nothing else is") {
    std::vector<Mesh> meshes = {mesh_box(10, 10, 10, 0xD02020)};
    std::vector<Affine> world = {Affine()};
    const cap::Image im = render_figure(meshes, world, front_camera(), 64, false);
    REQUIRE(im.w == 64);
    REQUIRE(im.h == 64);
    const uint8_t* mid = im.at(32, 32);
    CHECK(mid[0] > mid[1]);
    CHECK(mid[0] > mid[2]);
    CHECK_FALSE(is_bg(im, 32, 32));
    CHECK(is_bg(im, 0, 0));
    CHECK(is_bg(im, 63, 63));

    // No mesh: the background alone.
    const cap::Image empty = render_figure({}, {}, front_camera(), 16, false);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) CHECK(is_bg(empty, x, y));
}

TEST_CASE("sheet: the nearer box hides the farther one, whatever the order they are drawn in") {
    // The front camera is at +y: the box at y = +30 is the near one.
    const Mesh near_box = mesh_box(10, 10, 10, 0xD02020), far_box = mesh_box(10, 10, 10, 0x20D020);
    for (int order = 0; order < 2; ++order) {
        std::vector<Mesh> meshes = order ? std::vector<Mesh>{far_box, near_box} : std::vector<Mesh>{near_box, far_box};
        std::vector<Affine> world = order ? std::vector<Affine>{at(0, -30, 0), at(0, 30, 0)} : std::vector<Affine>{at(0, 30, 0), at(0, -30, 0)};
        const cap::Image im = render_figure(meshes, world, front_camera(), 64, false);
        const uint8_t* mid = im.at(32, 32);
        CHECK(mid[0] > mid[1]);   // red, not green
    }
}

TEST_CASE("sheet: a triangle behind the camera is skipped, and the grid is behind the figure") {
    Camera cam = front_camera();
    std::vector<Mesh> meshes = {mesh_box(10, 10, 10, 0xD02020)};
    const cap::Image behind = render_figure(meshes, {at(0, 10000, 0)}, cam, 32, false);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) CHECK(is_bg(behind, x, y));

    // The grid is lines on the floor: with no figure some pixels are not the background, and with the box over them the middle is the box.
    const cap::Image grid = render_figure({}, {}, cam, 64, true);
    int lit = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) lit += is_bg(grid, x, y) ? 0 : 1;
    CHECK(lit > 0);
    const cap::Image both = render_figure(meshes, {Affine()}, cam, 64, true);
    CHECK(both.at(32, 32)[0] > both.at(32, 32)[1]);
}

TEST_CASE("sheet: the header names the rig, and is cut by the width of the sheet") {
    CHECK(sheet_header("stand-in rig", "slam", -1, 43) == "mimic - rig stand-in rig - slam");
    CHECK(sheet_header("stand-in rig", "slam", 12, 50) == "mimic - rig stand-in rig - no pose 12 - slam");
    CHECK(sheet_header("stand-in rig", "slam", 12, 43) == "mimic - rig stand-in rig - no pose 12");
    // Too long: the clip goes first, then the pose note, then the rig's own name is cut.
    CHECK(sheet_header("stand-in rig", "a_long_clip_name_that_does_not_fit", 12, 43) == "mimic - rig stand-in rig - no pose 12");
    CHECK(sheet_header("a_rig_with_a_very_long_name_indeed", "slam", 12, 50) == "mimic - rig a_rig_with_a_very_long_name_indeed");
    const std::string cut = sheet_header("a_rig_with_a_very_long_name_indeed_and_more_and_more", "slam", -1, 40);
    CHECK(cut.size() == 40);
    CHECK(cut.rfind("mimic - rig a_rig_with", 0) == 0);
    CHECK(cut.substr(cut.size() - 3) == "...");
    // Never a header that says "capture": the mimic is not one.
    CHECK(sheet_header("r", "c", -1, 43).rfind("mimic", 0) == 0);
}

TEST_CASE("sheet: the design's example clip on the stand-in rig: a row per time, the two views, labelled") {
    const Loaded l = stand_in();
    const anim::Clip clip = parse_ok(slurp(kExample));
    SheetResult r = make_mimic_sheet(request_for(l, clip, {0.0f, 0.5f, 1.0f}));
    REQUIRE(r.ok);
    CHECK(r.error.empty());
    CHECK(r.rows == 3);
    CHECK(r.cols == 2);
    const cap::SheetSize size = cap::sheet_size(3, 2);
    CHECK(r.image.w == size.w);
    CHECK(r.image.h == size.h);
    // The layout is the capture's: `duration = "ability"` is 1000 ms, as kanim assumes.
    REQUIRE(r.labels.size() == 6);
    CHECK(r.labels[0] == "0ms front");
    CHECK(r.labels[1] == "0ms left");
    CHECK(r.labels[2] == "500ms front");
    CHECK(r.labels[3] == "500ms left");
    CHECK(r.labels[4] == "1000ms front");
    CHECK(r.labels[5] == "1000ms left");
    CHECK(r.labels[0] == cap::frame_label(0, cap::kViews[0].name));
    CHECK(r.header == "mimic - rig stand-in rig - design_example");
    CHECK(r.header.size() * 12 - 2 <= size_t(size.w - 2 * cap::kMargin));   // it fits the sheet's width
    CHECK(r.notes.empty());

    // The header strip, the label strip and the pictures are where the capture's sheet has them.
    const uint8_t* bg = r.image.at(0, 0);
    CHECK(bg[0] == 24);
    int x, y;
    cap::cell_origin(1, 1, &x, &y);
    const uint8_t* strip = r.image.at(x + cap::kCellW - 1, y + 1);
    CHECK(strip[0] == 52);
    CHECK(strip[2] == 64);

    // Every picture shows the figure, the same figure from the other side differs, and the clip moves between 0 and 0.5.
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col) {
            const std::string px = cell_pixels(r.image, row, col);
            int figure = 0;
            for (size_t i = 0; i < px.size(); i += 3) figure += (uint8_t(px[i]) != kBg[0] || uint8_t(px[i + 1]) != kBg[1] || uint8_t(px[i + 2]) != kBg[2]);
            CHECK(figure > 200);
        }
    CHECK(cell_pixels(r.image, 0, 0) != cell_pixels(r.image, 0, 1));
    CHECK(cell_pixels(r.image, 0, 0) != cell_pixels(r.image, 1, 0));

    // The PNG is the image's, and drawing it again gives the same bytes.
    REQUIRE(r.png.size() > 8);
    CHECK(r.png[0] == 0x89);
    CHECK(r.png[1] == 'P');
    CHECK(r.png[2] == 'N');
    CHECK(r.png[3] == 'G');
    SheetResult again = make_mimic_sheet(request_for(l, clip, {0.0f, 0.5f, 1.0f}));
    CHECK(again.png == r.png);
}

TEST_CASE("sheet: one time is one row, a time past 1 is the end, and a bad request says why") {
    const Loaded l = stand_in();
    const anim::Clip clip = parse_ok(slurp(kExample));
    SheetResult one = make_mimic_sheet(request_for(l, clip, {2.0f}));
    REQUIRE(one.ok);
    CHECK(one.rows == 1);
    CHECK(one.labels[0] == "1000ms front");

    SheetRequest q = request_for(l, clip, {});
    CHECK_FALSE(make_mimic_sheet(q).ok);
    q.fractions.assign(kSheetMaxTimes + 1, 0.5f);
    CHECK_FALSE(make_mimic_sheet(q).ok);
    q.fractions = {0.5f};
    q.ability_ms = 0;   // `duration = "ability"` of nothing: no length
    SheetResult none = make_mimic_sheet(q);
    CHECK_FALSE(none.ok);
    CHECK(none.error.find("no length") != std::string::npos);
    q.ability_ms = 1000;
    q.rig = nullptr;
    CHECK_FALSE(make_mimic_sheet(q).ok);
}

TEST_CASE("sheet: a clip that borrows a pose the library lacks is drawn on the neutral pose, and says so") {
    const Loaded l = stand_in();
    const anim::Clip clip = parse_ok("duration = 1000\n[borrow]\nmode = 57\n\n[[track]]\npart = \"head\"\nchannel = \"rot\"\nmode = \"add\"\n"
                                     "keys = [ { t = 0.0, v = [0, 0, 0] }, { t = 1.0, v = [0, 30, 0] } ]\n");
    SheetRequest q = request_for(l, clip, {0.0f, 1.0f});
    q.clip_name = "borrow";
    SheetResult r = make_mimic_sheet(q);
    REQUIRE(r.ok);
    REQUIRE(r.notes.size() == 1);
    CHECK(r.notes[0].find("pose 57") != std::string::npos);
    // 46 characters do not fit the 43 of a two-view sheet: the clip name goes, the note stays.
    CHECK(r.header == "mimic - rig stand-in rig - no pose 57");

    PoseLibrary lib;
    BakedPose p;
    p.pose_id = 57;
    BakedFrame f0, f1;
    f0.t_ms = 0;
    f1.t_ms = 1000;
    p.frames = {f0, f1};
    lib.add(p);
    q.library = &lib;
    SheetResult have = make_mimic_sheet(q);
    REQUIRE(have.ok);
    CHECK(have.notes.empty());
    CHECK(have.header == "mimic - rig stand-in rig - borrow");
}

TEST_CASE("sheet: the command writes the PNG, and each failure names its cause") {
    const fs::path dir = fs::temp_directory_path() / "kyubu_sheet_test";
    fs::create_directories(dir);
    const fs::path empty_library = dir / "no_library";
    fs::create_directories(empty_library);
    const std::string out_png = (dir / "sheet.png").string();
    fs::remove(out_png);

    SheetCommand cmd;
    cmd.clip_path = kExample;
    cmd.out_path = out_png;
    cmd.times = "0,0.5,1";
    cmd.rig_candidates = {kStandIn};
    cmd.library_dir = empty_library.string();

    auto run = [&](const SheetCommand& c, std::string* out, std::string* err) {
        std::FILE* fo = std::tmpfile();
        std::FILE* fe = std::tmpfile();
        REQUIRE(fo);
        REQUIRE(fe);
        const int code = run_sheet_command(c, fo, fe);
        *out = read_stream(fo);
        *err = read_stream(fe);
        std::fclose(fo);
        std::fclose(fe);
        return code;
    };
    std::string out, err;
    CHECK(run(cmd, &out, &err) == 0);
    CHECK(err.empty());
    CHECK(out.find("3 times x 2 views, rig stand-in rig") != std::string::npos);
    const std::string png = slurp(out_png);
    REQUIRE(png.size() > 8);
    CHECK(png.compare(1, 3, "PNG") == 0);

    SheetCommand bad = cmd;
    bad.times = "0,2";
    CHECK(run(bad, &out, &err) == 1);
    CHECK(err.find("--t takes numbers from 0 to 1") != std::string::npos);

    bad = cmd;
    bad.clip_path = (dir / "missing.anim.toml").string();
    CHECK(run(bad, &out, &err) == 1);
    CHECK(err.find("cannot read the clip") != std::string::npos);

    bad = cmd;
    bad.clip_path = KYUBU_SOURCE_DIR "/tests/fixtures/kanim/bad.anim.toml";
    CHECK(run(bad, &out, &err) == 1);
    CHECK(err.find("is not valid") != std::string::npos);

    bad = cmd;
    bad.rig_candidates = {(dir / "none.rig.toml").string()};
    CHECK(run(bad, &out, &err) == 1);
    CHECK(err.find("no rig file found") != std::string::npos);

    bad = cmd;
    bad.out_path = (dir / "no_such_folder" / "sheet.png").string();
    CHECK(run(bad, &out, &err) == 1);
    CHECK(err.find("cannot write") != std::string::npos);

    bad = cmd;
    bad.times = "0,0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1,0.95,0.99";
    CHECK(run(bad, &out, &err) == 1);
    CHECK(err.find("at most 12 times") != std::string::npos);

    fs::remove_all(dir);
}
