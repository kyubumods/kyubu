// The Animator's key inspector and curve view: see keyinspector.h.
#include "keyinspector.h"

#include <algorithm>
#include <cmath>

#include "animation_load.h"
#include "cliptext.h"

namespace kanimator {

// ---------------------------------------------------------------------------------------------------------------------------------
// Mirror L/R

bool mirror_partner(anim::Part part, anim::Part& other) {
    if (part == anim::Part::HandR) {
        other = anim::Part::HandL;
        return true;
    }
    if (part == anim::Part::HandL) {
        other = anim::Part::HandR;
        return true;
    }
    return false;
}

bool mirror_flips_axis(anim::Channel channel, int axis) {
    if (axis < 0 || axis > 2) return false;
    return channel == anim::Channel::Pos ? axis == 0 : axis != 0;
}

void mirror_vec(anim::Channel channel, const float in[3], float out[3]) {
    const float v[3] = {in[0], in[1], in[2]};
    for (int a = 0; a < 3; ++a) {
        // A flipped zero is written as 0, not -0, so the text of a mirrored track has no `-0`.
        out[a] = mirror_flips_axis(channel, a) && v[a] != 0 ? -v[a] : v[a];
    }
}

KeyEditResult mirror_part(const std::string& text, anim::Part from) {
    anim::Part to;
    if (!mirror_partner(from, to)) return fail_edit(text, std::string(anim::part_name(from)) + " has no left or right pair to mirror");
    anim::ClipLoad cl = anim::parse_clip(text, "clip");
    if (!cl.ok) return fail_edit(text, "the clip has errors; fix them before mirroring");
    const anim::Clip& clip = cl.clip;

    // A generator (osc, spring, expr) is numbers and an expression, not keys: the mirror works on keys, and a track that is a
    // generator on either side would be lost or left behind in silence.
    for (const anim::Track& t : clip.tracks)
        if ((t.part == from || t.part == to) && t.source != anim::Source::Keys)
            return fail_edit(text, std::string(anim::part_name(t.part)) + " has a generator track (" + anim::source_name(t.source) +
                                       "): mirror works on keys; bake it to keys first (kanim bake) or write the other side by hand");
    std::vector<size_t> sources;
    bool channel_used[2] = {false, false};
    for (size_t i = 0; i < clip.tracks.size(); ++i)
        if (clip.tracks[i].part == from) {
            sources.push_back(i);
            channel_used[static_cast<int>(clip.tracks[i].channel)] = true;
        }
    if (sources.empty()) return fail_edit(text, std::string(anim::part_name(from)) + " has no track to mirror");

    std::string cur = text, err;
    int replaced = 0;
    for (size_t i = clip.tracks.size(); i-- > 0;) {
        const anim::Track& tr = clip.tracks[i];
        if (tr.part != to || !channel_used[static_cast<int>(tr.channel)]) continue;
        std::string next;
        if (!remove_track(cur, static_cast<int>(i), next, err)) return fail_edit(text, err);
        cur = std::move(next);
        replaced += static_cast<int>(tr.keys.size());
    }
    for (size_t si : sources) {
        const anim::Track& src = clip.tracks[si];
        std::vector<anim::Key> keys = src.keys;
        for (anim::Key& k : keys) {
            float m[3];
            mirror_vec(src.channel, k.v, m);
            for (int a = 0; a < 3; ++a) k.v[a] = m[a];
        }
        std::string next;
        if (!append_track(cur, to, src.channel, src.mode, keys, next, err)) return fail_edit(text, err);
        cur = std::move(next);
    }

    // The new tracks are the last ones of the file, in the order of the sources.
    KeyEditResult res;
    anim::ClipLoad after = anim::parse_clip(cur, "clip");
    if (!after.ok) return fail_edit(text, "the mirror made an invalid clip; nothing was changed");
    const size_t first = after.clip.tracks.size() - sources.size();
    for (size_t t = first; t < after.clip.tracks.size(); ++t)
        for (size_t k = 0; k < after.clip.tracks[t].keys.size(); ++k) select_add(res.selection, {static_cast<int>(t), static_cast<int>(k)});
    res.made_tracks = static_cast<int>(sources.size());
    res.replaced = replaced;
    return finish_edit(text, std::move(cur), std::move(res));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The key

float value_limit(anim::Channel channel) { return channel == anim::Channel::Pos ? anim::kMaxPos : anim::kMaxRotDeg; }

namespace {

bool key_of(const anim::Clip& clip, KeyRef k) {
    return k.track >= 0 && static_cast<size_t>(k.track) < clip.tracks.size() && k.key >= 0 &&
           static_cast<size_t>(k.key) < clip.tracks[k.track].keys.size();
}

// Replaces the keys of `sel` with `with(key, track)`, one at a time: a key keeps its place, so no index changes.
template <class F>
KeyEditResult rewrite_keys(const std::string& text, const Selection& sel, F with) {
    if (sel.empty()) return fail_edit(text, "no key is selected");
    anim::ClipLoad cl = anim::parse_clip(text, "clip");
    if (!cl.ok) return fail_edit(text, "the clip has errors; fix them before editing a key");
    std::string cur = text, err;
    for (const KeyRef& r : sel) {
        if (!key_of(cl.clip, r)) return fail_edit(text, "a selected key is no longer in the clip");
        const anim::Track& tr = cl.clip.tracks[r.track];
        anim::Key k = tr.keys[r.key];
        with(k, tr);
        std::string next;
        if (!replace_key(cur, r.track, r.key, k, next, err)) return fail_edit(text, err);
        cur = std::move(next);
    }
    KeyEditResult res;
    res.selection = sel;
    return finish_edit(text, std::move(cur), std::move(res));
}

}  // namespace

KeyEditResult set_key_vec(const std::string& text, KeyRef key, const float v[3]) {
    for (int a = 0; a < 3; ++a)
        if (!std::isfinite(v[a])) return fail_edit(text, "a value must be a number");
    const float nv[3] = {v[0], v[1], v[2]};
    return rewrite_keys(text, Selection{key}, [&](anim::Key& k, const anim::Track&) {
        for (int a = 0; a < 3; ++a) k.v[a] = nv[a];
    });
}

KeyEditResult set_keys_ease(const std::string& text, const Selection& sel, anim::Ease ease) {
    return rewrite_keys(text, sel, [&](anim::Key& k, const anim::Track&) { k.ease = ease; });
}

KeyEditResult reset_keys(const std::string& text, const Selection& sel, const anim::Pose& base) {
    return rewrite_keys(text, sel, [&](anim::Key& k, const anim::Track& tr) {
        const float* neutral = nullptr;
        if (tr.mode == anim::TrackMode::Set) {
            const anim::PartPose& pp = base.parts[static_cast<int>(tr.part)];
            neutral = tr.channel == anim::Channel::Pos ? pp.pos : pp.rot;
        }
        for (int a = 0; a < 3; ++a) k.v[a] = neutral ? neutral[a] : 0.0f;
    });
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The curve

void curve_samples(const anim::Track& track, int axis, int n, std::vector<float>& out, float duration_ms,
                   const anim::Signals& signals) {
    out.clear();
    if (n < 1 || axis < 0 || axis > 2) return;
    out.reserve(static_cast<size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) {
        float v[3];
        anim::sample_track(track, static_cast<float>(i) / static_cast<float>(n), duration_ms, signals, v);
        out.push_back(v[axis]);
    }
}

CurveRange curve_range(const anim::Track& track, float duration_ms, const anim::Signals& signals) {
    float lo = 0, hi = 0;
    bool any = false;
    for (const anim::Key& k : track.keys)
        for (int a = 0; a < 3; ++a) {
            lo = any ? std::min(lo, k.v[a]) : k.v[a];
            hi = any ? std::max(hi, k.v[a]) : k.v[a];
            any = true;
        }
    if (track.source != anim::Source::Keys) {   // a generator has no keys to bound the curve: read it
        for (int i = 0; i <= 200; ++i) {
            float v[3];
            anim::sample_track(track, static_cast<float>(i) / 200.0f, duration_ms, signals, v);
            for (int a = 0; a < 3; ++a) {
                lo = any ? std::min(lo, v[a]) : v[a];
                hi = any ? std::max(hi, v[a]) : v[a];
                any = true;
            }
        }
    }
    float span = hi - lo;
    if (span < 1.0f) {
        const float mid = (lo + hi) * 0.5f;
        lo = mid - 0.5f;
        hi = mid + 0.5f;
        span = 1.0f;
    }
    const float pad = span * 0.1f;
    return {lo - pad, hi + pad};
}

}  // namespace kanimator
