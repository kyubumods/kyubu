// The Animator's offline sheet: see sheet.h.
#include "sheet.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "capture_args.h"
#include "kanim_core.h"
#include "model.h"
#include "effects.h"
#include "particles.h"

namespace kanimator {

namespace {

constexpr float kFallbackBox = 8;   // a named model that cannot be read is drawn as a box this big, in voxels (as the window)

// The colour of a node's box: a fixed palette by place in the rig, so a node keeps its colour from one run to the next (the window's).
uint32_t node_color(size_t index) {
    static const uint32_t kPalette[] = {0x8C9AAE, 0x6F9FD8, 0xE0B070, 0xD96A6A, 0x6AC08A, 0xB58AD8, 0xD8C86A, 0x8AD0D0};
    return kPalette[index % (sizeof kPalette / sizeof kPalette[0])];
}

bool read_all(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

struct ScreenVertex {
    float x = 0, y = 0, z = 0;
    float r = 0, g = 0, b = 0;
    float a = 1;   // 0 to 1: a see-through triangle (`glass`) is blended by it
};

uint8_t to_byte(float v) { return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f)); }

// One triangle, filled at the pixel centres. The vertex colours are interpolated, as Direct3D does; the depth is the projected depth
// (0 near, 1 far), which is linear on the screen. `glass`: blended by the vertices' alpha over what is there, tested against the
// depth and not written to it (as the window draws a shockwave).
void fill_triangle(anim_capture::Image& img, std::vector<float>& depth, const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c,
                   bool glass = false) {
    const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (std::fabs(area) < 1e-6f) return;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int x1 = std::min(img.w - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int y1 = std::min(img.h - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    const float inv = 1.0f / area;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) * inv;
            const float w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) * inv;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
            const float z = w0 * a.z + w1 * b.z + w2 * c.z;
            float& d = depth[static_cast<size_t>(y) * img.w + x];
            if (glass) {
                if (z > d) continue;
                const float al = std::clamp(w0 * a.a + w1 * b.a + w2 * c.a, 0.0f, 1.0f);
                const uint8_t* under = img.at(x, y);
                img.set(x, y, to_byte(under[0] + (w0 * a.r + w1 * b.r + w2 * c.r - under[0]) * al),
                        to_byte(under[1] + (w0 * a.g + w1 * b.g + w2 * c.g - under[1]) * al),
                        to_byte(under[2] + (w0 * a.b + w1 * b.b + w2 * c.b - under[2]) * al));
                continue;
            }
            if (z >= d) continue;
            d = z;
            img.set(x, y, to_byte(w0 * a.r + w1 * b.r + w2 * c.r), to_byte(w0 * a.g + w1 * b.g + w2 * c.g),
                    to_byte(w0 * a.b + w1 * b.b + w2 * c.b));
        }
    }
}

void draw_line(anim_capture::Image& img, float x0, float y0, float x1, float y1, uint32_t rgb) {
    const float dx = x1 - x0, dy = y1 - y0;
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy)))));
    if (steps > 4 * (img.w + img.h)) return;   // a line far off the picture: nothing to see, and the loop would be long
    for (int i = 0; i <= steps; ++i) {
        const float u = static_cast<float>(i) / steps;
        img.set(static_cast<int>(std::floor(x0 + dx * u)), static_cast<int>(std::floor(y0 + dy * u)),
                static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb));
    }
}

}  // namespace

Mesh node_mesh(const Rig& rig, size_t node, const std::string& models_dir, std::string& note) {
    note.clear();
    const Model& m = rig.nodes[node].model;
    if (m.kind == Model::Kind::Box) return mesh_box(m.box[0], m.box[1], m.box[2], node_color(node));
    if (m.kind != Model::Kind::Named) return Mesh{};
    std::string bytes, err;
    CubModel cub;
    if (models_dir.empty()) {
        err = "no models folder (--models)";
    } else if (!read_all(models_dir + "/" + m.name, bytes)) {
        err = "not found in the models folder";
    } else if (!parse_cub(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), cub, err)) {
        // err is set by the parser
    } else {
        return mesh_cub(cub);
    }
    note = "model " + m.name + ": " + err + "; a box stands for it";
    return mesh_box(kFallbackBox, kFallbackBox, kFallbackBox, node_color(node));
}

anim_capture::Image render_figure(const std::vector<Mesh>& meshes, const std::vector<Affine>& world, const Camera& cam, int px, bool grid,
                                  const Mesh* extra, const Mesh* glass) {
    return render_figure_wh(meshes, world, cam, px, px, grid ? kSheetGridHalf : 0, kSheetGridStep, extra, glass);
}

