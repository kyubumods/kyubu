// The Animator's parity check (openspec add-ability-animation, task 7.5). See parity.h.
#include "parity.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include "cubmesh.h"
#include "json.hpp"
#include "sheet.h"

namespace kanimator {

namespace {

using json = nlohmann::json;
namespace anim = kyubu::game::anim;
namespace fs = std::filesystem;

constexpr const char* kFormat = "kyubu-capture/1";

std::string where(const std::string& source, const std::string& field) { return source + ": " + field; }

// A number that may be `null` (the mod writes null for a value that is not finite).
bool read_number(const json& v, float& out) {
    if (v.is_null()) {
        out = 0;
        return true;
    }
    if (!v.is_number()) return false;
    const double d = v.get<double>();
    out = std::isfinite(d) ? static_cast<float>(d) : 0.0f;
    return true;
}

bool read_vector(const json& v, int n, float out[3]) {
    if (!v.is_array() || static_cast<int>(v.size()) < n || v.size() > 3) return false;
    for (int i = 0; i < n; ++i)
        if (!read_number(v[i], out[i])) return false;
    return true;
}

int pos_floats(anim::Part p) { return p == anim::Part::Tail ? 0 : p == anim::Part::Root ? 2 : 3; }

}  // namespace

// ---- the capture file

CaptureLoad parse_capture(const std::string& text, const std::string& source) {
    CaptureLoad r;
    if (text.size() > kMaxCaptureBytes) {
        r.error = where(source, "the file is larger than the limit");
        return r;
    }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        r.error = where(source, "not a JSON object");
        return r;
    }
    auto str_of = [&](const char* key) -> std::string {
        auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    if (str_of("format") != kFormat) {
        r.error = where(source, std::string("`format` is not ") + kFormat);
        return r;
    }
    Capture& c = r.capture;
    c.source = source;
    c.kind = str_of("kind");
    c.clip = str_of("clip");
    c.build = str_of("build");
    auto fr = j.find("frames");
    if (fr == j.end() || !fr->is_array() || fr->empty()) {
        r.error = where(source, "`frames` is missing or empty");
        return r;
    }
    if (fr->size() > kMaxCaptureFrames) {
        r.error = where(source, "`frames` has more frames than the limit");
        return r;
    }
    c.frames.reserve(fr->size());
    for (size_t i = 0; i < fr->size(); ++i) {
        const json& f = (*fr)[i];
        const std::string at = "frames[" + std::to_string(i) + "]";
        if (!f.is_object()) {
            r.error = where(source, at + " is not an object");
            return r;
        }
        CaptureFrame cf;
        cf.index = static_cast<int>(i);
        if (auto ix = f.find("index"); ix != f.end() && ix->is_number_integer()) cf.index = static_cast<int>(ix->get<long long>());
        if (auto t = f.find("t_ms"); t != f.end() && t->is_number()) cf.t_ms = t->get<double>();
        if (auto v = f.find("view"); v != f.end() && v->is_string()) cf.view = v->get<std::string>();
        if (auto y = f.find("body_yaw"); y != f.end() && y->is_number() && std::isfinite(y->get<double>())) cf.body_yaw = y->get<double>();
        auto vec = f.find("vectors");
        if (vec == f.end() || !vec->is_object()) {
            r.error = where(source, at + ".vectors is missing");
            return r;
        }
        for (int p = 0; p < anim::kPartCount; ++p) {
            const anim::Part part = static_cast<anim::Part>(p);
            const std::string name = anim::part_name(part);
            auto pv = vec->find(name);
            if (pv == vec->end() || !pv->is_object()) {
                r.error = where(source, at + ".vectors." + name + " is missing");
                return r;
            }
            anim::PartPose& pp = cf.pose.parts[p];
            auto rot = pv->find("rot");
            if (rot == pv->end() || !read_vector(*rot, 3, pp.rot)) {
                r.error = where(source, at + ".vectors." + name + ".rot is not three numbers");
                return r;
            }
            const int n = pos_floats(part);
            if (n > 0) {
                auto pos = pv->find("pos");
                if (pos == pv->end() || !read_vector(*pos, n, pp.pos)) {
                    r.error = where(source, at + ".vectors." + name + ".pos is not " + std::to_string(n) + " numbers");
                    return r;
                }
            }
        }
        auto mats = f.find("matrices");
        if (mats == f.end() || !mats->is_array() || static_cast<int>(mats->size()) != kCaptureSlots) {
            r.error = where(source, at + ".matrices is not " + std::to_string(kCaptureSlots) + " arrays");
            return r;
        }
        for (int s = 0; s < kCaptureSlots; ++s) {
            const json& m = (*mats)[s];
            if (!m.is_array() || m.size() != 16) {
                r.error = where(source, at + ".matrices[" + std::to_string(s) + "] is not 16 numbers");
                return r;
            }
            bool any = false, finite = true;
            for (int k = 0; k < 16; ++k) {
                if (m[k].is_null()) finite = false;   // a hidden part's matrix has numbers that are not finite
                if (!read_number(m[k], cf.matrix[s][k])) {
                    r.error = where(source, at + ".matrices[" + std::to_string(s) + "][" + std::to_string(k) + "] is not a number");
                    return r;
                }
                any = any || cf.matrix[s][k] != 0.0f;
            }
            cf.has_matrix[s] = any && finite;
        }
        c.frames.push_back(std::move(cf));
    }
    r.ok = true;
    return r;
}

// ---- the compare

namespace {

using V3 = std::array<double, 3>;

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double length(const V3& a) { return std::sqrt(dot(a, a)); }

// Three axes (the images of x, y and z), made orthonormal and right-handed in the sense the input had: the first two are
// Gram-Schmidt'd, the third is their cross product, negated when the input's third axis pointed against it (a mirrored model).
// False when the first two are not independent.
struct Basis {
    V3 axis[3];
    bool mirrored = false;
};
bool orthonormal(const V3& a0, const V3& a1, const V3& a2, Basis& out) {
    const double l0 = length(a0);
    if (!(l0 > 1e-12)) return false;
    const V3 u0 = {a0[0] / l0, a0[1] / l0, a0[2] / l0};
    const double d = dot(a1, u0);
    const V3 w = {a1[0] - d * u0[0], a1[1] - d * u0[1], a1[2] - d * u0[2]};
    const double lw = length(w);
    if (!(lw > 1e-12)) return false;
    const V3 u1 = {w[0] / lw, w[1] / lw, w[2] / lw};
    V3 u2 = cross(u0, u1);
    out.mirrored = dot(u2, a2) < 0;
    if (out.mirrored)
        for (double& x : u2) x = -x;
    out.axis[0] = u0;
    out.axis[1] = u1;
    out.axis[2] = u2;
    return true;
}

// The rig's side: the node's world matrix, columns being the images of the model axes.
bool rig_basis(const Affine& a, Basis& out) {
    V3 c[3];
    for (int j = 0; j < 3; ++j) c[j] = {a.r[0][j], a.r[1][j], a.r[2][j]};
    return orthonormal(c[0], c[1], c[2], out);
}

// A vector of the game's frame in the rig's: the figure is turned about z by the frame's body_yaw (`map.yaw_sign` says which way), so
// turn it back.
V3 unturn(const V3& v, const CaptureMap& map, double body_yaw_deg) {
    const double a = -map.yaw_sign * body_yaw_deg * 3.14159265358979323846 / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    return {c * v[0] - s * v[1], s * v[0] + c * v[1], v[2]};
}

// The capture's side: the matrix's three rows are the images of the model axes, in the game's frame.
bool capture_basis(const float m[16], const CaptureMap& map, double body_yaw_deg, Basis& out) {
    V3 r[3];
    for (int i = 0; i < 3; ++i) r[i] = unturn({m[4 * i], m[4 * i + 1], m[4 * i + 2]}, map, body_yaw_deg);
    return orthonormal(r[0], r[1], r[2], out);
}

double angle_between(const Basis& a, const Basis& b) {
    double tr = 0;   // trace of A^T B, the axes being the columns
    for (int i = 0; i < 3; ++i) tr += dot(a.axis[i], b.axis[i]);
    const double c = std::max(-1.0, std::min(1.0, (tr - 1.0) / 2.0));
    return std::acos(c) * 180.0 / 3.14159265358979323846;
}

}  // namespace

ParityReport run_parity(const Rig& rig, const std::vector<std::array<float, 3>>& sizes, const std::vector<Capture>& captures) {
    ParityReport rep;
    if (!rig.capture.present) {
        rep.error = "the rig has no `[capture]` table: it does not say how a capture's matrices sit in its frame";
        return rep;
    }
    if (rig.root < 0 || rig.nodes[rig.root].slot < 0) {
        rep.error = "the rig's root node has no `slot`: the other nodes are measured from it";
        return rep;
    }
    if (sizes.size() != rig.nodes.size()) {
        rep.error = "the model sizes are not one per node";
        return rep;
    }
    const int ref_slot = rig.nodes[rig.root].slot;
    const CaptureMap& map = rig.capture;
    for (size_t i = 0; i < rig.nodes.size(); ++i) {
        if (static_cast<int>(i) == rig.root || rig.nodes[i].slot < 0) continue;
        NodeParity np;
        np.node = static_cast<int>(i);
        np.slot = rig.nodes[i].slot;
        rep.nodes.push_back(np);
    }
    for (const Capture& cap : captures) {
        ++rep.captures;
        for (const CaptureFrame& f : cap.frames) {
            ++rep.frames;
            if (!f.has_matrix[ref_slot]) {
                ++rep.skipped;
                continue;
            }
            const Solved solved = solve_pose(rig, f.pose);
            const Affine& rig_root = solved.world[rig.root];
            const float* rm = f.matrix[ref_slot];
            const V3 cap_root = {rm[12], rm[13], rm[14]};
            const std::string label = cap.source + " frame " + std::to_string(f.index);
            for (NodeParity& np : rep.nodes) {
                if (!f.has_matrix[np.slot]) continue;
                const Affine& w = solved.world[np.node];
                Basis rb, cb;
                if (!rig_basis(w, rb) || !capture_basis(f.matrix[np.slot], map, f.body_yaw, cb)) continue;
                // The capture's centre of the node, relative to the root's origin, in voxels.
                const float* m = f.matrix[np.slot];
                V3 c = unturn(sub({m[12], m[13], m[14]}, cap_root), map, f.body_yaw);
                for (double& x : c) x /= map.unit;
                if (map.corner) {
                    const std::array<float, 3>& sz = sizes[np.node];
                    for (int i = 0; i < 3; ++i)
                        for (int k = 0; k < 3; ++k) c[k] += 0.5 * sz[i] * cb.axis[i][k];
                }
                const V3 rig_c = sub({w.t[0], w.t[1], w.t[2]}, {rig_root.t[0], rig_root.t[1], rig_root.t[2]});
                const double pe = length(sub(rig_c, c));
                ++np.frames;
                if (rb.mirrored != cb.mirrored) {
                    ++np.mirrored;
                    if (np.mirrored == 1) np.angle_at = label;
                } else {
                    const double ae = angle_between(rb, cb);
                    if (ae > np.max_angle) {
                        np.max_angle = ae;
                        if (np.mirrored == 0) np.angle_at = label;
                    }
                }
                if (pe > np.max_pos) {
                    np.max_pos = pe;
                    np.pos_at = label;
                }
            }
        }
    }
    if (rep.skipped > 0)
        rep.notes.push_back(std::to_string(rep.skipped) + " frame(s) have no matrix for the root (slot " + std::to_string(ref_slot) + "): not compared");
    for (const NodeParity& np : rep.nodes)
        if (np.frames == 0)
            rep.notes.push_back("node " + rig.nodes[np.node].name + " (slot " + std::to_string(np.slot) + "): the game left no matrix there in any frame");
    rep.ok = true;
    return rep;
}

namespace {

std::string fmt(const char* f, double a) {
    char b[64];
    std::snprintf(b, sizeof b, f, a);
    return b;
}

std::string pad(const std::string& s, size_t n) { return s.size() >= n ? s : s + std::string(n - s.size(), ' '); }
std::string lpad(const std::string& s, size_t n) { return s.size() >= n ? s : std::string(n - s.size(), ' ') + s; }

}  // namespace

std::string parity_table(const Rig& rig, const ParityReport& rep, const Tolerance& tol) {
    std::ostringstream o;
    o << "parity: rig \"" << rig.name << "\", " << rep.captures << " capture(s), " << rep.frames << " frame(s), tolerance "
      << fmt("%g", tol.pos) << " voxel and " << fmt("%g", tol.angle) << " degree\n";
    o << pad("node", 16) << lpad("slot", 5) << lpad("frames", 8) << lpad("pos (voxel)", 13) << lpad("angle (deg)", 13) << "  verdict\n";
    int outside = 0;
    for (const NodeParity& np : rep.nodes) {
        std::string verdict;
        if (np.frames == 0) {
            verdict = "no matrix";
        } else if (np.outside(tol)) {
            ++outside;
            verdict = "APPROXIMATE (";
            bool first = true;
            auto add = [&](const char* w) {
                verdict += (first ? "" : ", ") + std::string(w);
                first = false;
            };
            if (np.max_pos > tol.pos) add("position");
            if (np.max_angle > tol.angle) add("angle");
            if (np.mirrored > 0) add("mirror");
            verdict += ")";
        } else {
            verdict = "ok";
        }
        o << pad(rig.nodes[np.node].name, 16) << lpad(std::to_string(np.slot), 5) << lpad(std::to_string(np.frames), 8)
          << lpad(fmt("%.2f", np.max_pos), 13) << lpad(np.mirrored > 0 ? "mirror" : fmt("%.2f", np.max_angle), 13) << "  " << verdict << "\n";
    }
    if (outside == 0) {
        o << "every compared node is within the tolerance\n";
    } else {
        o << outside << " node(s) outside the tolerance; the worst frames:\n";
        for (const NodeParity& np : rep.nodes) {
            if (!np.outside(tol)) continue;
            const std::string& name = rig.nodes[np.node].name;
            if (np.max_pos > tol.pos) o << "  " << name << ": position " << fmt("%.2f", np.max_pos) << " voxel at " << np.pos_at << "\n";
            if (np.mirrored > 0)
                o << "  " << name << ": mirrored differently from the game in " << np.mirrored << " frame(s), first at " << np.angle_at << "\n";
            else if (np.max_angle > tol.angle)
                o << "  " << name << ": angle " << fmt("%.2f", np.max_angle) << " degree at " << np.angle_at << "\n";
        }
    }
    return o.str();
}

// ---- marking a rig file

namespace {

std::string trim_left(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// True when `line` (without its indent) is `<key> = ...`; `eq` is set to the position of the '='.
bool key_line(const std::string& line, const char* key, size_t& eq) {
    const std::string t = trim_left(line);
    const std::string k = key;
    if (t.compare(0, k.size(), k) != 0) return false;
    size_t i = k.size();
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
    if (i >= t.size() || t[i] != '=') return false;
    eq = (line.size() - t.size()) + i;
    return true;
}

}  // namespace

bool mark_approximate(const std::string& text, const std::vector<std::string>& nodes, std::string& out, std::string& err) {
    std::vector<std::string> lines;   // each keeps its '\r' when it has one
    {
        size_t start = 0;
        while (start <= text.size()) {
            size_t nl = text.find('\n', start);
            if (nl == std::string::npos) {
                lines.push_back(text.substr(start));
                break;
            }
            lines.push_back(text.substr(start, nl - start));   // without the '\n'
            start = nl + 1;
        }
    }
    struct Block {
        std::string name;
        int name_line = -1, approx_line = -1;
    };
    std::vector<Block> blocks;
    bool in_node = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string t = trim_left(lines[i]);
        while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
        if (t == "[[node]]") {
            blocks.emplace_back();
            in_node = true;
            continue;
        }
        if (t == "[capture]") {
            in_node = false;
            continue;
        }
        if (!in_node) continue;
        size_t eq = 0;
        Block& b = blocks.back();
        if (b.name_line < 0 && key_line(lines[i], "name", eq)) {
            const size_t q1 = lines[i].find('"', eq);
            const size_t q2 = q1 == std::string::npos ? q1 : lines[i].find('"', q1 + 1);
            if (q2 != std::string::npos) {
                b.name = lines[i].substr(q1 + 1, q2 - q1 - 1);
                b.name_line = static_cast<int>(i);
            }
        } else if (b.approx_line < 0 && key_line(lines[i], "approximate", eq)) {
            b.approx_line = static_cast<int>(i);
        }
    }
    std::vector<std::pair<int, std::string>> inserts;   // after this line
    for (const std::string& node : nodes) {
        const Block* hit = nullptr;
        for (const Block& b : blocks)
            if (b.name == node) hit = &b;
        if (!hit || hit->name_line < 0) {
            err = "no node named \"" + node + "\" in the rig text";
            return false;
        }
        const char* value = "true";
        if (hit->approx_line >= 0) {
            std::string& l = lines[hit->approx_line];
            size_t eq = 0;
            key_line(l, "approximate", eq);
            const bool cr = !l.empty() && l.back() == '\r';
            std::string tail;
            const size_t hash = l.find('#', eq);
            if (hash != std::string::npos) {
                tail = l.substr(hash);   // the comment, with its '\r'
                if (tail.back() == '\r') tail.pop_back();
                tail = "  " + tail;
            }
            l = l.substr(0, eq + 1) + " " + value + tail + (cr ? "\r" : "");
        } else {
            const std::string& nl = lines[hit->name_line];
            const size_t indent = nl.size() - trim_left(nl).size();
            const bool cr = !nl.empty() && nl.back() == '\r';
            inserts.emplace_back(hit->name_line, nl.substr(0, indent) + "approximate = " + value + (cr ? "\r" : ""));
        }
    }
    std::sort(inserts.begin(), inserts.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& ins : inserts) lines.insert(lines.begin() + ins.first + 1, ins.second);
    out.clear();
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) out += '\n';
    }
    return true;
}

