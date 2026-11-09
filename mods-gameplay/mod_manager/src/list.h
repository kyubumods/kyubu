// The My mods list and the details (design D9): what the page shows for the installed mods, with no game calls so that unit
// tests cover it. The glue (main.cpp) fills `View::mods` from mods_get and draws what this header decides: which rows are shown, in
// which order, which one is selected, where the list is scrolled, and the lines of the details.
#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "text.h"

namespace list {

enum class State { Loaded, Disabled, Failed, Faulted, Skipped, New, Missing };   // KyubuModState, in the same order
enum class Mark { Unknown, Verified, Unverified };   // Unknown until the catalog is loaded (section 7 decides the other two)
enum class Filter { All, Enabled, Disabled, Problems, Updates, Get, Installed };   // Get and Installed: the Install tab only
constexpr int kFilterCount = 5;
constexpr size_t kQueryMax = 40;

struct Dep {
    std::wstring id, req;
    bool optional = false;
    bool installed = false;   // a package with this id is in the mods folder
};

// The Install tab: what a catalog mod offers this player, and one line per version of it.
enum class Offer { None, Get, Update, Installed };
struct Ver {
    std::wstring version, size, published, changes, needs;
    bool fits = true;   // its loader requirement fits this loader
};

struct Mod {
    std::wstring id, name, version, authors, description, reason, update_to, mark_reason;
    Offer offer = Offer::None;     // catalog mods only
    std::wstring size_text;        // catalog mods: the size of the shown version
    std::wstring have;             // catalog mods: the installed version, empty when none
    std::vector<Ver> vers;         // catalog mods: newest first
    State state = State::Loaded;
    Mark mark = Mark::Unknown;
    std::vector<Dep> needs, needed_by;
};

inline const wchar_t* filter_name(Filter f) {
    switch (f) {
        case Filter::All: return L"ALL";
        case Filter::Enabled: return L"ENABLED";
        case Filter::Disabled: return L"DISABLED";
        case Filter::Problems: return L"PROBLEMS";
        case Filter::Updates: return L"UPDATES";
        case Filter::Get: return L"NOT INSTALLED";
        case Filter::Installed: return L"INSTALLED";
    }
    return L"";
}

inline const wchar_t* state_label(State s) {
    switch (s) {
        case State::Loaded: return L"Loaded";
        case State::Disabled: return L"Disabled";
        case State::Failed: return L"Failed";
        case State::Faulted: return L"Crashed";
        case State::Skipped: return L"Skipped";
        case State::New: return L"New";
        case State::Missing: return L"Missing";
    }
    return L"";
}

inline bool is_problem(State s) { return s == State::Failed || s == State::Faulted || s == State::Skipped || s == State::Missing; }

// The switch of a row: on, off, or a mark that something is wrong. A new package counts as on (it is enabled by default, D15).
inline const wchar_t* switch_label(State s) {
    if (is_problem(s)) return L"[ ! ]";
    return (s == State::Loaded || s == State::New) ? L"[ON ]" : L"[OFF]";
}

// The reason line of the details: the loader's own reason for a mod that did not load, a fixed text for a new or missing package.
inline std::wstring reason_text(const Mod& m) {
    if (m.state == State::New) return m.reason.empty() ? L"Loads after a restart." : m.reason;
    if (m.state == State::Missing) return L"Removed from the folder.";
    if (m.state == State::Loaded) return L"";
    return m.reason;
}

inline bool in_filter(const Mod& m, Filter f) {
    switch (f) {
        case Filter::All: return true;
        case Filter::Enabled: return m.state == State::Loaded || m.state == State::New;
        case Filter::Disabled: return m.state == State::Disabled;
        case Filter::Problems: return is_problem(m.state);
        case Filter::Updates: return !m.update_to.empty();
        case Filter::Get: return m.offer == Offer::Get;
        case Filter::Installed: return m.offer == Offer::Installed || m.offer == Offer::Update;
    }
    return true;
}

inline std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Search matches name, id and authors, any case; blanks around the query are ignored.
inline bool matches(const Mod& m, const std::wstring& query) {
    const size_t a = query.find_first_not_of(L' ');
    if (a == std::wstring::npos) return true;
    const std::wstring q = lower(query.substr(a, query.find_last_not_of(L' ') - a + 1));
    return lower(m.name).find(q) != std::wstring::npos || lower(m.id).find(q) != std::wstring::npos ||
           lower(m.authors).find(q) != std::wstring::npos;
}

struct View {
    std::vector<Mod> mods;
    Filter filter = Filter::All;
    std::wstring query;
    bool catalog = false;        // the Install tab's list: the filters are ALL, NOT INSTALLED, UPDATES, INSTALLED
    bool packs = false;          // the Modpacks list: one filter, ALL
    std::wstring selected;       // the id of the selected mod; empty for none
    std::vector<int> shown;      // indexes into `mods`, in the order drawn
    int first = 0;               // the first row drawn (scroll)
    int rows = 8;                // how many rows fit; the layout sets it
    int info_first = 0;          // the first line of the details drawn (scroll)

