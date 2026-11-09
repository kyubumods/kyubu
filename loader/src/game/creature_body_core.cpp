#include "creature_body_core.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace kyubu::game::body {

namespace {

bool good_clip_name(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    return true;
}

std::string first_error(const std::vector<anim::ClipError>& errs) {
    return errs.empty() ? std::string("unknown error") : anim::format_error(errs[0]);
}

}  // namespace

int BodyDef::clip_index(const std::string& n) const {
    for (size_t i = 0; i < clips.size(); ++i)
        if (clips[i].name == n) return static_cast<int>(i);
    return -1;
}

int BodyDef::piece_index(const std::string& n) const {
    for (size_t i = 0; i < pieces.size(); ++i)
        if (pieces[i].name == n) return static_cast<int>(i);
    return -1;
}

DefineResult define(const std::string& name, const std::string& rig_text, const std::vector<ClipSpec>& clips, const std::vector<PieceSpec>& pieces) {
    DefineResult r;
    auto def = std::make_shared<BodyDef>();
    def->name = name;
    kanimator::RigLoad rl = kanimator::parse_rig(rig_text, name + ".rig.toml");
    if (!rl.ok) {
        r.error = "the rig is refused: " + first_error(rl.errors);
        return r;
    }
    def->rig = std::move(rl.rig);
    if (clips.size() > static_cast<size_t>(kMaxClips)) {
        r.error = "too many clips (" + std::to_string(clips.size()) + ", at most " + std::to_string(kMaxClips) + ")";
        return r;
    }
    // pieces: every node with a model needs a spec, every spec a node with a model
    std::set<std::string> named;
    for (const PieceSpec& ps : pieces) {
        int n = def->rig.find(ps.node);
        if (n < 0) {
            r.error = "piece '" + ps.node + "' is not a node of the rig";
            return r;
        }
        if (def->rig.nodes[n].model.kind == kanimator::Model::Kind::None) {
            r.error = "piece '" + ps.node + "' is a node without a model";
            return r;
        }
        if (!named.insert(ps.node).second) {
            r.error = "piece '" + ps.node + "' is given twice";
            return r;
        }
    }
    for (size_t i = 0; i < def->rig.nodes.size(); ++i) {
        const kanimator::Node& nd = def->rig.nodes[i];
        if (nd.model.kind == kanimator::Model::Kind::None) continue;
        if (!named.count(nd.name)) {
            r.error = "piece '" + nd.name + "' has no model: no slot was registered for it";
            return r;
        }
    }
    for (int idx : def->rig.order) {
        const kanimator::Node& nd = def->rig.nodes[idx];
        if (nd.model.kind == kanimator::Model::Kind::None) continue;
        for (const PieceSpec& ps : pieces)
            if (ps.node == nd.name) {
                BodyDef::Piece p;
                p.node = idx;
                p.name = nd.name;
                p.slot = ps.slot;
                for (int k = 0; k < 3; ++k) p.size[k] = ps.size[k];
                def->pieces.push_back(p);
            }
    }
    if (def->pieces.size() > static_cast<size_t>(kMaxPieces)) {
        r.error = "too many pieces";
        return r;
    }
    const uint32_t used = def->rig.parts_used();
    std::set<std::string> seen;
    for (const ClipSpec& cs : clips) {
        if (!good_clip_name(cs.name)) {
            r.error = "clip name '" + cs.name + "' is not 1 to 64 of a-z, 0-9, '_', '-' and '.'";
            return r;
        }
        if (!seen.insert(cs.name).second) {
            r.error = "clip '" + cs.name + "' is given twice";
            return r;
        }
        anim::ClipLoad cl = anim::parse_clip(cs.text, cs.name + ".anim.toml");
        if (!cl.ok) {
            r.error = "clip '" + cs.name + "' is refused: " + first_error(cl.errors);
            return r;
        }
        BodyDef::ClipDef cd;
        cd.name = cs.name;
        cd.duration_ms = anim::resolve_duration(cl.clip, 0.0f);
        if (!(cd.duration_ms > 0)) {
            r.error = "clip '" + cs.name + "' has no duration of its own (duration = \"ability\" is for abilities)";
            return r;
        }
        for (const anim::Track& tr : cl.clip.tracks) {
            const int pi = static_cast<int>(tr.part);
            if (!(used & (1u << pi))) {
                r.error = "clip '" + cs.name + "' writes the pose value '" + anim::part_name(tr.part) + "' that no node of the rig reads";
                return r;
            }
        }
        cd.clip = std::move(cl.clip);
        def->clips.push_back(std::move(cd));
    }
    r.def = std::move(def);
    return r;
}