// ---- the command

namespace {

bool read_all(const std::string& path, std::string& out) {
    std::ifstream f(fs::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

fs::path to_path(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
std::string path_text(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// A `pose.json`, a folder holding one, or a folder of folders that hold one (sorted by name). Empty when there is none.
std::vector<fs::path> capture_files(const std::string& arg) {
    std::vector<fs::path> files;
    std::error_code ec;
    const fs::path p = to_path(arg);
    if (fs::is_regular_file(p, ec)) {
        files.push_back(p);
    } else if (fs::is_regular_file(p / "pose.json", ec)) {
        files.push_back(p / "pose.json");
    } else if (fs::is_directory(p, ec)) {
        for (const fs::directory_entry& e : fs::directory_iterator(p, ec))
            if (e.is_directory(ec) && fs::is_regular_file(e.path() / "pose.json", ec)) files.push_back(e.path() / "pose.json");
        std::sort(files.begin(), files.end());
    }
    return files;
}

}  // namespace

int run_parity_command(const ParityCommand& cmd, std::FILE* out, std::FILE* err) {
    auto fail = [&](const std::string& why) {
        std::fprintf(err, "kyubu-animator --parity: %s\n", why.c_str());
        return 1;
    };
    if (cmd.rig_path.empty()) return fail("--rig <file.rig.toml> is needed: the stand-in rig has no game matrices to be compared with");
    std::string rig_text;
    if (!read_all(cmd.rig_path, rig_text)) return fail("cannot read the rig " + cmd.rig_path);
    const RigLoad rl = parse_rig(rig_text, path_text(to_path(cmd.rig_path).filename()));
    if (!rl.ok) {
        for (const ClipError& e : rl.errors) std::fprintf(err, "%s\n", anim::format_error(e).c_str());
        return fail("the rig is not valid");
    }

    std::vector<Capture> captures;
    for (const std::string& arg : cmd.capture_paths) {
        const std::vector<fs::path> files = capture_files(arg);
        if (files.empty()) return fail("no pose.json at " + arg);
        for (const fs::path& f : files) {
            std::string text;
            const std::string file = path_text(f);
            if (!read_all(file, text)) return fail("cannot read " + file);
            // The folder's name says more than "pose.json".
            const std::string label = f.filename() == "pose.json" ? path_text(f.parent_path().filename()) : path_text(f.filename());
            CaptureLoad cl = parse_capture(text, label);
            if (!cl.ok) return fail(cl.error);
            if (cl.capture.kind == "bake-replay")
                std::fprintf(err, "note: %s is the replay of a bake, not the game's own draws\n", label.c_str());
            captures.push_back(std::move(cl.capture));
        }
    }

    // The size of a model is the one its file declares, not the bounds of its voxels: the game puts the matrix at the corner of the
    // declared box, and the Animator's mesh is centred on it (cubmesh.h).
    std::vector<std::array<float, 3>> sizes(rl.rig.nodes.size(), {0, 0, 0});
    for (size_t i = 0; i < rl.rig.nodes.size(); ++i) {
        const Model& m = rl.rig.nodes[i].model;
        if (m.kind == Model::Kind::Box) {
            sizes[i] = {m.box[0], m.box[1], m.box[2]};
        } else if (m.kind == Model::Kind::Named) {
            std::string bytes, why;
            CubModel cub;
            if (cmd.models_dir.empty()) why = "no models folder (--models)";
            else if (!read_all(cmd.models_dir + "/" + m.name, bytes)) why = "not found in the models folder";
            else if (parse_cub(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), cub, why)) {
                sizes[i] = {static_cast<float>(cub.size[0]), static_cast<float>(cub.size[1]), static_cast<float>(cub.size[2])};
                continue;
            }
            std::fprintf(err, "note: model %s: %s; its size is taken as 0, so a node at a corner is off by half of it\n", m.name.c_str(),
                         why.c_str());
        }
    }

    const ParityReport rep = run_parity(rl.rig, sizes, captures);
    if (!rep.ok) return fail(rep.error);
    for (const std::string& n : rep.notes) std::fprintf(err, "note: %s\n", n.c_str());
    std::fputs(parity_table(rl.rig, rep, cmd.tol).c_str(), out);

    bool any_outside = false;
    std::vector<std::string> marks;
    for (const NodeParity& np : rep.nodes) {
        if (!np.outside(cmd.tol)) continue;
        any_outside = true;
        marks.push_back(rl.rig.nodes[np.node].name);
    }
    if (cmd.mark) {
        std::string marked, why;
        if (!mark_approximate(rig_text, marks, marked, why)) return fail(why);
        if (marked != rig_text) {
            std::ofstream o(to_path(cmd.rig_path), std::ios::binary | std::ios::trunc);
            o << marked;
            if (!o) return fail("cannot write " + cmd.rig_path);
            std::fprintf(out, "rig file updated: `approximate = true` on %zu node(s)\n", marks.size());
        } else {
            std::fprintf(out, "rig file unchanged: every node outside the tolerance was marked already\n");
        }
    }
    return any_outside ? 3 : 0;
}

}  // namespace kanimator