    // Recomputes `shown` after the mods, the filter or the query changed. New packages sit at the top (D15); the rest keep the
    // loader's order. The selection stays while its mod is shown, else it moves to the first row.
    void rebuild() {
        shown.clear();
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 0; i < mods.size(); ++i)
                if ((mods[i].state == State::New) == (pass == 0) && in_filter(mods[i], filter) && matches(mods[i], query))
                    shown.push_back(static_cast<int>(i));
        if (selected_row() < 0) {
            selected = shown.empty() ? L"" : mods[shown[0]].id;
            info_first = 0;
        }
        clamp();
    }
    void set_mods(std::vector<Mod> m) {
        mods = std::move(m);
        rebuild();
    }
    int selected_row() const {   // the position of the selection in `shown`, or -1
        for (size_t r = 0; r < shown.size(); ++r)
            if (mods[shown[r]].id == selected) return static_cast<int>(r);
        return -1;
    }
    const Mod* current() const {
        const int r = selected_row();
        return r < 0 ? nullptr : &mods[shown[r]];
    }
    const Mod* find(const std::wstring& id) const {
        for (const Mod& m : mods)
            if (m.id == id) return &m;
        return nullptr;
    }
    void clamp() {
        first = std::clamp(first, 0, std::max(0, static_cast<int>(shown.size()) - std::max(rows, 1)));
        info_first = std::max(0, info_first);
    }
    void select_row(int row) {
        if (row < 0 || row >= static_cast<int>(shown.size())) return;
        selected = mods[shown[row]].id;
        info_first = 0;
    }
    // The arrows of the filter picker: +1 next, -1 previous, wrapping.
    void step_filter(int dir) {
        if (packs) return;
        static const Filter mine[kFilterCount] = {Filter::All, Filter::Enabled, Filter::Disabled, Filter::Problems, Filter::Updates};
        static const Filter shop[4] = {Filter::All, Filter::Get, Filter::Updates, Filter::Installed};
        const Filter* set = catalog ? shop : mine;
        const int count = catalog ? 4 : kFilterCount;
        int at = 0;
        for (int i = 0; i < count; ++i) if (set[i] == filter) at = i;
        filter = set[(at + dir + count) % count];
        first = 0;
        rebuild();
    }
    void type_char(unsigned cp) {
        if (cp < 32 || cp == 127 || cp > 0xFFFF || query.size() >= kQueryMax) return;
        query.push_back(static_cast<wchar_t>(cp));
        first = 0;
        rebuild();
    }
    void backspace() {
        if (query.empty()) return;
        query.pop_back();
        first = 0;
        rebuild();
    }
    // A dependency chip was clicked: show its mod, whatever the filter and the search hide.
    bool jump_to(const std::wstring& id) {
        if (!find(id)) return false;
        if (!in_filter(*find(id), filter) || !matches(*find(id), query)) { filter = Filter::All; query.clear(); }
        selected = id;
        info_first = 0;
        rebuild();
        const int r = selected_row();
        if (r >= 0) first = std::clamp(first, r - rows + 1, r);
        clamp();
        return true;
    }
    void scroll_rows(int delta) { first += delta; clamp(); }
    int on_count() const {
        int n = 0;
        for (const Mod& m : mods)
            n += catalog ? (m.offer == Offer::Installed || m.offer == Offer::Update ? 1 : 0) : (m.state == State::Loaded || m.state == State::New ? 1 : 0);
        return n;
    }
    std::wstring count_text() const {
        const size_t n = mods.size();
        if (packs) return (shown.size() != n ? std::to_wstring(shown.size()) + L" OF " : L"") + std::to_wstring(n) + (n == 1 ? L" MODPACK" : L" MODPACKS");
        std::wstring s = std::to_wstring(n) + (n == 1 ? L" MOD, " : L" MODS, ") + std::to_wstring(on_count()) + (catalog ? L" INSTALLED" : L" ON");
        if (shown.size() != n) s = std::to_wstring(shown.size()) + L" OF " + s;
        return s;
    }
};

// A text of the details in one colour: kind 0 plain, 1 secondary, 2 warning, 3 error, 4 good.
struct Seg { std::wstring text; int kind = 0; };
struct Line { std::vector<Seg> segs; };

