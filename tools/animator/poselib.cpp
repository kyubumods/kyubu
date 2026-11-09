// The Animator's pose library (openspec add-ability-animation, task 6.8). See poselib.h.
#include "poselib.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "cliptext.h"
#include "json.hpp"

namespace kanimator {

namespace {

using json = nlohmann::json;
using anim::Channel;
using anim::Part;
using anim::kPartCount;

constexpr const char* kFormat = "kyubu-capture/1";

std::filesystem::path to_path(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string path_text(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool read_text(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// A number that may be `null` (the mod writes null for a value that is not finite). False when it is neither.
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

// `n` numbers of an array into `out` (the rest of `out` stays 0). A short array is an error: the root's pos has two numbers, the
// game reads no third.
bool read_vector(const json& v, int n, float out[3]) {
    if (!v.is_array() || static_cast<int>(v.size()) < n || v.size() > 3) return false;
    for (int i = 0; i < n; ++i)
        if (!read_number(v[i], out[i])) return false;
    return true;
}

// The number of floats the game reads of a part's `pos`: 3, the root's 2, the tail's 0.
int pos_floats(Part p) { return p == Part::Tail ? 0 : p == Part::Root ? 2 : 3; }

std::string where(const std::string& source, const std::string& field) { return source + ": " + field; }

}  // namespace

BakedLoad parse_baked_pose(const std::string& text, const std::string& source) {
    BakedLoad r;
    if (text.size() > kMaxPoseFileBytes) {
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
    const std::string kind = str_of("kind");
    if (kind != "bake") {
        // A rig, a capture or a bake's replay is a capture file of the same format: not a baked pose, and not wrong.
        if (kind == "rig" || kind == "capture" || kind == "bake-replay") {
            r.other_kind = true;
            r.error = where(source, "`kind` is `" + kind + "`, not `bake`");
        } else {
            r.error = where(source, "`kind` is missing or not one the mod writes");
        }
        return r;
    }
    r.pose.source = source;
    r.pose.build = str_of("build");
    auto fr = j.find("frames");
    if (fr == j.end() || !fr->is_array() || fr->empty()) {
        r.error = where(source, "`frames` is missing or empty");
        return r;
    }
    if (fr->size() > kMaxBakedFrames) {
        r.error = where(source, "`frames` has more draws than the limit");
        return r;
    }
    r.pose.frames.reserve(fr->size());
    for (size_t i = 0; i < fr->size(); ++i) {
        const json& f = (*fr)[i];
        const std::string at = "frames[" + std::to_string(i) + "]";
        if (!f.is_object()) {
            r.error = where(source, at + " is not an object");
            return r;
        }
        BakedFrame bf;
        auto t = f.find("t_ms");
        if (t == f.end() || !t->is_number() || !std::isfinite(t->get<double>()) || t->get<double>() < 0) {
            r.error = where(source, at + ".t_ms is missing or not a time of 0 or more");
            return r;
        }
        bf.t_ms = t->get<double>();
        if (!r.pose.frames.empty() && bf.t_ms < r.pose.frames.back().t_ms) {
            r.error = where(source, at + ".t_ms is earlier than the frame before it: the frames must be in time order");
            return r;
        }
        auto pid = f.find("pose_id");
        if (pid == f.end() || !pid->is_number_integer() || pid->get<long long>() < 0 || pid->get<long long>() > kMaxPoseId) {
            r.error = where(source, at + ".pose_id is missing or not 0 to " + std::to_string(kMaxPoseId));
            return r;
        }
        const int id = static_cast<int>(pid->get<long long>());
        if (r.pose.pose_id >= 0 && id != r.pose.pose_id) {
            r.error = where(source, at + ".pose_id differs from the first frame's: a bake is one pose");
            return r;
        }
        r.pose.pose_id = id;
        auto vec = f.find("vectors");
        if (vec == f.end() || !vec->is_object()) {
            r.error = where(source, at + ".vectors is missing");
            return r;
        }
        for (int p = 0; p < kPartCount; ++p) {
            const Part part = static_cast<Part>(p);
            const std::string name = anim::part_name(part);
            auto pv = vec->find(name);
            if (pv == vec->end() || !pv->is_object()) {
                r.error = where(source, at + ".vectors." + name + " is missing");
                return r;
            }
            anim::PartPose& pp = bf.pose.parts[p];
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
        r.pose.frames.push_back(bf);
    }
    r.ok = true;
    return r;
}

anim::Pose sample_baked(const BakedPose& p, double t_ms, bool* past_end) {
    if (past_end) *past_end = false;
    if (p.frames.empty()) return anim::Pose();
    if (past_end && t_ms > p.frames.back().t_ms + 0.5) *past_end = true;
    if (!(t_ms > p.frames.front().t_ms)) return p.frames.front().pose;
    if (t_ms >= p.frames.back().t_ms) return p.frames.back().pose;
    // The first frame later than t_ms; the one before it is at or before.
    auto hi = std::upper_bound(p.frames.begin(), p.frames.end(), t_ms,
                               [](double t, const BakedFrame& f) { return t < f.t_ms; });
    auto lo = hi - 1;
    const double span = hi->t_ms - lo->t_ms;
    const float f = span > 0 ? static_cast<float>((t_ms - lo->t_ms) / span) : 1.0f;
    anim::Pose out;
    for (int i = 0; i < kPartCount; ++i)
        for (int k = 0; k < 3; ++k) {
            out.parts[i].pos[k] = lo->pose.parts[i].pos[k] + (hi->pose.parts[i].pos[k] - lo->pose.parts[i].pos[k]) * f;
            out.parts[i].rot[k] = lo->pose.parts[i].rot[k] + (hi->pose.parts[i].rot[k] - lo->pose.parts[i].rot[k]) * f;
        }
    return out;
}

// ---- the library

void PoseLibrary::clear() {
    poses_.clear();
    folder_.clear();
}

void PoseLibrary::add(BakedPose p) {
    auto it = std::lower_bound(poses_.begin(), poses_.end(), p.pose_id,
                               [](const BakedPose& a, int id) { return a.pose_id < id; });
    if (it != poses_.end() && it->pose_id == p.pose_id) *it = std::move(p);
    else poses_.insert(it, std::move(p));
}

const BakedPose* PoseLibrary::find(int pose_id) const {
    auto it = std::lower_bound(poses_.begin(), poses_.end(), pose_id,
                               [](const BakedPose& a, int id) { return a.pose_id < id; });
    return it != poses_.end() && it->pose_id == pose_id ? &*it : nullptr;
}

std::vector<int> PoseLibrary::ids() const {
    std::vector<int> v;
    v.reserve(poses_.size());
    for (const BakedPose& p : poses_) v.push_back(p.pose_id);
    return v;
}

PoseLibrary::Scan PoseLibrary::scan(const std::string& folder) {
    Scan result;
    clear();
    folder_ = folder;
    if (folder.empty()) {
        result.problems.push_back("no pose library folder is set");
        return result;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = to_path(folder);
    if (!fs::is_directory(root, ec)) {
        result.problems.push_back("the pose library folder does not exist: " + folder);
        return result;
    }
    // The folders to read: the chosen one and its `captures`. Later names last, so a later stamp wins.
    struct Found {
        std::string name;    // the folder's name: the order of stamps
        fs::path file;
    };
    std::vector<Found> found;
    for (const fs::path& base : {root, root / "captures"}) {
        if (!fs::is_directory(base, ec)) continue;
        for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            const fs::path file = it->path() / "pose.json";
            if (!fs::is_regular_file(file, e2)) continue;
            found.push_back({path_text(it->path().filename()), file});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.name < b.name; });
    for (const Found& f : found) {
        std::string text;
        const std::string label = path_text(f.file);
        if (!read_text(f.file, text)) {
            result.problems.push_back(label + ": cannot be read");
            continue;
        }
        BakedLoad l = parse_baked_pose(text, label);
        if (l.other_kind) continue;
        if (!l.ok) {
            result.problems.push_back(l.error);
            continue;
        }
        if (find(l.pose.pose_id)) ++result.replaced;
        add(std::move(l.pose));
    }
    result.loaded = static_cast<int>(poses_.size());
    return result;
}

// ---- settings

Settings parse_settings(const std::string& text) {
    Settings s;
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return s;
    auto it = j.find("library_folder");
    if (it != j.end() && it->is_string()) s.library_folder = it->get<std::string>();
    it = j.find("game_folder");
    if (it != j.end() && it->is_string()) s.game_folder = it->get<std::string>();
    it = j.find("sound_folder");
    if (it != j.end() && it->is_string()) s.sound_folder = it->get<std::string>();
    it = j.find("sound_volume");
    if (it != j.end() && it->is_number()) {
        const double v = it->get<double>();
        if (v >= 0 && v <= 2) s.sound_volume = static_cast<float>(v);
    }
    return s;
}

std::string settings_text(const Settings& s) {
    json j = json::object();
    j["library_folder"] = s.library_folder;
    j["game_folder"] = s.game_folder;
    j["sound_folder"] = s.sound_folder;
    j["sound_volume"] = s.sound_volume;
    return j.dump(2) + "\n";
}

std::string settings_file_path() {
    const char* base = std::getenv("APPDATA");
    if (!base || !*base) return std::string();
    return std::string(base) + "\\kyubu-animator\\settings.json";
}

// ---- the base pose

BaseResult base_pose_for(const PoseLibrary& lib, const anim::Clip& clip, float elapsed_ms) {
    BaseResult r;
    if (!clip.borrow.present) return r;
    r.pose_id = static_cast<int>(clip.borrow.mode);
    const BakedPose* p = lib.find(r.pose_id);
    if (!p) {
        r.kind = BaseResult::Kind::Missing;
        return r;
    }
    r.kind = BaseResult::Kind::Borrowed;
    r.pose_ms = anim::borrowed_mode_time(clip, elapsed_ms);
    r.pose = sample_baked(*p, r.pose_ms, &r.past_end);
    return r;
}

std::string base_label(const BaseResult& r) {
    switch (r.kind) {
    case BaseResult::Kind::Neutral:
        return std::string();
    case BaseResult::Kind::Missing:
        return "pose " + std::to_string(r.pose_id) + " is not in the pose library: the tracks are drawn over the neutral pose";
    case BaseResult::Kind::Borrowed: {
        char b[64];
        std::snprintf(b, sizeof b, "%.0f", r.pose_ms);
        std::string s = "borrowed pose " + std::to_string(r.pose_id) + " at " + b + " ms";
        if (r.past_end) s += " (past its end: the game draws pose 0 here)";
        return s;
    }
    }
    return std::string();
}

// ---- the edits

namespace {

constexpr int kChannels = 2;   // pos, rot

// The first `set` track of a part and channel, or -1.
int find_set_track(const anim::Clip& c, Part p, Channel ch) {
    for (size_t i = 0; i < c.tracks.size(); ++i)
        if (c.tracks[i].part == p && c.tracks[i].channel == ch && c.tracks[i].mode == anim::TrackMode::Set) return static_cast<int>(i);
    return -1;
}

bool has_add_track(const anim::Clip& c, Part p, Channel ch) {
    for (const anim::Track& t : c.tracks)
        if (t.part == p && t.channel == ch && t.mode == anim::TrackMode::Add) return true;
    return false;
}

// Whether a part has the channel at all: the tail has no position.
bool part_has_channel(Part p, Channel ch) { return !(p == Part::Tail && ch == Channel::Pos); }

anim::Key make_key(float t, const float v[3]) {
    anim::Key k;
    k.t = std::clamp(t, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) k.v[i] = std::isfinite(v[i]) ? v[i] : 0.0f;
    k.ease = anim::Ease::Linear;
    return k;
}

const float* vector_of(const anim::Pose& p, Part part, Channel ch) {
    return ch == Channel::Pos ? p.parts[static_cast<int>(part)].pos : p.parts[static_cast<int>(part)].rot;
}

}  // namespace

bool capture_at(const std::string& text, const anim::Clip& clip, const BakedPose& pose, double pose_ms, float clip_u, float duration_ms,
                std::string& out, std::string& err, CaptureResult* result) {
    CaptureResult res;
    if (pose.frames.empty()) {
        err = "the pose has no frames";
        return false;
    }
    if (!(duration_ms > 0)) {
        err = "the clip has no length yet: give it a duration, or type the ability's length";
        return false;
    }
    const anim::Pose at = sample_baked(pose, pose_ms);
    const float u = std::clamp(clip_u, 0.0f, 1.0f);
    std::string cur = text, next;
    for (int p = 0; p < kPartCount; ++p) {
        const Part part = static_cast<Part>(p);
        for (int c = 0; c < kChannels; ++c) {
            const Channel ch = c == 0 ? Channel::Pos : Channel::Rot;
            if (!part_has_channel(part, ch)) continue;
            const anim::Key key = make_key(u, vector_of(at, part, ch));
            if (has_add_track(clip, part, ch)) ++res.over_add_tracks;
            const int ti = find_set_track(clip, part, ch);
            if (ti < 0) {
                if (!append_track(cur, part, ch, anim::TrackMode::Set, {key}, next, err)) return false;
                ++res.tracks_made;
            } else {
                // A key at this time is replaced, so a second capture at the same time does not stack two keys.
                int same = -1;
                const std::vector<anim::Key>& keys = clip.tracks[ti].keys;
                for (size_t k = 0; k < keys.size(); ++k)
                    if (std::fabs((keys[k].t - key.t) * duration_ms) <= kKeyEpsilonMs) {
                        same = static_cast<int>(k);
                        break;
                    }
                if (same >= 0) {
                    anim::Key with = key;
                    with.ease = keys[same].ease;   // the segment's shape is the author's
                    if (!replace_key(cur, ti, same, with, next, err)) return false;
                    ++res.keys_replaced;
                } else {
                    if (!insert_key(cur, ti, key, next, err)) return false;
                    ++res.keys_added;
                }
            }
            cur.swap(next);
        }
    }
    out = cur;
    if (result) *result = res;
    return true;
}

bool bake_to_keys(const std::string& text, const anim::Clip& clip, const BakedPose& pose, float duration_ms, std::string& out,
                  std::string& err, BakeResult* result) {
    BakeResult res;
    if (pose.frames.empty()) {
        err = "the pose has no frames";
        return false;
    }
    if (!(duration_ms > 0)) {
        err = "the clip has no length yet: give it a duration, or type the ability's length";
        return false;
    }
    // The draws that fit in the clip, thinned: the first, each later one far enough after the last kept, and the last of them.
    std::vector<size_t> fit;
    for (size_t i = 0; i < pose.frames.size(); ++i)
        if (pose.frames[i].t_ms <= double(duration_ms) + 0.5) fit.push_back(i);
    res.dropped = static_cast<int>(pose.frames.size() - fit.size());
    std::vector<size_t> kept;
    for (size_t k = 0; k < fit.size(); ++k) {
        const bool last = k + 1 == fit.size();
        if (kept.empty() ||
            pose.frames[fit[k]].t_ms - pose.frames[kept.back()].t_ms >= kBakeKeyGapMs - 0.5 ||
            (last && pose.frames[fit[k]].t_ms > pose.frames[kept.back()].t_ms))
            kept.push_back(fit[k]);
    }
    if (kept.size() < 2) {
        err = "fewer than two draws of the pose fit in the clip's " + fmt_value(duration_ms) + " ms";
        return false;
    }
    res.keys = static_cast<int>(kept.size());

    std::string cur = text, next;
    // The `set` tracks the bake replaces go first, from the last to the first so the indexes stay.
    std::vector<int> drop;
    for (int p = 0; p < kPartCount; ++p)
        for (int c = 0; c < kChannels; ++c) {
            const Part part = static_cast<Part>(p);
            const Channel ch = c == 0 ? Channel::Pos : Channel::Rot;
            if (!part_has_channel(part, ch)) continue;
            if (has_add_track(clip, part, ch)) ++res.over_add_tracks;
            for (size_t i = 0; i < clip.tracks.size(); ++i)
                if (clip.tracks[i].part == part && clip.tracks[i].channel == ch && clip.tracks[i].mode == anim::TrackMode::Set)
                    drop.push_back(static_cast<int>(i));
        }
    std::sort(drop.begin(), drop.end());
    drop.erase(std::unique(drop.begin(), drop.end()), drop.end());
    for (auto it = drop.rbegin(); it != drop.rend(); ++it) {
        if (!remove_track(cur, *it, next, err)) return false;
        cur.swap(next);
        ++res.tracks_replaced;
    }
    for (int p = 0; p < kPartCount; ++p)
        for (int c = 0; c < kChannels; ++c) {
            const Part part = static_cast<Part>(p);
            const Channel ch = c == 0 ? Channel::Pos : Channel::Rot;
            if (!part_has_channel(part, ch)) continue;
            std::vector<anim::Key> keys;
            keys.reserve(kept.size());
            for (size_t k : kept) {
                const BakedFrame& f = pose.frames[k];
                keys.push_back(make_key(static_cast<float>(f.t_ms / double(duration_ms)), vector_of(f.pose, part, ch)));
            }
            if (!append_track(cur, part, ch, anim::TrackMode::Set, keys, next, err)) return false;
            cur.swap(next);
            ++res.tracks_made;
        }
    out = cur;
    if (result) *result = res;
    return true;
}

}  // namespace kanimator
