// The Animator's timeline: see timeline.h.
#include "timeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "animation_load.h"

namespace kanimator {

// ---------------------------------------------------------------------------------------------------------------------------------
// Rows and the selection

void select_add(Selection& sel, KeyRef k) {
    auto it = std::lower_bound(sel.begin(), sel.end(), k);
    if (it == sel.end() || !(*it == k)) sel.insert(it, k);
}

void select_toggle(Selection& sel, KeyRef k) {
    auto it = std::lower_bound(sel.begin(), sel.end(), k);
    if (it != sel.end() && *it == k) sel.erase(it);
    else sel.insert(it, k);
}

bool selected(const Selection& sel, KeyRef k) { return std::binary_search(sel.begin(), sel.end(), k); }

std::vector<TimelineRow> timeline_rows(const anim::Clip& clip) {
    std::vector<TimelineRow> rows;
    for (int p = 0; p < anim::kPartCount; ++p) {
        for (anim::Channel ch : {anim::Channel::Pos, anim::Channel::Rot}) {
            TimelineRow row;
            row.part = static_cast<anim::Part>(p);
            row.channel = ch;
            for (size_t t = 0; t < clip.tracks.size(); ++t) {
                const anim::Track& tr = clip.tracks[t];
                if (tr.part != row.part || tr.channel != ch) continue;
                row.edit_track = static_cast<int>(t);
                for (size_t k = 0; k < tr.keys.size(); ++k) row.keys.push_back({static_cast<int>(t), static_cast<int>(k)});
            }
            if (row.edit_track < 0) continue;
            std::stable_sort(row.keys.begin(), row.keys.end(), [&](const KeyRef& a, const KeyRef& b) {
                return clip.tracks[a.track].keys[a.key].t < clip.tracks[b.track].keys[b.key].t;
            });
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

std::string row_label(const TimelineRow& row) {
    return std::string(anim::part_name(row.part)) + "." + anim::channel_name(row.channel);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The time axis

float snap_ms(float t_ms, float step_ms, float duration_ms) {
    const float end = std::max(duration_ms, 0.0f);
    float t = std::min(end, std::max(0.0f, t_ms));
    if (!(step_ms > 0)) return t;
    const float on_grid = std::min(end, std::round(t / step_ms) * step_ms);
    // The end of the clip is a place to snap to, though the grid may not reach it.
    return std::fabs(end - t) < std::fabs(on_grid - t) ? end : on_grid;
}

float TimelineView::px_per_ms() const { return length_ms > 0 ? width * zoom / length_ms : 0.0f; }
float TimelineView::x_of(float t_ms) const { return x + (t_ms - start_ms) * px_per_ms(); }
float TimelineView::ms_at(float px) const {
    const float s = px_per_ms();
    return s > 0 ? start_ms + (px - x) / s : 0.0f;
}
float TimelineView::visible_ms() const { return zoom > 0 ? length_ms / zoom : length_ms; }

void TimelineView::fit(float left, float w, float length) {
    x = left;
    width = std::max(w, 1.0f);
    length_ms = std::max(length, 0.0f);
    zoom = std::min(kMaxTimelineZoom, std::max(1.0f, zoom));
    start_ms = std::min(std::max(length_ms - visible_ms(), 0.0f), std::max(0.0f, start_ms));
}

void TimelineView::zoom_at(float px, float factor) {
    if (!(length_ms > 0) || !(factor > 0)) return;
    const float under = ms_at(px);
    zoom = std::min(kMaxTimelineZoom, std::max(1.0f, zoom * factor));
    start_ms = under - (px - x) / px_per_ms();
    fit(x, width, length_ms);
}

void TimelineView::pan_px(float dx) {
    const float s = px_per_ms();
    if (!(s > 0)) return;
    start_ms -= dx / s;
    fit(x, width, length_ms);
}

float tick_step_ms(float px_per_ms, float min_px) {
    if (!(px_per_ms > 0)) return 1000.0f;
    float decade = 1.0f;
    for (int i = 0; i < 9; ++i, decade *= 10.0f)
        for (float m : {1.0f, 2.0f, 5.0f})
            if (m * decade * px_per_ms >= min_px) return m * decade;
    return decade;
}

KeyRef key_near(const anim::Clip& clip, const TimelineRow& row, float t_ms, float tolerance_ms, float duration_ms) {
    KeyRef best;
    float best_d = tolerance_ms;
    for (const KeyRef& k : row.keys) {
        const float d = std::fabs(clip.tracks[k.track].keys[k.key].t * duration_ms - t_ms);
        if (d <= best_d) {
            best_d = d;
            best = k;
        }
    }
    return best;
}

Selection keys_in_box(const anim::Clip& clip, const std::vector<TimelineRow>& rows, int row_a, int row_b, float ms_a, float ms_b,
                      float duration_ms) {
    Selection sel;
    if (rows.empty()) return sel;
    const int last = static_cast<int>(rows.size()) - 1;
    const int r0 = std::max(0, std::min(row_a, row_b)), r1 = std::min(last, std::max(row_a, row_b));
    const float lo = std::min(ms_a, ms_b), hi = std::max(ms_a, ms_b);
    for (int r = r0; r <= r1; ++r)
        for (const KeyRef& k : rows[r].keys) {
            const float t = clip.tracks[k.track].keys[k.key].t * duration_ms;
            if (t >= lo && t <= hi) select_add(sel, k);
        }
    return sel;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The edits

namespace {

KeyEditResult fail(const std::string& text, const std::string& why) {
    KeyEditResult r;
    r.text = text;
    r.error = why;
    return r;
}

std::string first_message(const anim::ClipLoad& cl) {
    if (cl.errors.empty()) return "the clip is not valid";
    return anim::format_error(cl.errors.front());
}

bool parse(const std::string& text, anim::Clip& clip, std::string& err) {
    anim::ClipLoad cl = anim::parse_clip(text, "clip");
    if (!cl.ok) {
        err = "the clip has errors: " + first_message(cl);
        return false;
    }
    clip = std::move(cl.clip);
    return true;
}

// Takes `out` as the result when the loader's validator passes it.
KeyEditResult finish(const std::string& text, std::string out, KeyEditResult res) {
    anim::ClipLoad after = anim::parse_clip(out, "clip");
    if (!after.ok) return fail(text, "the edit made an invalid clip (" + first_message(after) + "); nothing was changed");
    for (const KeyRef& k : res.selection)
        if (k.track < 0 || static_cast<size_t>(k.track) >= after.clip.tracks.size() || k.key < 0 ||
            static_cast<size_t>(k.key) >= after.clip.tracks[k.track].keys.size())
            return fail(text, "the edit did not land on a key; nothing was changed");
    res.ok = true;
    res.text = std::move(out);
    res.clip = std::move(after.clip);
    return res;
}

bool valid_selection(const anim::Clip& clip, const Selection& sel) {
    for (const KeyRef& k : sel)
        if (k.track < 0 || static_cast<size_t>(k.track) >= clip.tracks.size() || k.key < 0 ||
            static_cast<size_t>(k.key) >= clip.tracks[k.track].keys.size())
            return false;
    return true;
}

int last_track(const anim::Clip& clip, anim::Part part, anim::Channel channel) {
    int ti = -1;
    for (size_t i = 0; i < clip.tracks.size(); ++i)
        if (clip.tracks[i].part == part && clip.tracks[i].channel == channel && clip.tracks[i].source == anim::Source::Keys)
            ti = static_cast<int>(i);   // a generator has no keys to put one in
    return ti;
}

int key_at(const anim::Track& tr, float u, float duration_ms) {
    for (size_t i = 0; i < tr.keys.size(); ++i)
        if (std::fabs(tr.keys[i].t - u) * duration_ms < kKeyEpsilonMs) return static_cast<int>(i);
    return -1;
}

std::string track_label(const anim::Track& tr) { return std::string(anim::part_name(tr.part)) + "." + anim::channel_name(tr.channel); }

}  // namespace

KeyEditResult add_key(const std::string& text, anim::Part part, anim::Channel channel, float t_ms, float duration_ms) {
    if (!(duration_ms > 0) || !std::isfinite(t_ms)) return fail(text, "the clip has no length to put a key in");
    anim::Clip clip;
    std::string err;
    if (!parse(text, clip, err)) return fail(text, err);
    const int ti = last_track(clip, part, channel);
    if (ti < 0)
        return fail(text, std::string("the clip has no track for ") + anim::part_name(part) + "." + anim::channel_name(channel) +
                              ": drag a handle of the part to make one");
    const anim::Track& tr = clip.tracks[ti];
    const float u = std::min(1.0f, std::max(0.0f, t_ms / duration_ms));
    KeyEditResult res;
    const int have = key_at(tr, u, duration_ms);
    if (have >= 0) {
        res.ok = true;
        res.text = text;
        res.clip = std::move(clip);
        res.selection = {{ti, have}};
        return res;
    }
    anim::Key nk;
    nk.t = u;
    anim::sample_track(tr, u, nk.v);
    size_t j = 0;
    while (j < tr.keys.size() && tr.keys[j].t <= u) ++j;
    nk.ease = j < tr.keys.size() ? tr.keys[j].ease : anim::Ease::Linear;   // the segment it splits keeps its shape
    std::string out;
    int index = -1;
    if (!insert_key(text, ti, nk, out, err, &index)) return fail(text, err);
    res.selection = {{ti, index}};
    return finish(text, std::move(out), std::move(res));
}

float clamp_move_ms(const anim::Clip& clip, const Selection& sel, float delta_ms, float duration_ms) {
    if (sel.empty() || !valid_selection(clip, sel) || !(duration_ms > 0) || !std::isfinite(delta_ms)) return 0.0f;
    float lo = 1.0f, hi = 0.0f;
    for (const KeyRef& k : sel) {
        const float t = clip.tracks[k.track].keys[k.key].t;
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    return std::min((1.0f - hi) * duration_ms, std::max(-lo * duration_ms, delta_ms));
}

KeyEditResult move_keys(const std::string& text, const Selection& sel, float delta_ms, float duration_ms) {
    if (!(duration_ms > 0)) return fail(text, "the clip has no length to move a key in");
    anim::Clip clip;
    std::string err;
    if (!parse(text, clip, err)) return fail(text, err);
    if (sel.empty()) return fail(text, "no key is selected");
    if (!valid_selection(clip, sel)) return fail(text, "the selection names a key the clip does not have");
    const float du = clamp_move_ms(clip, sel, delta_ms, duration_ms) / duration_ms;

    KeyEditResult res;
    if (std::fabs(du) * duration_ms < 1e-3f) {
        res.ok = true;
        res.text = text;
        res.clip = std::move(clip);
        res.selection = sel;
        return res;
    }

    struct Entry {
        int index;        // in the track as it is
        float t;          // where the key is after the move
        bool moved;
        bool gone;        // replaced by a moved key
    };
    std::vector<KeyTime> in_place;                      // tracks whose order stays: only the numbers change
    std::vector<KeyRef> removals;                       // every key to take out
    std::vector<std::pair<int, anim::Key>> inserts;     // (track, key) for tracks whose order changes
    for (size_t ti = 0; ti < clip.tracks.size(); ++ti) {
        const anim::Track& tr = clip.tracks[ti];
        std::vector<Entry> es;
        bool any = false;
        for (size_t k = 0; k < tr.keys.size(); ++k) {
            const bool moved = selected(sel, {static_cast<int>(ti), static_cast<int>(k)});
            any = any || moved;
            float t = tr.keys[k].t;
            if (moved) t = std::min(1.0f, std::max(0.0f, t + du));
            es.push_back({static_cast<int>(k), t, moved, false});
        }
        if (!any) continue;
        for (Entry& e : es) {
            if (e.moved) continue;
            for (const Entry& m : es)
                if (m.moved && std::fabs(m.t - e.t) * duration_ms < kKeyEpsilonMs) e.gone = true;
        }
        bool in_order = true;
        float prev = -1.0f;
        for (const Entry& e : es) {
            if (e.gone) continue;
            if (e.t < prev) in_order = false;
            prev = e.t;
        }
        // Where each moved key is in the new clip: the keys that stay keep their order, the moved ones go in by time.
        std::vector<Entry> after;
        for (const Entry& e : es)
            if (!e.gone && (in_order || !e.moved)) after.push_back(e);
        if (!in_order)
            for (const Entry& e : es) {
                if (!e.moved) continue;
                size_t j = 0;
                while (j < after.size() && after[j].t <= e.t) ++j;
                after.insert(after.begin() + static_cast<std::ptrdiff_t>(j), e);
            }
        for (size_t i = 0; i < after.size(); ++i)
            if (after[i].moved) res.selection.push_back({static_cast<int>(ti), static_cast<int>(i)});

        for (const Entry& e : es) {
            if (e.gone) {
                removals.push_back({static_cast<int>(ti), e.index});
                ++res.replaced;
            } else if (e.moved && in_order) {
                in_place.push_back({static_cast<int>(ti), e.index, e.t});
            } else if (e.moved) {
                removals.push_back({static_cast<int>(ti), e.index});
                anim::Key k = tr.keys[e.index];
                k.t = e.t;
                inserts.emplace_back(static_cast<int>(ti), k);
            }
        }
    }

    std::string out = text, next;
    if (!in_place.empty()) {
        if (!set_key_times(out, in_place, next, err)) return fail(text, err);
        out.swap(next);
    }
    // The last key first, so the indexes of the keys still to go stay true.
    std::sort(removals.begin(), removals.end());
    for (auto it = removals.rbegin(); it != removals.rend(); ++it) {
        if (!remove_key(out, it->track, it->key, next, err)) return fail(text, err);
        out.swap(next);
    }
    // The earliest key first: each lands after the ones before it.
    std::stable_sort(inserts.begin(), inserts.end(), [](const auto& a, const auto& b) { return a.second.t < b.second.t; });
    for (const auto& ins : inserts) {
        if (!insert_key(out, ins.first, ins.second, next, err)) return fail(text, err);
        out.swap(next);
    }
    std::sort(res.selection.begin(), res.selection.end());
    return finish(text, std::move(out), std::move(res));
}

KeyEditResult delete_keys(const std::string& text, const Selection& sel) {
    anim::Clip clip;
    std::string err;
    if (!parse(text, clip, err)) return fail(text, err);
    if (sel.empty()) return fail(text, "no key is selected");
    if (!valid_selection(clip, sel)) return fail(text, "the selection names a key the clip does not have");
    std::vector<size_t> count(clip.tracks.size(), 0);
    for (const KeyRef& k : sel) ++count[k.track];
    for (size_t t = 0; t < clip.tracks.size(); ++t)
        if (count[t] > 0 && count[t] >= clip.tracks[t].keys.size())
            return fail(text, "the track " + track_label(clip.tracks[t]) +
                                  " would have no key left, and a track needs one: leave a key, or remove the track in the file");
    Selection order = sel;
    std::sort(order.begin(), order.end());
    std::string out = text, next;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (!remove_key(out, it->track, it->key, next, err)) return fail(text, err);
        out.swap(next);
    }
    return finish(text, std::move(out), KeyEditResult{});
}

KeyClipboard copy_keys(const anim::Clip& clip, const Selection& sel, float duration_ms) {
    KeyClipboard cb;
    if (sel.empty() || !valid_selection(clip, sel)) return cb;
    float first = 1.0f;
    for (const KeyRef& k : sel) first = std::min(first, clip.tracks[k.track].keys[k.key].t);
    for (const KeyRef& k : sel) {
        const anim::Track& tr = clip.tracks[k.track];
        const anim::Key& key = tr.keys[k.key];
        ClipboardKey c;
        c.part = tr.part;
        c.channel = tr.channel;
        c.offset_ms = (key.t - first) * duration_ms;
        std::memcpy(c.v, key.v, sizeof c.v);
        c.ease = key.ease;
        cb.keys.push_back(c);
    }
    std::stable_sort(cb.keys.begin(), cb.keys.end(), [](const ClipboardKey& a, const ClipboardKey& b) { return a.offset_ms < b.offset_ms; });
    return cb;
}

KeyEditResult paste_keys(const std::string& text, const KeyClipboard& cb, float at_ms, float duration_ms) {
    if (!(duration_ms > 0) || !std::isfinite(at_ms)) return fail(text, "the clip has no length to paste a key in");
    if (cb.empty()) return fail(text, "nothing was copied");
    anim::Clip clip;
    std::string err;
    if (!parse(text, clip, err)) return fail(text, err);

    KeyEditResult res;
    struct Landed {
        anim::Part part;
        anim::Channel channel;
        float t;
    };
    std::vector<Landed> landed;
    std::string out = text, next;
    for (const ClipboardKey& c : cb.keys) {
        const float t_ms = std::max(0.0f, at_ms) + c.offset_ms;
        if (t_ms > duration_ms + kKeyEpsilonMs) {
            ++res.dropped;
            continue;
        }
        anim::Key k;
        k.t = std::min(1.0f, t_ms / duration_ms);
        std::memcpy(k.v, c.v, sizeof k.v);
        k.ease = c.ease;
        const int ti = last_track(clip, c.part, c.channel);
        if (ti < 0) {
            if (!append_track(out, c.part, c.channel, {k}, next, err)) return fail(text, err);
            ++res.made_tracks;
        } else {
            const int have = key_at(clip.tracks[ti], k.t, duration_ms);
            if (have >= 0) {
                if (!replace_key(out, ti, have, k, next, err)) return fail(text, err);
                ++res.replaced;
            } else if (!insert_key(out, ti, k, next, err)) {
                return fail(text, err);
            }
        }
        out.swap(next);
        // The next key is placed in the clip as it is now.
        anim::ClipLoad cl = anim::parse_clip(out, "clip");
        if (!cl.ok) return fail(text, "the paste made an invalid clip (" + first_message(cl) + "); nothing was changed");
        clip = std::move(cl.clip);
        landed.push_back({c.part, c.channel, k.t});
    }
    if (landed.empty()) return fail(text, "every copied key falls past the end of the clip: scrub to an earlier time");
    for (const Landed& l : landed) {
        const int ti = last_track(clip, l.part, l.channel);
        const int ki = ti >= 0 ? key_at(clip.tracks[ti], l.t, duration_ms) : -1;
        if (ki >= 0) select_add(res.selection, {ti, ki});
    }
    return finish(text, std::move(out), std::move(res));
}

KeyEditResult fail_edit(const std::string& text, const std::string& why) { return fail(text, why); }

KeyEditResult finish_edit(const std::string& text, std::string out, KeyEditResult res) {
    return finish(text, std::move(out), std::move(res));
}

}  // namespace kanimator