anim_capture::Image render_figure_wh(const std::vector<Mesh>& meshes, const std::vector<Affine>& world, const Camera& cam, int w, int h,
                                     float grid_half, float grid_step, const Mesh* extra, const Mesh* glass, int line_px) {
    anim_capture::Image img(w, h, 58, 64, 78);
    Viewport vp;
    vp.w = static_cast<float>(w);
    vp.h = static_cast<float>(h);
    if (grid_half > 0 && grid_step > 0) {
        for (float g = -grid_half; g <= grid_half + 1e-3f; g += grid_step) {
            const uint32_t c = std::fabs(g) < 1e-3f ? 0x6A6D75 : 0x45484F;
            float ax, ay, bx, by;
            if (project(cam, vp, {g, -grid_half, 0}, ax, ay) && project(cam, vp, {g, grid_half, 0}, bx, by))
                for (int o = 0; o < line_px; ++o) draw_line(img, ax + o, ay, bx + o, by, c);
            if (project(cam, vp, {-grid_half, g, 0}, ax, ay) && project(cam, vp, {grid_half, g, 0}, bx, by))
                for (int o = 0; o < line_px; ++o) draw_line(img, ax, ay + o, bx, by + o, c);
        }
    }
    std::vector<float> depth(static_cast<size_t>(w) * h, 2.0f);
    auto draw = [&](const Mesh& m, const Affine& w, bool see_through = false) {
        for (size_t t = 0; t + 2 < m.tris.size(); t += 3) {
            ScreenVertex sv[3];
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
                const MeshVertex& v = m.tris[t + k];
                const float p[3] = {v.x, v.y, v.z};
                float q[3];
                w.apply(p, q);
                ok = project(cam, vp, {q[0], q[1], q[2]}, sv[k].x, sv[k].y, &sv[k].z);
                sv[k].r = static_cast<float>((v.color >> 16) & 0xFF);
                sv[k].g = static_cast<float>((v.color >> 8) & 0xFF);
                sv[k].b = static_cast<float>(v.color & 0xFF);
                sv[k].a = static_cast<float>(v.color >> 24) / 255.0f;
            }
            if (ok) fill_triangle(img, depth, sv[0], sv[1], sv[2], see_through);
        }
    };
    for (size_t i = 0; i < meshes.size() && i < world.size(); ++i) draw(meshes[i], world[i]);
    if (extra) draw(*extra, Affine{});
    if (glass) draw(*glass, Affine{}, true);
    return img;
}

std::string sheet_header(const std::string& rig_name, const std::string& clip_name, int no_pose, size_t max_chars) {
    const std::string head = "mimic - rig ";
    const std::string a = head + rig_name;
    const std::string pose = no_pose >= 0 ? " - no pose " + std::to_string(no_pose) : std::string();
    const std::string clip = clip_name.empty() ? std::string() : " - " + clip_name;
    for (const std::string& s : {a + pose + clip, a + pose, a})
        if (s.size() <= max_chars) return s;
    const size_t room = max_chars > head.size() ? max_chars - head.size() : 0;
    if (rig_name.size() <= room) return a;
    return head + (room > 3 ? rig_name.substr(0, room - 3) + "..." : rig_name.substr(0, room));
}

