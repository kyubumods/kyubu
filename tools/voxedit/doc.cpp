#include "doc.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace kvox {

Affine inverse(const Affine& a) {
    const float(*m)[3] = a.r;
    const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Affine o;
    if (std::fabs(det) < 1e-12f) return o;
    const float k = 1.0f / det;
    o.r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * k;
    o.r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
    o.r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
    o.r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * k;
    o.r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
    o.r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
    o.r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * k;
    o.r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
    o.r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
    for (int i = 0; i < 3; ++i) o.t[i] = -(o.r[i][0] * a.t[0] + o.r[i][1] * a.t[1] + o.r[i][2] * a.t[2]);
    return o;
}

namespace {

bool read_bytes(const std::string& path, std::string& out) {
    std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);   // a UTF-8 path
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string file_name(const std::string& path) {
    const size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? path : path.substr(s + 1);
}

std::string folder_of(const std::string& path) {
    const size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? std::string(".") : path.substr(0, s);
}

Vec3 xform(const Affine& a, Vec3 p) {
    const float in[3] = {p.x, p.y, p.z};
    float out[3];
    a.apply(in, out);
    return {out[0], out[1], out[2]};
}

int outside_by(const Grid& g, Cell c) {
    const int v[3] = {c.x, c.y, c.z};
    int d = 0;
    for (int a = 0; a < 3; ++a) d = std::max({d, -v[a], v[a] - (g.size[a] - 1)});
    return d;
}

void bounds_of(const std::vector<Cell>& cells, Cell& lo, Cell& hi) {
    lo = hi = cells.front();
    for (const Cell& c : cells) {
        lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
        hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
    }
}

}  // namespace

void mark_cell_dirty(Piece& p, Cell c) {
    if (p.remesh_all) return;
    const Grid& g = p.grid;
    const int n[3] = {chunk_count(g, 0), chunk_count(g, 1), chunk_count(g, 2)};
    const int v[3] = {c.x, c.y, c.z};
    int k[3];
    for (int a = 0; a < 3; ++a) k[a] = v[a] / kChunk;
    auto add = [&](int x, int y, int z) {
        if (x < 0 || y < 0 || z < 0 || x >= n[0] || y >= n[1] || z >= n[2]) return;
        p.dirty_chunks.push_back(uint32_t(x + n[0] * (y + n[1] * z)));
    };
    add(k[0], k[1], k[2]);
    for (int a = 0; a < 3; ++a) {
        int m[3] = {k[0], k[1], k[2]};
        if (v[a] % kChunk == 0) {
            m[a] = k[a] - 1;
            add(m[0], m[1], m[2]);
        } else if (v[a] % kChunk == kChunk - 1) {
            m[a] = k[a] + 1;
            add(m[0], m[1], m[2]);
        }
    }
}

bool Doc::open_cub(const std::string& path, std::string& err) {
    std::string bytes;
    if (!read_bytes(path, bytes)) {
        err = "cannot read " + path;
        return false;
    }
    Piece p;
    if (!parse_grid(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), p.grid, err)) return false;
    p.name = file_name(path);
    p.path = path;
    p.editable = true;
    p.world.t[2] = p.grid.size[2] * 0.5f;   // stands on the ground
    p.inv = inverse(p.world);
    *this = Doc();
    pieces.push_back(std::move(p));
    set_active(0);
    return true;
}

void Doc::new_model(int sx, int sy, int sz) {
    Piece p;
    p.grid.resize(std::clamp(sx, 1, kMaxSize), std::clamp(sy, 1, kMaxSize), std::clamp(sz, 1, kMaxSize));
    p.name = "new model";
    p.editable = true;
    p.world.t[2] = p.grid.size[2] * 0.5f;
    p.inv = inverse(p.world);
    p.rev = 1;   // unsaved
    *this = Doc();
    pieces.push_back(std::move(p));
    set_active(0);
}