anim::Pose Instance::sample(int clip, float t_ms, bool loop) const {
    anim::Pose p;
    if (!def || clip < 0 || clip >= static_cast<int>(def->clips.size())) return p;
    const BodyDef::ClipDef& cd = def->clips[clip];
    const float d = cd.duration_ms;
    float e = t_ms < 0 ? 0 : t_ms;
    if (loop) e = std::fmod(e, d);
    else e = std::min(e, d - 0.001f);
    anim::Timing tm;
    tm.elapsed_ms = e;
    tm.duration_ms = d;
    tm.run_ms = 0;
    anim::apply_clip(cd.clip, tm, p);
    return p;
}

bool Instance::play(int clip, float start_ms, bool loop, float fade_ms) {
    if (!def || clip < 0 || clip >= static_cast<int>(def->clips.size())) return false;
    if (cur_ >= 0 && fade_ms > 0) {
        prev_ = cur_;
        prev_t_ = cur_t_;
        prev_loop_ = cur_loop_;
        fade_ms_ = fade_ms;
        fade_left_ = fade_ms;
    } else {
        prev_ = -1;
        fade_ms_ = fade_left_ = 0;
    }
    cur_ = clip;
    cur_t_ = start_ms < 0 ? 0 : start_ms;
    cur_loop_ = loop;
    return true;
}

void Instance::advance(float dt_ms, std::vector<MarkerHit>* markers) {
    if (cur_ < 0 || !(dt_ms > 0)) return;
    const BodyDef::ClipDef& cd = def->clips[cur_];
    const float from = cur_t_, to = cur_t_ + dt_ms;
    const double dur = cd.duration_ms;
    if (markers && !cd.clip.markers.empty()) {
        std::vector<int> order(cd.clip.markers.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return cd.clip.markers[a].t < cd.clip.markers[b].t; });
        long long first = 0, last = 0;
        if (cur_loop_) {
            first = static_cast<long long>(std::floor(double(from) / dur));
            last = static_cast<long long>(std::floor(double(to) / dur));
            if (last - first > 64) last = first + 64;
        }
        for (long long lap = first; lap <= last; ++lap)
            for (int i : order) {
                const double at = (double(lap) + cd.clip.markers[i].t) * dur;
                if (at > from && at <= to) markers->push_back({cur_, cd.clip.markers[i].name});
            }
    }
    cur_t_ = to;
    if (cur_loop_) {
        if (cur_t_ >= cd.duration_ms) cur_t_ = static_cast<float>(std::fmod(double(cur_t_), dur));
    } else if (cur_t_ > cd.duration_ms) {
        cur_t_ = cd.duration_ms;
    }
    if (fade_left_ > 0) {
        fade_left_ -= dt_ms;
        prev_t_ += dt_ms;
        if (fade_left_ <= 0) {
            fade_left_ = 0;
            prev_ = -1;
        }
    }
}

void Instance::set_offset(anim::Part part, anim::Channel ch, const float v[3], int group) {
    const int p = static_cast<int>(part), c = ch == anim::Channel::Pos ? 0 : 1;
    if (p < 0 || p >= anim::kPartCount) return;
    for (int k = 0; k < 3; ++k) off_[p][c][k] = v[k];
    off_set_[p][c] = true;
    off_group_[p][c] = static_cast<int8_t>(std::clamp(group, 0, kMaxGroups - 1));
}

