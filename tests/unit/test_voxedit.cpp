// Unit tests of kyubu-voxedit's core (tools/voxedit/voxel.h, doc.h): the grid and its .cub bytes, picking, the tools' cells,
// strokes and undo, growth and trimming that keep a piece in place, mirror across pieces of a rig, and the chunk mesher.
#include "doctest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doc.h"
#include "voxel.h"

using namespace kvox;

namespace {

const Rgb kRed{200, 30, 30}, kBlue{30, 30, 200};

Grid filled(int sx, int sy, int sz, Rgb c) {
    Grid g;
    g.resize(sx, sy, sz);
    for (size_t i = 0; i < g.cells(); ++i) g.put(i, c);
    return g;
}

std::filesystem::path temp_folder(const char* name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_file(const std::filesystem::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary);
    f << bytes;
}

}  // namespace

TEST_CASE("voxedit: a grid round-trips through .cub bytes") {
    Grid g;
    g.resize(3, 2, 4);
    g.put(g.index(2, 1, 3), kRed);
    const std::string b = cub_bytes(g);
    REQUIRE(b.size() == 12 + 3 * 24);
    Grid h;
    std::string err;
    REQUIRE(parse_grid(reinterpret_cast<const uint8_t*>(b.data()), b.size(), h, err));
    CHECK(h.size[0] == 3);
    CHECK(h.size[2] == 4);
    CHECK(h.get(2, 1, 3) == kRed);
    CHECK(h.count() == 1);
    CHECK_FALSE(parse_grid(reinterpret_cast<const uint8_t*>(b.data()), b.size() - 1, h, err));
}

TEST_CASE("voxedit: black paints as (1,1,1), never as empty") {
    Grid g;
    g.resize(2, 2, 2);
    Stroke s;
    s.begin(0, g);
    CHECK(s.apply(g, {0, 0, 0}, Op::Add, Rgb{0, 0, 0}));
    CHECK(g.solid(0, 0, 0));
    CHECK(g.get(0, 0, 0) == Rgb{1, 1, 1});
}

TEST_CASE("voxedit: a ray hits the first shown voxel and the face it enters") {
    Grid g;
    g.resize(4, 4, 4);   // spans -2..2 on each axis
    g.put(g.index(1, 2, 3), kRed);
    g.put(g.index(1, 2, 1), kBlue);
    Slice all;
    // From above, straight down through cell x 1, y 2.
    Hit h = raycast(g, all, {-0.5f, 0.5f, 10}, {0, 0, -1});
    REQUIRE(h.solid);
    CHECK(h.cell == Cell{1, 2, 3});
    CHECK(h.normal[2] == 1);
    Cell add;
    REQUIRE(h.add_cell(add));
    CHECK(add == Cell{1, 2, 4});   // outside the grid: an Add grows it
    // A slice that hides z 3 shows the blue one.
    Slice low;
    low.on = true;
    low.hi[0] = low.hi[1] = 3;
    low.hi[2] = 2;
    h = raycast(g, low, {-0.5f, 0.5f, 10}, {0, 0, -1});
    REQUIRE(h.solid);
    CHECK(h.cell == Cell{1, 2, 1});
    // From the side along -x.
    h = raycast(g, all, {10, 0.5f, 1.5f}, {-1, 0, 0});
    REQUIRE(h.solid);
    CHECK(h.cell == Cell{1, 2, 3});
    CHECK(h.normal[0] == 1);
    // Where no voxel is: the floor.
    h = raycast(g, all, {1.5f, -1.5f, 10}, {0, 0, -1});
    CHECK_FALSE(h.solid);
    REQUIRE(h.floor);
    CHECK(h.cell == Cell{3, 0, 0});
    // Missing the grid.
    CHECK_FALSE(raycast(g, all, {10, 10, 10}, {0, 0, -1}).any());
}