SheetResult make_mimic_sheet(const SheetRequest& req) {
    SheetResult r;
    if (!req.rig || !req.meshes || !req.clip) {
        r.error = "no rig or no clip to draw";
        return r;
    }
    const Rig& rig = *req.rig;
    if (req.meshes->size() != rig.nodes.size()) {
        r.error = "the meshes are not the rig's";
        return r;
    }
    if (req.fractions.empty() || req.fractions.size() > kSheetMaxTimes) {
        r.error = "give 1 to " + std::to_string(kSheetMaxTimes) + " times";
        return r;
    }
    const float length = anim::resolve_duration(*req.clip, req.ability_ms);
    if (!(length > 0)) {
        r.error = "the clip has no length: nothing to draw";
        return r;
    }
    static const PoseLibrary kNoLibrary;
    const PoseLibrary& lib = req.library ? *req.library : kNoLibrary;

    // Every frame first, so one camera can frame them all. The particles' bursts are planned once for the whole clip.
    std::vector<std::vector<Affine>> frames;
    std::vector<Mesh> sparks;   // per frame: its particles and pillars
    std::vector<Mesh> rings;    // per frame: its shockwaves (see-through)
    std::vector<Vec3> pts = {{0, 0, 0}};
    int missing_pose = -1;
    uint32_t missing_parts = 0;
    ParticlePlan plan;
    if (!rig.emitters.empty())
        plan = plan_particles(rig, *req.clip, length, [&](float ms) { return base_pose_for(lib, *req.clip, ms).pose; });
    EffectPlan effect_plan;
    if (rig.has_effects())
        effect_plan = plan_effects(rig, *req.clip, length, [&](float ms) { return base_pose_for(lib, *req.clip, ms).pose; });
    for (float f : req.fractions) {
        const float ms = std::clamp(f, 0.0f, 1.0f) * length;
        const BaseResult base = base_pose_for(lib, *req.clip, ms);
        if (base.kind == BaseResult::Kind::Missing && missing_pose < 0) missing_pose = base.pose_id;
        anim::Timing at;
        at.elapsed_ms = ms;
        at.duration_ms = length;
        at.run_ms = length;
        Solved solved = solve(rig, base.pose, *req.clip, at);
        missing_parts |= solved.missing_parts;
        for (size_t i = 0; i < req.meshes->size() && i < solved.world.size(); ++i) {
            Vec3 lo, hi;
            if (!mesh_bounds((*req.meshes)[i], lo, hi)) continue;
            for (int c = 0; c < 8; ++c) {
                const float p[3] = {c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z};
                float q[3];
                solved.world[i].apply(p, q);
                pts.push_back({q[0], q[1], q[2]});
            }
        }
        r.times_ms.push_back(std::round(static_cast<double>(ms)));
        frames.push_back(std::move(solved.world));
        const std::vector<LiveParticle> live = live_particles(rig, plan, ms, false);
        for (const LiveParticle& p : live) pts.push_back({p.pos[0], p.pos[1], p.pos[2]});
        sparks.push_back(particle_mesh(live));
        const LiveEffects fx = live_effects(rig, effect_plan, ms, false);
        const Mesh solid = pillar_mesh(rig, fx.pillars);
        sparks.back().tris.insert(sparks.back().tris.end(), solid.tris.begin(), solid.tris.end());
        rings.push_back(ring_mesh(rig, fx.rings));
        for (const Mesh* m : {&solid, static_cast<const Mesh*>(&rings.back())}) {   // framed too: a ring may reach past the figure
            Vec3 lo, hi;
            if (!mesh_bounds(*m, lo, hi)) continue;
            pts.push_back(lo);
            pts.push_back(hi);
        }
    }
    if (missing_pose >= 0) r.notes.push_back("pose " + std::to_string(missing_pose) + " is not in the pose library: the neutral pose stands for it");
    if (missing_parts) {
        std::string lack;
        for (int p = 0; p < anim::kPartCount; ++p)
            if (missing_parts & (1u << p)) lack += std::string(lack.empty() ? "" : ", ") + anim::part_name(static_cast<anim::Part>(p));
        r.notes.push_back("the rig has no node for " + lack + ": the clip's keys for them are not drawn");
    }

    Camera cam;
    const Sphere ball = bounding_sphere(pts.data(), pts.size());
    cam.frame(ball.centre, ball.radius);

    constexpr int kCols = anim_capture::kViewCount;
    std::vector<anim_capture::SheetCell> cells;
    for (size_t row = 0; row < frames.size(); ++row) {
        for (int v = 0; v < kCols; ++v) {
            Camera view = cam;
            view.set_preset(v == 0 ? ViewPreset::Front : ViewPreset::Left);
            anim_capture::SheetCell cell;
            cell.picture = render_figure(*req.meshes, frames[row], view, kSheetViewPx, req.grid, &sparks[row], &rings[row]);
            cell.label = anim_capture::frame_label(r.times_ms[row], anim_capture::kViews[v].name);
            r.labels.push_back(cell.label);
            cells.push_back(std::move(cell));
        }
    }
    r.rows = static_cast<int>(frames.size());
    r.cols = kCols;
    const anim_capture::SheetSize size = anim_capture::sheet_size(r.rows, r.cols);
    const int per_line = (size.w - 2 * anim_capture::kMargin + anim_capture::kLabelScale) /
                         (anim_capture::kGlyphAdvance * anim_capture::kLabelScale);
    r.header = sheet_header(rig.name, req.clip_name, missing_pose, static_cast<size_t>(std::max(per_line, 1)));
    r.image = anim_capture::make_sheet(r.header, cells, r.cols);
    r.png = anim_capture::encode_png(r.image);
    if (r.png.empty()) {
        r.error = "the PNG could not be encoded";
        return r;
    }
    r.ok = true;
    return r;
}