inline std::wstring mark_text(Mark m) {
    switch (m) {
        case Mark::Verified: return L"Verified";
        case Mark::Unverified: return L"Unverified";
        case Mark::Unknown: break;
    }
    return L"Not checked";
}
inline int mark_kind(Mark m) { return m == Mark::Verified ? 4 : m == Mark::Unverified ? 2 : 1; }
inline int state_kind(State s) {
    if (is_problem(s)) return 3;
    return s == State::Loaded ? 4 : s == State::New ? 2 : 1;
}

// The details under the header (name and version are drawn by the glue): authors, mark and state, the reason, the description
// wrapped to `width_of` at `max_width`. Needs and Needed by are chips, which the glue lays out itself.
template <typename WidthFn>
std::vector<Line> details_lines(const Mod& m, float max_width, WidthFn width_of) {
    std::vector<Line> out;
    out.push_back({{{m.authors.empty() ? L"No author given" : L"by " + m.authors, 1}}});
    out.push_back({{{mark_text(m.mark), mark_kind(m.mark)}, {L"  -  ", 1}, {state_label(m.state), state_kind(m.state)}}});
    if (m.mark == Mark::Unverified && !m.mark_reason.empty())
        for (const std::wstring& l : text::wrap(m.mark_reason, max_width, width_of)) out.push_back({{{l, 2}}});
    if (!m.update_to.empty()) out.push_back({{{L"Update available: " + m.update_to, 2}}});
    const std::wstring why = reason_text(m);
    if (!why.empty())
        for (const std::wstring& l : text::wrap(why, max_width, width_of)) out.push_back({{{l, is_problem(m.state) ? 3 : 2}}});
    if (!m.description.empty()) {
        out.push_back({{}});
        for (const std::wstring& l : text::wrap(m.description, max_width, width_of)) out.push_back({{{l, 0}}});
    }
    return out;
}

// One row of the scrollable details: plain text, a label ("NEEDS:") on a row of its own, or a row of dependency chips under it (a chip is
// a mod that can be clicked when installed).
constexpr float kChipIndent = 16.0f;
struct Chip {
    std::wstring text, id;
    bool installed = false;
    int kind = 0;   // 0 plain, 1 secondary (an optional mod that is not installed), 3 error (a needed mod that is not installed)
};
struct InfoRow {
    Line line;                   // a text row
    std::wstring label;          // a label row: "NEEDS:" or "NEEDED BY:", with no chips
    std::vector<Chip> chips;     // a chip row, drawn kChipIndent from the left
};

inline Chip make_chip(const View& v, const Dep& d, bool show_req) {
    Chip c;
    const Mod* m = v.find(d.id);
    c.id = d.id;
    c.installed = m != nullptr;
    c.text = m ? m->name : d.id;
    if (show_req && !d.req.empty() && d.req != L"*") c.text += L" " + d.req;
    if (!c.installed) {
        c.text += d.optional ? L" (optional)" : L" (missing)";
        c.kind = d.optional ? 1 : 3;
    }
    return c;
}

// Everything under the header, in the order drawn: `details_lines`, then the chips of what the mod needs and of what needs it. Each group
// is a label row and then rows of chips that wrap to `max_width`; `gap` is the space between two chips.
template <typename WidthFn>
std::vector<InfoRow> info_rows(const View& v, const Mod& m, float max_width, float gap, WidthFn width_of) {
    std::vector<InfoRow> out;
    for (Line& l : details_lines(m, max_width, width_of)) out.push_back({std::move(l), L"", {}});
    out.push_back({{}, L"", {}});
    const struct { const wchar_t* label; const std::vector<Dep>* deps; bool req; } groups[] = {{L"NEEDS", &m.needs, true}, {L"NEEDED BY", &m.needed_by, false}};
    for (const auto& g : groups) {
        if (g.deps->empty()) {
            out.push_back({{{{std::wstring(g.label) + L": none", 1}}}, L"", {}});
            continue;
        }
        out.push_back({{}, std::wstring(g.label) + L":", {}});
        InfoRow row;
        float used = kChipIndent;
        for (const Dep& d : *g.deps) {
            Chip c = make_chip(v, d, g.req);
            const float w = width_of(c.text);
            if (!row.chips.empty() && used + w > max_width) {
                out.push_back(std::move(row));
                row = InfoRow();
                used = kChipIndent;
            }
            row.chips.push_back(std::move(c));
            used += w + gap;
        }
        out.push_back(std::move(row));
    }
    return out;
}

}  // namespace list