TEST_CASE("voxedit: brush, box, connected and colour cells") {
    CHECK(brush_cells({5, 5, 5}, 1, Shape::Cube).size() == 1);
    CHECK(brush_cells({5, 5, 5}, 3, Shape::Cube).size() == 27);
    const auto sphere = brush_cells({0, 0, 0}, 5, Shape::Sphere);
    CHECK(sphere.size() < 125);
    CHECK(sphere.size() > 50);
    CHECK(box_cells({2, 0, 0}, {0, 1, 0}).size() == 6);

    Grid g = filled(4, 1, 1, kRed);
    g.put(g.index(2, 0, 0), kBlue);
    Slice all;
    CHECK(connected_cells(g, all, {0, 0, 0}).size() == 2);   // the blue one cuts it
    CHECK(cells_of_colour(g, all, kRed).size() == 3);
    CHECK(mirror_x(g, {0, 0, 0}) == Cell{3, 0, 0});
}

TEST_CASE("voxedit: a stroke is one undo step, and undo and redo restore the revision") {
    Doc d;
    d.new_model(8, 8, 8);
    Piece& p = d.pieces[0];
    p.saved_rev = p.rev;
    d.begin_stroke();
    CHECK(d.apply({{1, 1, 1}, {2, 1, 1}}, Op::Add, kRed, false) == 2);
    CHECK(d.apply({{2, 1, 1}, {3, 1, 1}}, Op::Add, kRed, false) == 1);
    d.end_stroke("add");
    CHECK(p.grid.count() == 3);
    CHECK(p.dirty());
    REQUIRE(d.undo());
    CHECK(p.grid.count() == 0);
    CHECK_FALSE(p.dirty());
    REQUIRE(d.redo());
    CHECK(p.grid.count() == 3);
    CHECK(d.undo_name() == "add");
    // Paint touches solid cells only.
    d.begin_stroke();
    CHECK(d.apply(box_cells({0, 1, 1}, {4, 1, 1}), Op::Paint, kBlue, false) == 3);
    d.end_stroke("paint");
    CHECK(p.grid.get(1, 1, 1) == kBlue);
    CHECK_FALSE(p.grid.solid(0, 1, 1));
}

TEST_CASE("voxedit: an Add outside grows the grid on both sides and keeps the piece in place") {
    Doc d;
    d.new_model(4, 4, 4);
    Piece& p = d.pieces[0];
    d.begin_stroke();
    d.apply({{1, 1, 1}}, Op::Add, kRed, false);
    const Vec3 world_before = p.grid.centre_of({1, 1, 1});
    d.apply({{5, 1, 1}}, Op::Add, kBlue, false);   // two past the +x side
    d.end_stroke("add");
    CHECK(p.grid.size[0] == 8);
    CHECK(p.grid.size[1] == 4);
    // The red voxel moved by 2 cells but sits at the same place.
    CHECK(p.grid.get(3, 1, 1) == kRed);
    const Vec3 world_after = p.grid.centre_of({3, 1, 1});
    CHECK(world_after.x == doctest::Approx(world_before.x));
    CHECK(p.grid.get(7, 1, 1) == kBlue);
    REQUIRE(d.undo());
    CHECK(p.grid.size[0] == 4);
    CHECK(p.grid.count() == 0);
    REQUIRE(d.redo());
    CHECK(p.grid.size[0] == 8);
    CHECK(p.grid.count() == 2);
    // Trim cuts as many empty layers from both sides: x has margins 3 and 0 (nothing cut), y and z have 1 and 2 (1 cut each).
    REQUIRE(d.trim_active());
    CHECK(p.grid.size[0] == 8);
    CHECK(p.grid.size[1] == 2);
    CHECK(p.grid.size[2] == 2);
    CHECK(p.grid.get(3, 0, 0) == kRed);
}

TEST_CASE("voxedit: the plane lock keeps a dragged Add on one layer") {
    Doc d;
    d.new_model(8, 8, 8);
    d.begin_stroke();
    d.lock_plane(2, 0);
    CHECK(d.apply({{1, 1, 0}}, Op::Add, kRed, false) == 1);
    CHECK(d.apply({{1, 1, 1}}, Op::Add, kRed, false) == 0);   // the voxel just added, stacked: refused
    d.end_stroke("add");
    CHECK(d.pieces[0].grid.count() == 1);
}