namespace {

std::string first_clip_errors(const std::vector<anim::ClipError>& errors, size_t max = 3) {
    std::string s;
    for (size_t i = 0; i < errors.size() && i < max; ++i) s += "  " + anim::format_error(errors[i]) + "\n";
    if (errors.size() > max) s += "  ... and " + std::to_string(errors.size() - max) + " more\n";
    return s;
}

}  // namespace

bool load_sheet_scene(const SheetCommand& cmd, SheetScene& scene, std::FILE* out, std::string& why) {
    auto fail = [&](const std::string& w) {
        why = w;
        return false;
    };
    std::string clip_text;
    if (!read_all(cmd.clip_path, clip_text)) return fail("cannot read the clip " + cmd.clip_path);
    std::string clip_name;
    if (!name_from_path(cmd.clip_path, clip_name)) {
        const size_t slash = cmd.clip_path.find_last_of("\\/");
        clip_name = slash == std::string::npos ? cmd.clip_path : cmd.clip_path.substr(slash + 1);
    }
    anim::ClipLoad& cl = scene.clip;
    cl = anim::parse_clip(clip_text, clip_name);
    scene.clip_name = clip_name;
    if (!cl.ok) return fail("the clip " + clip_name + " is not valid:\n" + first_clip_errors(cl.errors));

    std::vector<std::string> candidates = cmd.rig_candidates;
    if (!cmd.rig_path.empty()) candidates = {cmd.rig_path};
    std::string rig_text, rig_file;
    for (const std::string& c : candidates)
        if (!c.empty() && read_all(c, rig_text)) {
            rig_file = c;
            break;
        }
    if (rig_file.empty()) return fail("no rig file found" + (candidates.empty() ? std::string() : " (looked for " + candidates.front() + ")"));
    RigLoad& rl = scene.rig;
    rl = parse_rig(rig_text, rig_file);
    if (!rl.ok) return fail("the rig " + rig_file + " is not valid:\n" + first_clip_errors(rl.errors));

    std::vector<Mesh>& meshes = scene.meshes;
    meshes.assign(rl.rig.nodes.size(), Mesh{});
    for (size_t i = 0; i < meshes.size(); ++i) {
        std::string note;
        meshes[i] = node_mesh(rl.rig, i, cmd.models_dir, note);
        if (!note.empty()) std::fprintf(out, "note: %s\n", note.c_str());
    }

    PoseLibrary& lib = scene.library;
    std::string folder = cmd.library_dir;
    if (folder.empty()) {
        std::string settings;
        const std::string sp = settings_file_path();
        if (!sp.empty() && read_all(sp, settings)) folder = parse_settings(settings).library_folder;
    }
    if (!folder.empty()) {
        const PoseLibrary::Scan scan = lib.scan(folder);
        if (cl.clip.borrow.present)
            for (const std::string& p : scan.problems) std::fprintf(out, "note: pose library: %s\n", p.c_str());
    }
    return true;
}

int run_sheet_command(const SheetCommand& cmd, std::FILE* out, std::FILE* err) {
    auto fail = [&](const std::string& why) {
        std::fprintf(err, "kyubu-animator --sheet: %s\n", why.c_str());
        return 1;
    };
    std::vector<float> fractions;
    if (!kyubu::kanim::parse_times(cmd.times, fractions)) return fail("--t takes numbers from 0 to 1, such as 0,0.5");
    if (fractions.size() > kSheetMaxTimes) return fail("at most " + std::to_string(kSheetMaxTimes) + " times in one sheet");
    SheetScene scene;
    std::string why;
    if (!load_sheet_scene(cmd, scene, out, why)) return fail(why);
    const RigLoad& rl = scene.rig;
    const anim::ClipLoad& cl = scene.clip;

    SheetRequest req;
    req.rig = &rl.rig;
    req.meshes = &scene.meshes;
    req.clip = &cl.clip;
    req.library = &scene.library;
    req.clip_name = scene.clip_name;
    req.ability_ms = cmd.ability_ms;
    req.fractions = fractions;
    SheetResult sheet = make_mimic_sheet(req);
    if (!sheet.ok) return fail(sheet.error);
    for (const std::string& n : sheet.notes) std::fprintf(out, "note: %s\n", n.c_str());

    std::ofstream f(cmd.out_path, std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(sheet.png.data()), static_cast<std::streamsize>(sheet.png.size()));
    if (!f) return fail("cannot write " + cmd.out_path);
    f.close();
    std::fprintf(out, "wrote %s: %d times x %d views, rig %s, %dx%d px\n", cmd.out_path.c_str(), sheet.rows, sheet.cols, rl.rig.name.c_str(),
                 sheet.image.w, sheet.image.h);
    return 0;
}

}  // namespace kanimator