bool Doc::open_rig(const std::string& path, const std::string& models_dir, std::string& err) {
    std::string text;
    if (!read_bytes(path, text)) {
        err = "cannot read " + path;
        return false;
    }
    kanimator::RigLoad rl = kanimator::parse_rig(text, file_name(path));
    if (!rl.ok) {
        err = rl.errors.empty() ? "the rig is not valid" : rl.errors.front().message;
        if (!rl.errors.empty()) err = file_name(path) + ":" + std::to_string(rl.errors.front().line) + ": " + err;
        return false;
    }
    const std::string dir = models_dir.empty() ? folder_of(path) : models_dir;
    const kanimator::Solved solved = kanimator::solve_pose(rl.rig, kanimator::Pose{});
    Doc d;
    d.rig_mode = true;
    d.rig_path = path;
    for (size_t i = 0; i < rl.rig.nodes.size(); ++i) {
        const kanimator::Node& n = rl.rig.nodes[i];
        Piece p;
        p.name = n.name;
        p.world = solved.world[i];
        p.inv = inverse(p.world);
        if (n.model.kind == kanimator::Model::Kind::Named) {
            p.path = dir + "/" + n.model.name;
            std::string bytes, why;
            if (!read_bytes(p.path, bytes)) {
                d.notes.push_back(n.name + ": " + n.model.name + " is not in " + dir);
                continue;
            }
            if (!parse_grid(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), p.grid, why)) {
                d.notes.push_back(n.name + ": " + why);
                continue;
            }
            p.editable = true;
        } else if (n.model.kind == kanimator::Model::Kind::Box) {
            int s[3];
            for (int a = 0; a < 3; ++a) s[a] = std::clamp(int(std::lround(n.model.box[a])), 1, kMaxSize);
            p.grid.resize(s[0], s[1], s[2]);
            for (size_t c = 0; c < p.grid.cells(); ++c) p.grid.put(c, Rgb{128, 132, 140});
        } else {
            continue;
        }
        d.pieces.push_back(std::move(p));
    }
    if (d.pieces.empty()) {
        err = "the rig has no model that could be read";
        if (!d.notes.empty()) err += " (" + d.notes.front() + ")";
        return false;
    }
    int first = -1;
    for (size_t i = 0; i < d.pieces.size() && first < 0; ++i)
        if (d.pieces[i].editable) first = int(i);
    *this = std::move(d);
    if (first >= 0) set_active(first);
    return true;
}

bool Doc::set_active(int i) {
    if (in_stroke_ || i < 0 || i >= int(pieces.size()) || !pieces[i].editable) return false;
    if (active >= 0 && active < int(pieces.size()) && slice.on) pieces[active].remesh_all = true;   // it was drawn sliced
    active = i;
    slice = Slice();
    for (int a = 0; a < 3; ++a) slice.hi[a] = pieces[i].grid.size[a] - 1;
    return true;
}

// ----- strokes

void Doc::begin_stroke() {
    in_stroke_ = true;
    lock_axis_ = -1;
    for (int& s : last_shift_) s = 0;
    strokes_.assign(pieces.size(), Stroke());
    stroked_.assign(pieces.size(), 0);
}

void Doc::lock_plane(int axis, int value) {
    lock_axis_ = axis;
    lock_value_ = value;
}

int Doc::target_for(Vec3 world, int reach, Cell& cell) const {
    int best = -1, best_d = reach + 1;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const Piece& p = pieces[i];
        if (!p.editable) continue;
        const Cell c = p.grid.cell_at(xform(p.inv, world));
        const int d = outside_by(p.grid, c);
        if (int(i) == active && d <= reach) {
            cell = c;
            return active;
        }
        if (d < best_d) {
            best_d = d;
            best = int(i);
            cell = c;
        }
    }
    return best;
}

int Doc::apply(const std::vector<Cell>& cells_in, Op op, Rgb colour, bool mirror) {
    Piece* a = act();
    if (!in_stroke_ || !a || !a->editable || cells_in.empty()) return 0;
    std::vector<Cell> cells;
    cells.reserve(cells_in.size());
    for (const Cell& c : cells_in) {
        const int v[3] = {c.x, c.y, c.z};
        if (op != Op::Paint && lock_axis_ >= 0 && v[lock_axis_] != lock_value_) continue;
        cells.push_back(c);
    }
    if (cells.empty()) return 0;

    // Gather the work per piece: the active piece's cells, then the mirror images.
    std::vector<std::vector<Cell>> work(pieces.size());
    work[active] = cells;
    if (mirror) {
        const int reach = op == Op::Add ? 4 : 0;
        for (const Cell& c : cells) {
            Vec3 w = xform(a->world, a->grid.centre_of(c));
            w.x = -w.x;
            Cell m;
            const int t = target_for(w, reach, m);
            if (t >= 0) work[t].push_back(m);
        }
    }

    int changed = 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (work[i].empty()) continue;
        Piece& p = pieces[i];
        if (!stroked_[i]) {
            strokes_[i].begin(int(i), p.grid);
            stroked_[i] = 1;
        }
        if (op == Op::Add) {
            Cell lo, hi;
            bounds_of(work[i], lo, hi);
            if (!p.grid.inside(lo) || !p.grid.inside(hi)) {
                strokes_[i].before_resize(p.grid);
                int shift[3];
                if (grow_to_fit(p.grid, lo, hi, shift)) {
                    for (Cell& c : work[i]) {
                        c.x += shift[0];
                        c.y += shift[1];
                        c.z += shift[2];
                    }
                    p.remesh_all = true;
                    p.dirty_chunks.clear();
                    if (int(i) == active) {
                        for (int k = 0; k < 3; ++k) last_shift_[k] += shift[k];
                        if (lock_axis_ >= 0) lock_value_ += shift[lock_axis_];
                        for (int k = 0; k < 3; ++k) {
                            slice.lo[k] += shift[k];
                            slice.hi[k] += shift[k];
                        }
                        if (!slice.on)
                            for (int k = 0; k < 3; ++k) {
                                slice.lo[k] = 0;
                                slice.hi[k] = p.grid.size[k] - 1;
                            }
                    }
                }
            }
        }
        for (const Cell& c : work[i]) {
            if (int(i) == active && !slice.shows(c.x, c.y, c.z)) continue;   // hidden cells are not edited
            if (strokes_[i].apply(p.grid, c, op, colour)) {
                mark_cell_dirty(p, c);
                ++changed;
            }
        }
    }
    return changed;
}

