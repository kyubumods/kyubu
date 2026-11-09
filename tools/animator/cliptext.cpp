// The Animator's edits of a clip's text: see cliptext.h.
#include "cliptext.h"

#include <algorithm>
#include <cstdio>

#include "toml_config.h"

namespace kanimator {

namespace {

// The byte offsets of the starts of the lines, and the (line, column) to offset map toml++ needs. Its columns count characters.
struct Text {
    const std::string& s;
    std::vector<size_t> line_start;
    explicit Text(const std::string& t) : s(t) {
        line_start.push_back(0);
        for (size_t i = 0; i < s.size(); ++i)
            if (s[i] == '\n') line_start.push_back(i + 1);
    }
    bool offset(const toml::source_position& p, size_t& out) const {
        if (p.line < 1 || p.line > line_start.size() || p.column < 1) return false;
        size_t i = line_start[p.line - 1];
        size_t end = p.line < line_start.size() ? line_start[p.line] : s.size();
        for (toml::source_index c = 1; c < p.column; ++c) {
            if (i >= end) return false;
            ++i;
            while (i < end && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;   // a character is its lead byte and continuations
        }
        out = i;
        return true;
    }
    size_t line_of_offset(size_t off) const {
        return static_cast<size_t>(std::upper_bound(line_start.begin(), line_start.end(), off) - line_start.begin()) - 1;
    }
};

struct Span {
    size_t begin = 0, end = 0;
};

// toml++ gives a node's region with an exclusive end. A `[[table]]`'s region is its header only; an inline table's is all of it.
bool span_of(const Text& tx, const toml::node& n, Span& out) {
    toml::source_region r = n.source();
    return tx.offset(r.begin, out.begin) && tx.offset(r.end, out.end) && out.end >= out.begin && out.end <= tx.s.size();
}

std::string trimmed(const char* buf) {
    std::string t = buf;
    size_t dot = t.find('.');
    if (dot != std::string::npos) {
        while (!t.empty() && t.back() == '0') t.pop_back();
        if (!t.empty() && t.back() == '.') t.pop_back();
    }
    if (t == "-0" || t.empty()) t = "0";
    return t;
}

bool blank(char c) { return c == ' ' || c == '\t'; }

// The text of one clip, parsed for the places of its keys.
struct Doc {
    toml::parse_result pr;
    Text tx;
    explicit Doc(const std::string& text) : pr(toml::parse(text, std::string("clip"))), tx(text) {}

