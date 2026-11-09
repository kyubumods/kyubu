// Install / Modpacks (design D7): the list of the signed index's packs, each pack's plan (the planner's apply_pack with the
// pack's loader requirement checked first) and the lines of its details: one per mod (installed, will download, will change, will be
// enabled, will be disabled) and the number of settings that change. Pure: the glue (main.cpp) passes in the index, the planner's
// world and the option fields of the installed mods, so unit tests cover it.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "browse.h"
#include "form.h"
#include "index.h"
#include "json.hpp"
#include "list.h"
#include "plan.h"
#include "text.h"

namespace packs {

// The option fields of an installed mod (its form), or null when none are known.
using FieldsOf = std::function<const std::vector<form::Field>*(const std::string& id)>;

struct Plan {
    plan::Result result;      // the planner's answer; !ok carries the reason
    int settings = 0;         // option values the pack changes
    bool settings_exact = true;   // false when a mod is downloaded: its options are only known after the restart
};

inline std::vector<plan::Pin> pins_of(const cindex::Pack& p) {
    std::vector<plan::Pin> out;
    for (const auto& [id, ver] : p.mods) out.push_back({id, ver});
    return out;
}

// A JSON value of the pack's "config" as the loader's option text (strings without their quotes, like form::Field::value).
inline std::string value_text(const nlohmann::json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return v.dump();
}

// The pack's "config" object as id -> key -> option text; empty when it is missing or not an object of objects.
inline std::map<std::string, std::map<std::string, std::string>> config_of(const cindex::Pack& p) {
    std::map<std::string, std::map<std::string, std::string>> out;
    if (p.config.empty()) return out;
    const nlohmann::json doc = nlohmann::json::parse(p.config, nullptr, false);
    if (!doc.is_object()) return out;
    for (const auto& [id, keys] : doc.items()) {
        if (!keys.is_object()) continue;
        for (const auto& [key, v] : keys.items()) out[id][key] = value_text(v);
    }
    return out;
}

// The settings a pack changes. For a pinned mod that stays installed (Keep, Enable), a named option counts when its value differs
// from the one in use, and with reset_unlisted every other option counts when it is not at its default. For a mod that is downloaded
// the options are not known yet: every named option counts and the count is a lower bound.
inline void count_settings(const cindex::Pack& p, Plan& out, const FieldsOf& fields_of) {
    const auto cfg = config_of(p);
    for (const plan::Op& op : out.result.ops) {
        if (op.kind == plan::Kind::Disable) continue;
        auto named_it = cfg.find(op.id);
        const std::map<std::string, std::string> none;
        const std::map<std::string, std::string>& named = named_it == cfg.end() ? none : named_it->second;
        const std::vector<form::Field>* fields = op.kind == plan::Kind::Download ? nullptr : fields_of(op.id);
        if (!fields) {
            out.settings += static_cast<int>(named.size());
            if (op.kind == plan::Kind::Download && (p.reset_unlisted || !named.empty())) out.settings_exact = false;
            continue;
        }
        size_t matched = 0;
        for (const form::Field& f : *fields) {
            auto n = named.find(f.key);
            if (n != named.end()) {
                ++matched;
                if (!form::same(f.type, n->second, f.value)) ++out.settings;
            } else if (p.reset_unlisted && !form::same(f.type, f.value, f.def)) {
                ++out.settings;
            }
        }
        out.settings += static_cast<int>(named.size() - matched);   // a key the installed version does not declare
    }
}

// The plan of a pack: refused when this loader does not fit the pack's requirement, else the planner's apply_pack.
inline Plan plan_of(const cindex::Pack& p, const plan::World& w, const FieldsOf& fields_of) {
    Plan out;
    if (!browse::satisfies(w.api, p.kyubu)) {
        out.result.reason = "the pack needs loader API " + p.kyubu + ", this is " + w.api;
        return out;
    }
    out.result = plan::apply_pack(w, pins_of(p));
    if (out.result.ok) count_settings(p, out, fields_of);
    return out;
}

inline std::wstring mods_text(size_t n) { return std::to_wstring(n) + (n == 1 ? L" MOD" : L" MODS"); }

// One row per pack, in the index's order. `size_text` holds the number of mods, drawn where a catalog row has its offer.
inline std::vector<list::Mod> pack_rows(const cindex::Index& ix) {
    std::vector<list::Mod> out;
    for (const cindex::Pack& p : ix.packs) {
        list::Mod m;
        m.id = browse::wide(p.id);
        m.name = p.name.empty() ? m.id : browse::utf8_wide(p.name);
        m.authors = browse::join(p.authors);
        m.description = browse::utf8_wide(p.description);
        m.version = browse::wide(p.version);
        m.size_text = mods_text(p.mods.size());
        m.mark = list::Mark::Verified;   // every pack is in the signed index (D7)
        out.push_back(std::move(m));
    }
    return out;
}

inline const cindex::Pack* find_pack(const cindex::Index& ix, const std::string& id) {
    for (const cindex::Pack& p : ix.packs)
        if (p.id == id) return &p;
    return nullptr;
}

// What happens to one mod: the text after its name and the colour kind (list::Seg: 1 secondary, 2 warning, 4 good).
struct OpText {
    std::wstring text;
    int kind = 0;
};
inline OpText op_text(const plan::Op& op) {
    const std::wstring v = browse::wide(op.version);
    switch (op.kind) {
        case plan::Kind::Keep: return {L"installed " + v, 4};
        case plan::Kind::Enable: return {L"installed " + v + L", will be enabled", 2};
        case plan::Kind::Disable: return {L"will be disabled", 2};
        case plan::Kind::Download: {
            const std::wstring size = op.size > 0 ? L" (" + browse::size_text(op.size) + L")" : L"";
            if (op.from.empty()) return {L"will download " + v + size, 2};
            // the installed copy is that version but Unverified (D7): it is fetched again
            if (browse::wide(op.from) == v) return {L"will download " + v + L" again, not verified" + size, 2};
            // ">" as on My mods: the game's font has no arrow glyph (replay mod-packs-pinned run 1)
            return {L"will change " + browse::wide(op.from) + L" > " + v + size, 2};
        }
    }
    return {};
}

inline std::wstring settings_text(const Plan& pl) {
    const std::wstring n = std::to_wstring(pl.settings) + (pl.settings_exact ? L"" : L" or more");
    return n + (pl.settings == 1 && pl.settings_exact ? L" setting changes" : L" settings change");
}

// The details under the header: authors, the mark and the mod count, the loader requirement, the description, then the plan.
// `name_of` gives a mod's display name (the catalog's or the installed one's).
template <typename WidthFn>
std::vector<list::InfoRow> info_rows(const cindex::Pack& p, const Plan& pl, const std::function<std::wstring(const std::string&)>& name_of,
                                     float max_width, WidthFn width_of, const std::wstring& stopped = L"") {
    std::vector<list::InfoRow> out;
    auto text_row = [&](const std::wstring& t, int kind) { out.push_back({{{{t, kind}}}, L"", {}}); };
    if (!stopped.empty()) {   // the last Apply modpack failed: the mod and the reason, above everything else
        out.push_back({{}, L"APPLY STOPPED:", {}});
        for (const std::wstring& l : text::wrap(stopped, max_width, width_of)) text_row(l, 3);
        out.push_back({{}, L"", {}});
    }
    text_row(p.authors.empty() ? L"No author given" : L"by " + browse::join(p.authors), 1);
    out.push_back({{{{L"Verified", 4}, {L"  -  ", 1}, {mods_text(p.mods.size()), 1}}}, L"", {}});
    text_row(L"Loader " + browse::wide(p.kyubu), 1);
    if (!p.description.empty()) {
        out.push_back({{}, L"", {}});
        for (const std::wstring& l : text::wrap(browse::utf8_wide(p.description), max_width, width_of)) text_row(l, 0);
    }
    out.push_back({{}, L"", {}});
    if (!pl.result.ok) {
        out.push_back({{}, L"CANNOT BE APPLIED:", {}});
        for (const std::wstring& l : text::wrap(browse::wide(pl.result.reason), max_width, width_of)) text_row(l, 3);
        out.push_back({{}, L"", {}});
        out.push_back({{}, L"PINS:", {}});
        for (const auto& [id, ver] : p.mods) text_row(name_of(id) + L"  " + browse::wide(ver), 0);
        return out;
    }
    out.push_back({{}, L"PLAN:", {}});
    for (const plan::Op& op : pl.result.ops) {
        const OpText t = op_text(op);
        const std::wstring name = name_of(op.id);
        if (width_of(name + L"  " + t.text) <= max_width) {
            out.push_back({{{{name, 0}, {L"  ", 0}, {t.text, t.kind}}}, L"", {}});
            continue;
        }
        text_row(name, 0);   // too long for one line: what happens goes under the name, indented
        for (const std::wstring& l : text::wrap(t.text, max_width - width_of(L"  "), width_of)) text_row(L"  " + l, t.kind);
    }
    for (const std::wstring& l : text::wrap(settings_text(pl), max_width, width_of)) text_row(l, pl.settings > 0 ? 2 : 1);
    if (p.reset_unlisted)
        for (const std::wstring& l : text::wrap(L"Options the pack does not name go back to their defaults.", max_width, width_of)) text_row(l, 1);
    return out;
}

}  // namespace packs