void Doc::end_stroke(const std::string& what) {
    if (!in_stroke_) return;
    in_stroke_ = false;
    Step step;
    step.what = what;
    for (size_t i = 0; i < pieces.size() && i < stroked_.size(); ++i) {
        if (!stroked_[i]) continue;
        PieceStep ps;
        if (!strokes_[i].finish(ps)) continue;
        ps.rev_before = pieces[i].rev;
        ps.rev_after = next_rev_++;
        pieces[i].rev = ps.rev_after;
        step.pieces.push_back(std::move(ps));
    }
    strokes_.clear();
    stroked_.clear();
    if (!step.pieces.empty()) push(std::move(step));
}

void Doc::push(Step&& s) {
    undo_.push_back(std::move(s));
    if (undo_.size() > kUndoDepth) undo_.erase(undo_.begin());
    redo_.clear();
}

bool Doc::trim_active() {
    Piece* p = act();
    if (!p || in_stroke_) return false;
    PieceStep ps;
    ps.piece = active;
    ps.snapshot = true;
    ps.other = p->grid;
    int shift[3];
    if (!trim_even(p->grid, shift)) return false;
    ps.rev_before = p->rev;
    ps.rev_after = next_rev_++;
    p->rev = ps.rev_after;
    p->remesh_all = true;
    Step s;
    s.what = "trim";
    s.pieces.push_back(std::move(ps));
    push(std::move(s));
    set_active(active);
    return true;
}

namespace {
void after_step(Doc& d, PieceStep& ps, bool undo) {
    Piece& p = d.pieces[ps.piece];
    p.rev = undo ? ps.rev_before : ps.rev_after;
    if (ps.snapshot) {
        p.remesh_all = true;
        p.dirty_chunks.clear();
        if (ps.piece == d.active) {
            if (d.slice.on) d.slice.clamp_to(p.grid);
            else
                for (int k = 0; k < 3; ++k) {
                    d.slice.lo[k] = 0;
                    d.slice.hi[k] = p.grid.size[k] - 1;
                }
        }
    } else {
        for (const Change& c : ps.changes) mark_cell_dirty(p, p.grid.cell_of(c.index));
    }
}
}  // namespace

bool Doc::undo() {
    if (in_stroke_ || undo_.empty()) return false;
    Step s = std::move(undo_.back());
    undo_.pop_back();
    for (auto it = s.pieces.rbegin(); it != s.pieces.rend(); ++it) {
        undo_piece(pieces[it->piece].grid, *it);
        after_step(*this, *it, true);
    }
    redo_.push_back(std::move(s));
    return true;
}

bool Doc::redo() {
    if (in_stroke_ || redo_.empty()) return false;
    Step s = std::move(redo_.back());
    redo_.pop_back();
    for (PieceStep& ps : s.pieces) {
        redo_piece(pieces[ps.piece].grid, ps);
        after_step(*this, ps, false);
    }
    undo_.push_back(std::move(s));
    return true;
}

const std::string& Doc::undo_name() const {
    static const std::string none;
    return undo_.empty() ? none : undo_.back().what;
}

const std::string& Doc::redo_name() const {
    static const std::string none;
    return redo_.empty() ? none : redo_.back().what;
}

bool Doc::any_dirty() const {
    for (const Piece& p : pieces)
        if (p.editable && p.dirty()) return true;
    return false;
}

void Doc::mark_saved(int piece) {
    if (piece >= 0 && piece < int(pieces.size())) pieces[piece].saved_rev = pieces[piece].rev;
}

}  // namespace kvox