    const toml::array* tracks() const {
        if (!pr) return nullptr;
        const toml::node* n = pr.table().get("track");
        return n ? n->as_array() : nullptr;
    }
    // The `keys` list of a track, or null with `err` set.
    const toml::array* keys(int track, std::string& err) const {
        if (!pr) {
            err = "the clip text could not be read again";
            return nullptr;
        }
        const toml::array* ta = tracks();
        const toml::table* tt = ta && track >= 0 && static_cast<size_t>(track) < ta->size() ? (*ta)[track].as_table() : nullptr;
        const toml::array* ka = tt && tt->get("keys") ? tt->get("keys")->as_array() : nullptr;
        if (!ka) err = "the track's keys are not a list the Animator can edit";
        return ka;
    }
    const toml::table* key(int track, int k, std::string& err) const {
        const toml::array* ka = keys(track, err);
        if (!ka) return nullptr;
        const toml::table* kt = k >= 0 && static_cast<size_t>(k) < ka->size() ? (*ka)[k].as_table() : nullptr;
        if (!kt) err = "the track's keys are not a list the Animator can edit";
        return kt;
    }
};

const char* kTablesNotList = "the keys are written as `[[track.keys]]` tables; the Animator edits a `keys = [ ... ]` list";

bool all_inline(const toml::array& keys) {
    for (const toml::node& el : keys) {
        const toml::table* kt = el.as_table();
        if (!kt || !kt->is_inline()) return false;
    }
    return true;
}

}  // namespace

std::string fmt_value(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return trimmed(buf);
}

std::string fmt_time(double t) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.5f", t);
    std::string s = trimmed(buf);
    if (s.find('.') == std::string::npos) s += ".0";
    return s;
}

std::string vec_text(const float v[3]) { return "[" + fmt_value(v[0]) + ", " + fmt_value(v[1]) + ", " + fmt_value(v[2]) + "]"; }

std::string key_text(float t, const float v[3], anim::Ease ease) {
    std::string s = "{ t = " + fmt_time(t) + ", v = " + vec_text(v);
    if (ease != anim::Ease::Linear) s += std::string(", ease = \"") + anim::ease_name(ease) + "\"";
    return s + " }";
}

std::string text_eol(const std::string& text) { return text.find("\r\n") != std::string::npos ? "\r\n" : "\n"; }

bool set_key_times(const std::string& text, const std::vector<KeyTime>& times, std::string& out, std::string& err) {
    const Doc doc(text);
    struct Put {
        Span at;
        std::string with;
    };
    std::vector<Put> puts;
    for (const KeyTime& kt : times) {
        const toml::table* k = doc.key(kt.track, kt.key, err);
        if (!k) return false;
        const toml::node* tn = k->get("t");
        Put p;
        if (!tn || !tn->is_number() || !span_of(doc.tx, *tn, p.at)) {
            err = "the key's time is not a number the Animator can edit";
            return false;
        }
        p.with = fmt_time(kt.t);
        puts.push_back(std::move(p));
    }
    // From the end of the text to its start, so the offsets not yet used stay true.
    std::sort(puts.begin(), puts.end(), [](const Put& a, const Put& b) { return a.at.begin > b.at.begin; });
    for (size_t i = 0; i + 1 < puts.size(); ++i)
        if (puts[i + 1].at.end > puts[i].at.begin) {
            err = "a key is named twice";
            return false;
        }
    out = text;
    for (const Put& p : puts) out.replace(p.at.begin, p.at.end - p.at.begin, p.with);
    return true;
}

bool set_key_value(const std::string& text, int track, int key, const float v[3], std::string& out, std::string& err) {
    const Doc doc(text);
    const toml::table* kt = doc.key(track, key, err);
    if (!kt) return false;
    const toml::array* va = kt->get("v") ? kt->get("v")->as_array() : nullptr;
    Span sp;
    if (!va || !span_of(doc.tx, *va, sp)) {
        err = "the key's value is not a list the Animator can edit";
        return false;
    }
    out = text;
    out.replace(sp.begin, sp.end - sp.begin, vec_text(v));
    return true;
}

bool replace_key(const std::string& text, int track, int key, const anim::Key& with, std::string& out, std::string& err) {
    const Doc doc(text);
    const toml::table* kt = doc.key(track, key, err);
    if (!kt) return false;
    if (!kt->is_inline()) {
        err = kTablesNotList;
        return false;
    }
    Span sp;
    if (!span_of(doc.tx, *kt, sp)) {
        err = "a key's place in the text is not known";
        return false;
    }
    out = text;
    out.replace(sp.begin, sp.end - sp.begin, key_text(with.t, with.v, with.ease));
    return true;
}

bool remove_key(const std::string& text, int track, int key, std::string& out, std::string& err) {
    const Doc doc(text);
    const toml::table* kt = doc.key(track, key, err);
    if (!kt) return false;
    if (!kt->is_inline()) {
        err = kTablesNotList;
        return false;
    }
    Span ks;
    if (!span_of(doc.tx, *kt, ks)) {
        err = "a key's place in the text is not known";
        return false;
    }
    const size_t ls = doc.tx.line_start[doc.tx.line_of_offset(ks.begin)];
    size_t q = ks.end;
    while (q < text.size() && blank(text[q])) ++q;
    const bool comma = q < text.size() && text[q] == ',';
    const size_t after = comma ? q + 1 : q;
    size_t eol_at = text.find('\n', after);
    if (eol_at == std::string::npos) eol_at = text.size();
    size_t first = after;
    while (first < eol_at && (blank(text[first]) || text[first] == '\r')) ++first;
    const bool tail_blank = first == eol_at || text[first] == '#';
    bool lead_blank = true;
    for (size_t i = ls; i < ks.begin; ++i) lead_blank = lead_blank && blank(text[i]);

    size_t from = ks.begin, to = ks.end;
    if (lead_blank && tail_blank) {
        // The key has the line to itself: the line goes, with its end and a comment after the key.
        from = ls;
        to = eol_at < text.size() ? eol_at + 1 : eol_at;
    } else if (comma) {
        to = after;
        while (to < text.size() && blank(text[to])) ++to;
    } else {
        // The last key of its line with no comma after it: the comma before it goes instead.
        size_t p = ks.begin;
        while (p > ls && blank(text[p - 1])) --p;
        if (p > ls && text[p - 1] == ',') from = p - 1;
    }
    out = text;
    out.erase(from, to - from);
    return true;
}

bool insert_key(const std::string& text, int track, const anim::Key& key, std::string& out, std::string& err, int* index) {
    const Doc doc(text);
    const toml::array* keys = doc.keys(track, err);
    if (!keys) return false;
    if (!all_inline(*keys)) {
        err = kTablesNotList;
        return false;
    }
    const size_t n = keys->size();
    // The place in the order: after the keys at or before the new time.
    size_t j = 0;
    for (; j < n; ++j) {
        const toml::node* tn = (*keys)[j].as_table()->get("t");
        const float t = tn ? static_cast<float>(tn->value_or(0.0)) : 0.0f;
        if (!(t <= key.t)) break;
    }
    const std::string eol = text_eol(text);
    const std::string elem = key_text(key.t, key.v, key.ease);
    const Text& tx = doc.tx;
    out = text;
    if (j < n) {
        // Before key j: a new line of its own when that key starts its line, else inline.
        Span ks;
        if (!span_of(tx, (*keys)[j], ks)) {
            err = "a key's place in the text is not known";
            return false;
        }
        const size_t ls = tx.line_start[tx.line_of_offset(ks.begin)];
        const std::string lead = text.substr(ls, ks.begin - ls);
        if (lead.find_first_not_of(" \t") == std::string::npos) out.insert(ls, lead + elem + "," + eol);
        else out.insert(ks.begin, elem + ", ");
    } else {
        Span ks;
        if (n == 0 || !span_of(tx, (*keys)[n - 1], ks)) {
            err = "a key's place in the text is not known";
            return false;
        }
        size_t q = ks.end;
        while (q < text.size() && blank(text[q])) ++q;
        const bool comma = q < text.size() && text[q] == ',';
        const size_t after = comma ? q + 1 : q;
        size_t eol_at = text.find('\n', after);
        if (eol_at == std::string::npos) eol_at = text.size();
        const size_t line_end = eol_at > 0 && eol_at > after && text[eol_at - 1] == '\r' ? eol_at - 1 : eol_at;
        std::string rest = text.substr(after, line_end - after);
        const size_t first = rest.find_first_not_of(" \t");
        const bool tail_blank = first == std::string::npos || rest[first] == '#';
        const size_t ls = tx.line_start[tx.line_of_offset(ks.begin)];
        const std::string lead = text.substr(ls, ks.begin - ls);
        if (tail_blank && lead.find_first_not_of(" \t") == std::string::npos) {
            out.insert(line_end, eol + lead + elem + ",");
            if (!comma) out.insert(ks.end, ",");   // the later offset first, so this one is still where it was
        } else {
            out.insert(ks.end, ", " + elem);
        }
    }
    if (index) *index = static_cast<int>(j);
    return true;
}

bool append_track(const std::string& text, anim::Part part, anim::Channel channel, const std::vector<anim::Key>& keys,
                  std::string& out, std::string& err) {
    return append_track(text, part, channel, anim::TrackMode::Add, keys, out, err);
}

bool append_track(const std::string& text, anim::Part part, anim::Channel channel, anim::TrackMode mode,
                  const std::vector<anim::Key>& keys, std::string& out, std::string& err) {
    const Doc doc(text);
    if (!doc.pr) {
        err = "the clip text could not be read again";
        return false;
    }
    if (keys.empty()) {
        err = "a new track needs a key";
        return false;
    }
    if (const toml::array* ta = doc.tracks())
        for (const toml::node& el : *ta)
            if (const toml::table* t = el.as_table())
                if (t->is_inline()) {
                    err = "the tracks are written as a `track = [ ... ]` list; the Animator adds a `[[track]]` table, "
                          "which the file cannot also have";
                    return false;
                }
    const std::string eol = text_eol(text);
    std::string rows;
    for (const anim::Key& k : keys) rows += "  " + key_text(k.t, k.v, k.ease) + "," + eol;
    out = text;
    if (!out.empty() && out.back() != '\n') out += eol;
    out += eol + "[[track]]" + eol + "part = \"" + anim::part_name(part) + "\"" + eol + "channel = \"" + anim::channel_name(channel) +
           "\"" + eol + "mode = \"" + (mode == anim::TrackMode::Set ? "set" : "add") + "\"" + eol + "keys = [" + eol + rows + "]" + eol;
    return true;
}

bool remove_track(const std::string& text, int track, std::string& out, std::string& err) {
    const Doc doc(text);
    if (!doc.pr) {
        err = "the clip text could not be read again";
        return false;
    }
    const toml::array* ta = doc.tracks();
    const toml::table* tt = ta && track >= 0 && static_cast<size_t>(track) < ta->size() ? (*ta)[track].as_table() : nullptr;
    if (!tt) {
        err = "the track is not in the file";
        return false;
    }
    if (tt->is_inline()) {
        err = "the tracks are written as a `track = [ ... ]` list; the Animator removes a `[[track]]` table";
        return false;
    }
    Span hs;
    if (!span_of(doc.tx, *tt, hs)) {
        err = "a track's place in the text is not known";
        return false;
    }
    size_t from = doc.tx.line_start[doc.tx.line_of_offset(hs.begin)];
    // Comment lines right above the header are the table's own.
    for (size_t line = doc.tx.line_of_offset(hs.begin); line > 0;) {
        const size_t ls = doc.tx.line_start[line - 1], le = doc.tx.line_start[line];
        size_t p = ls;
        while (p < le && blank(text[p])) ++p;
        if (p >= le || text[p] != '#') break;
        from = ls;
        --line;
    }
    // The next table of the file at the top level (another track, `[borrow]`, a `[[hide]]`, ...) ends this one.
    size_t to = text.size();
    for (const auto& kv : doc.pr.table()) {
        std::vector<const toml::table*> heads;
        if (const toml::table* t = kv.second.as_table()) heads.push_back(t);
        if (const toml::array* a = kv.second.as_array())
            for (const toml::node& el : *a)
                if (const toml::table* t = el.as_table()) heads.push_back(t);
        for (const toml::table* t : heads) {
            Span s;
            if (t->is_inline() || !span_of(doc.tx, *t, s) || s.begin <= hs.begin) continue;
            to = std::min(to, doc.tx.line_start[doc.tx.line_of_offset(s.begin)]);
        }
    }
    // Comment and blank lines at the end go with the next table, not with this one.
    while (to > from) {
        size_t last = to;
        if (text[last - 1] == '\n') --last;
        size_t ls = last > 0 ? text.rfind('\n', last - 1) : std::string::npos;
        ls = ls == std::string::npos ? 0 : ls + 1;
        if (ls <= from) break;
        size_t p = ls;
        while (p < last && (blank(text[p]) || text[p] == '\r')) ++p;
        if (p < last && text[p] != '#') break;
        to = ls;
    }
    // The blank lines above the table were its separator: they go too, unless the table is not last and has no blank line of
    // its own after it, when they separate the neighbours.
    if (to >= text.size() || text[to] == '\n' || text[to] == '\r') {
        while (from > 0) {
            const size_t line = doc.tx.line_of_offset(from - 1);
            const size_t ls = doc.tx.line_start[line];
            size_t p = ls;
            while (p < from && (blank(text[p]) || text[p] == '\r' || text[p] == '\n')) ++p;
            if (p < from) break;
            from = ls;
        }
    }
    out = text;
    out.erase(from, to - from);
    return true;
}

}  // namespace kanimator