TEST_CASE("voxedit: mirror repeats an edit at -x on the same model") {
    Doc d;
    d.new_model(10, 4, 4);
    d.begin_stroke();
    CHECK(d.apply({{1, 2, 2}}, Op::Add, kRed, true) == 2);
    d.end_stroke("add");
    CHECK(d.pieces[0].grid.solid(8, 2, 2));
}

TEST_CASE("voxedit: a rig opens every model at its rest place, and mirror lands on the other side's piece") {
    const auto dir = temp_folder("kyubu-voxedit-test");
    Grid g = filled(4, 4, 4, kRed);
    write_file(dir / "l.cub", cub_bytes(g));
    write_file(dir / "r.cub", cub_bytes(g));
    write_file(dir / "t.rig.toml",
               "name = \"pair\"\n"
               "[[node]]\nname = \"root\"\nsteps = [[\"translate\", 0, 0, 0]]\n"
               "[[node]]\nname = \"left\"\nparent = \"root\"\nmodel = \"l.cub\"\nsteps = [[\"translate\", -6, 0, 2]]\n"
               "[[node]]\nname = \"right\"\nparent = \"root\"\nmodel = \"r.cub\"\nsteps = [[\"translate\", 6, 0, 2]]\n"
               "[[node]]\nname = \"post\"\nparent = \"root\"\nmodel = \"box 2 2 6\"\nsteps = [[\"translate\", 0, 0, 3]]\n");
    Doc d;
    std::string err;
    REQUIRE_MESSAGE(d.open_rig((dir / "t.rig.toml").string(), "", err), err);
    REQUIRE(d.pieces.size() == 3);
    CHECK(d.pieces[0].name == "left");
    CHECK(d.pieces[0].world.t[0] == doctest::Approx(-6));
    CHECK_FALSE(d.pieces[2].editable);   // the box
    CHECK(d.active == 0);
    // Remove the left piece's outer corner column (x 0, world x -8 + 0.5); the mirror removes the right piece's x 3.
    d.begin_stroke();
    CHECK(d.apply({{0, 0, 0}}, Op::Remove, {}, true) == 2);
    d.end_stroke("remove");
    CHECK_FALSE(d.pieces[0].grid.solid(0, 0, 0));
    CHECK_FALSE(d.pieces[1].grid.solid(3, 0, 0));
    CHECK(d.pieces[1].grid.count() == 63);
    CHECK(d.pieces[0].dirty());
    CHECK(d.pieces[1].dirty());
    REQUIRE(d.undo());   // one step for both
    CHECK(d.pieces[1].grid.count() == 64);
    CHECK_FALSE(d.any_dirty());
    std::filesystem::remove_all(dir);
}

TEST_CASE("voxedit: the chunk mesher makes outside faces only and honours the slice") {
    Grid g = filled(2, 1, 1, kRed);
    Slice all;
    CHECK(mesh_chunk(g, all, 0, 0, 0).triangle_count() == 10 * 2);   // 2 cubes, 10 outside faces
    Slice one;
    one.on = true;
    one.hi[0] = 0;
    CHECK(mesh_chunk(g, one, 0, 0, 0).triangle_count() == 6 * 2);   // the shown cube alone, its cut face made
    Grid big = filled(20, 1, 1, kRed);
    CHECK(chunk_count(big, 0) == 2);
    CHECK(mesh_chunk(big, all, 0, 0, 0).triangle_count() + mesh_chunk(big, all, 1, 0, 0).triangle_count() == (20 * 4 + 2) * 2);
}

TEST_CASE("voxedit: colours by count") {
    Grid g = filled(3, 1, 1, kRed);
    g.put(0, kBlue);
    const auto c = colour_counts(g, 8);
    REQUIRE(c.size() == 2);
    CHECK(c[0].c == kRed);
    CHECK(c[0].n == 2);
}