void Instance::clear_offsets() {
    for (int p = 0; p < anim::kPartCount; ++p)
        for (int c = 0; c < 2; ++c) {
            off_set_[p][c] = false;
            for (int k = 0; k < 3; ++k) off_[p][c][k] = 0;
        }
}

void Instance::set_group_weight(int group, float weight) {
    if (group < 0 || group >= kMaxGroups) return;
    group_w_[group] = std::clamp(weight, 0.0f, 1.0f);
}

anim::Pose Instance::pose_of(int clip, float t_ms, bool loop) const { return sample(clip, t_ms, loop); }

anim::Pose Instance::pose() const {
    anim::Pose out = sample(cur_, cur_t_, cur_loop_);
    if (prev_ >= 0 && fade_left_ > 0 && fade_ms_ > 0) {
        const anim::Pose b = sample(prev_, prev_t_, prev_loop_);
        const float f = 1.0f - fade_left_ / fade_ms_;   // 0 = still the old clip, 1 = the new one
        for (int p = 0; p < anim::kPartCount; ++p)
            for (int k = 0; k < 3; ++k) {
                out.parts[p].pos[k] = b.parts[p].pos[k] + (out.parts[p].pos[k] - b.parts[p].pos[k]) * f;
                out.parts[p].rot[k] = b.parts[p].rot[k] + (out.parts[p].rot[k] - b.parts[p].rot[k]) * f;
            }
    }
    for (int p = 0; p < anim::kPartCount; ++p)
        for (int c = 0; c < 2; ++c) {
            if (!off_set_[p][c]) continue;
            const float w = group_w_[off_group_[p][c]];
            float* dst = c == 0 ? out.parts[p].pos : out.parts[p].rot;
            for (int k = 0; k < 3; ++k) dst[k] += off_[p][c][k] * w;
        }
    return out;
}

void piece_point(const BodyDef& def, const kanimator::Solved& solved, int piece, const float p[3], const float pos[3], float yaw, float scale, float out[3]) {
    const BodyDef::Piece& pc = def.pieces[piece];
    const float local[3] = {p[0] - 0.5f * pc.size[0], p[1] - 0.5f * pc.size[1], p[2] - 0.5f * pc.size[2]};
    float w[3];
    solved.world[pc.node].apply(local, w);   // voxels, in the rig's frame
    const float c = std::cos(yaw), s = std::sin(yaw);
    out[0] = pos[0] + scale * (c * w[0] - s * w[1]);
    out[1] = pos[1] + scale * (s * w[0] + c * w[1]);
    out[2] = pos[2] + scale * w[2];
}

void piece_matrix(const BodyDef& def, const kanimator::Solved& solved, int piece, const float pos[3], float yaw, float scale, float out[16]) {
    const BodyDef::Piece& pc = def.pieces[piece];
    // R = Rz(yaw) * scale * node.r; t = the point of the model's corner (0,0,0) in the world
    const kanimator::Affine& a = solved.world[pc.node];
    const float c = std::cos(yaw), s = std::sin(yaw);
    float r[3][3];
    for (int j = 0; j < 3; ++j) {
        r[0][j] = scale * (c * a.r[0][j] - s * a.r[1][j]);
        r[1][j] = scale * (s * a.r[0][j] + c * a.r[1][j]);
        r[2][j] = scale * a.r[2][j];
    }
    const float zero[3] = {0, 0, 0};
    float t[3];
    piece_point(def, solved, piece, zero, pos, yaw, scale, t);
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row) out[col * 4 + row] = r[row][col];
    out[3] = out[7] = out[11] = 0;
    out[12] = t[0];
    out[13] = t[1];
    out[14] = t[2];
    out[15] = 1;
}

}  // namespace kyubu::game::body
