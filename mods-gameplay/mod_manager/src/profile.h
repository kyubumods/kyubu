// Applying a modpack and going back (design D7). Pure, so unit tests cover it; the glue (main.cpp) downloads, queues and
// restarts.
//
// - `pack_work` turns a pack's plan into what Apply modpack does: the queue (installs from kyubu\staging\, enables, disables, and the
//   options of a downloaded mod as config ops, which the loader writes at boot after the installs) and the options of the mods that
//   stay installed, set at once with mods_option_set. With reset_unlisted the options the pack does not name go back to their defaults.
// - A local profile has the shape of a pack (cindex::Pack): the enabled mods with their versions, and every option value in use.
//   `capture` makes "Before <pack>"; `to_json` / `from_json` save and read it in kyubu\data\mod_manager\profiles\<id>.json.
// - `restore_plan` and `restore_work` put a profile back with what is still on disk: an installed mod of another version comes back
//   from kyubu\backup\ when the loader kept it there, else the installed version stays.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "actions.h"
#include "browse.h"
#include "form.h"
#include "index.h"
#include "json.hpp"
#include "list.h"
#include "packs.h"
#include "plan.h"
#include "text.h"

namespace profile {

struct OptSet {
    std::string id, key, value;
};

// What an apply does: the queue (in this order) and the options set at once, before the restart.
struct Work {
    std::vector<actions::Queued> queue;
    std::vector<OptSet> live;
    bool empty() const { return queue.empty() && live.empty(); }
};

inline actions::Queued simple(actions::Queued::Kind k, const std::string& id) {
    actions::Queued q;
    q.kind = k;
    q.id = id;
    return q;
}

inline actions::Queued config_op(const std::string& id, const std::string& key, const std::string& value, bool optional) {
    actions::Queued q = simple(actions::Queued::Config, id);
    q.key = key;
    q.value = value;
    q.optional = optional;
    return q;
}

inline const plan::Installed* installed(const plan::World& w, const std::string& id) { return actions::find(w, id); }

// The options of a mod that stays installed: the named values that differ from the ones in use, and with `reset` every other option
// that is not at its default. Without known fields every named value is set (the loader refuses an unknown key; the glue logs it).
inline void live_options(const std::string& id, const std::map<std::string, std::string>& named, bool reset,
                         const std::vector<form::Field>* fields, std::vector<OptSet>& out) {
    if (!fields) {
        for (const auto& [k, v] : named) out.push_back({id, k, v});
        return;
    }
    std::map<std::string, bool> seen;
    for (const form::Field& f : *fields) {
        auto n = named.find(f.key);
        if (n != named.end()) {
            seen[f.key] = true;
            if (!form::same(f.type, n->second, f.value)) out.push_back({id, f.key, n->second});
        } else if (reset && !form::same(f.type, f.value, f.def)) {
            out.push_back({id, f.key, f.def});
        }
    }
    for (const auto& [k, v] : named)
        if (!seen.count(k)) out.push_back({id, k, v});
}

// Apply modpack: the plan's ops as the queue, and the pack's options. A downloaded mod gets its options as config ops after its
// install; with reset_unlisted the options of the version it replaces that are off their default are reset too, as optional ops
// (the new version may not declare them). `fields_of` gives the option fields of an installed mod.
inline Work pack_work(const cindex::Pack& p, const plan::Result& r, const plan::World& w, const packs::FieldsOf& fields_of) {
    Work out;
    const auto cfg = packs::config_of(p);
    const std::map<std::string, std::string> none;
    for (const plan::Op& op : r.ops) {
        const auto ci = cfg.find(op.id);
        const std::map<std::string, std::string>& named = ci == cfg.end() ? none : ci->second;
        switch (op.kind) {
            case plan::Kind::Disable: out.queue.push_back(simple(actions::Queued::Disable, op.id)); break;
            case plan::Kind::Enable:
                out.queue.push_back(simple(actions::Queued::Enable, op.id));
                live_options(op.id, named, p.reset_unlisted, fields_of(op.id), out.live);
                break;
            case plan::Kind::Keep: live_options(op.id, named, p.reset_unlisted, fields_of(op.id), out.live); break;
            case plan::Kind::Download: {
                const plan::Installed* m = installed(w, op.id);
                actions::Queued q = simple(actions::Queued::Install, op.id);
                q.version = op.version;
                q.from = actions::staged_from(op.id, op.version);
                q.prev = m ? m->version : "";
                q.was_on = m && m->enabled;
                out.queue.push_back(q);
                if (!q.was_on) out.queue.push_back(simple(actions::Queued::Enable, op.id));
                for (const auto& [k, v] : named) out.queue.push_back(config_op(op.id, k, v, false));
                if (p.reset_unlisted && m)
                    if (const std::vector<form::Field>* fields = fields_of(op.id))
                        for (const form::Field& f : *fields)
                            if (!named.count(f.key) && !form::same(f.type, f.value, f.def)) out.queue.push_back(config_op(op.id, f.key, f.def, true));
                break;
            }
        }
    }
    return out;
}

// Nothing to do: every pinned mod is kept as it is, nothing is disabled and no option changes.
inline bool in_use(const plan::Result& r, const Work& w) {
    for (const plan::Op& op : r.ops)
        if (op.kind != plan::Kind::Keep) return false;
    return w.empty();
}

// --- local profiles ---------------------------------------------------------------------------------------------------------------

inline std::string before_id(const std::string& pack_id) { return "before_" + pack_id; }

// The current setup as a profile: every enabled mod except `self`, and every option value in use of every installed mod.
inline cindex::Pack capture(const plan::World& w, const packs::FieldsOf& fields_of, const std::string& id, const std::string& name,
                            const std::string& description) {
    cindex::Pack p;
    p.id = id;
    p.name = name;
    p.description = description;
    p.version = "1.0.0";
    p.reset_unlisted = false;   // it names every value, so nothing is reset
    nlohmann::json cfg = nlohmann::json::object();
    for (const plan::Installed& m : w.installed) {
        if (m.id == w.self) continue;
        if (m.enabled) p.mods[m.id] = m.version;
        if (const std::vector<form::Field>* fields = fields_of(m.id))
            for (const form::Field& f : *fields) cfg[m.id][f.key] = f.value;
    }
    p.config = cfg.empty() ? "" : cfg.dump();
    return p;
}

inline std::string to_json(const cindex::Pack& p) {
    nlohmann::json j;
    j["id"] = p.id;
    j["name"] = p.name;
    j["description"] = p.description;
    j["version"] = p.version;
    j["mods"] = p.mods;
    j["config"] = p.config.empty() ? nlohmann::json::object() : nlohmann::json::parse(p.config, nullptr, false);
    if (j["config"].is_discarded()) j["config"] = nlohmann::json::object();
    j["reset_unlisted"] = p.reset_unlisted;
    return j.dump(2) + "\n";
}

// False when the text is not a profile (a file the player broke): it is then not listed.
inline bool from_json(const std::string& text, cindex::Pack& p) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    p = cindex::Pack();
    p.id = cindex::detail::str(j, "id");
    if (!cindex::detail::is_id(p.id)) return false;
    p.name = cindex::detail::str(j, "name", p.id);
    p.description = cindex::detail::str(j, "description");
    p.version = cindex::detail::str(j, "version");
    if (j.contains("mods") && !cindex::detail::str_map(j, "mods", p.mods)) return false;
    auto c = j.find("config");
    if (c != j.end()) {
        if (!c->is_object()) return false;
        if (!c->empty()) p.config = c->dump();
    }
    auto ru = j.find("reset_unlisted");
    if (ru != j.end() && ru->is_boolean()) p.reset_unlisted = ru->get<bool>();
    return true;
}

// --- Restore ----------------------------------------------------------------------------------------------------------------------

enum class St { Keep, Enable, Disable, Backup, Stays, Gone };

struct Step {
    St st = St::Keep;
    std::string id, version;   // the profile's version (Disable: the installed one)
    std::string have;          // the installed version, empty when none
    std::string from;          // Backup: "kyubu/backup/<id>-<version>[.zip]"
};

// The path of a kept package of that version in kyubu\backup\, or "" when the loader kept none.
using BackupOf = std::function<std::string(const std::string& id, const std::string& version)>;

inline std::vector<Step> restore_plan(const cindex::Pack& prof, const plan::World& w, const BackupOf& backup_of) {
    std::vector<Step> out;
    for (const plan::Installed& m : w.installed) {
        if (m.id == w.self) continue;
        Step s;
        s.id = m.id;
        s.have = m.version;
        auto want = prof.mods.find(m.id);
        if (want == prof.mods.end()) {
            if (!m.enabled) continue;
            s.st = St::Disable;
            s.version = m.version;
        } else {
            s.version = want->second;
            if (m.version == want->second) s.st = m.enabled ? St::Keep : St::Enable;
            else {
                s.from = backup_of(m.id, want->second);
                s.st = s.from.empty() ? St::Stays : St::Backup;
            }
        }
        out.push_back(s);
    }
    for (const auto& [id, ver] : prof.mods) {
        if (id == w.self || installed(w, id)) continue;
        Step s;
        s.id = id;
        s.version = ver;
        s.from = backup_of(id, ver);
        s.st = s.from.empty() ? St::Gone : St::Backup;
        out.push_back(s);
    }
    return out;
}

// The queue and the options of a Restore. A package that comes back from the backup gets its options as config ops; an installed
// one gets the values that differ from the ones in use (every value when its fields are not known). A Gone mod has none.
inline Work restore_work(const cindex::Pack& prof, const std::vector<Step>& steps, const plan::World& w, const packs::FieldsOf& fields_of) {
    Work out;
    const auto cfg = packs::config_of(prof);
    std::map<std::string, St> st_of;
    for (const Step& s : steps) {
        st_of[s.id] = s.st;
        const plan::Installed* m = installed(w, s.id);
        switch (s.st) {
            case St::Keep: break;
            case St::Enable: out.queue.push_back(simple(actions::Queued::Enable, s.id)); break;
            case St::Disable: out.queue.push_back(simple(actions::Queued::Disable, s.id)); break;
            case St::Stays:
                if (m && !m->enabled) out.queue.push_back(simple(actions::Queued::Enable, s.id));
                break;
            case St::Backup: {
                actions::Queued q = simple(actions::Queued::Install, s.id);
                q.version = s.version;
                q.from = s.from;
                q.prev = s.have;
                q.was_on = m && m->enabled;
                out.queue.push_back(q);
                if (!q.was_on) out.queue.push_back(simple(actions::Queued::Enable, s.id));
                break;
            }
            case St::Gone: break;
        }
    }
    for (const auto& [id, values] : cfg) {
        if (id == w.self) continue;
        auto st = st_of.find(id);
        if (st != st_of.end() && st->second == St::Gone) continue;
        if (st != st_of.end() && st->second == St::Backup) {
            for (const auto& [k, v] : values) out.queue.push_back(config_op(id, k, v, true));
            continue;
        }
        if (!installed(w, id)) continue;   // deleted since the profile was saved
        live_options(id, values, false, fields_of(id), out.live);
    }
    return out;
}

// The text after a mod's name in a profile's plan (list::Seg kinds: 1 secondary, 2 warning, 3 error, 4 good).
inline packs::OpText step_text(const Step& s) {
    const std::wstring v = browse::wide(s.version), have = browse::wide(s.have);
    switch (s.st) {
        case St::Keep: return {L"installed " + v, 4};
        case St::Enable: return {L"installed " + v + L", will be enabled", 2};
        case St::Disable: return {L"will be disabled", 2};
        case St::Backup: return {(s.have.empty() ? L"will come back " + v : L"will change " + have + L" > " + v) + L" from the backup", 2};
        case St::Stays: return {L"keeps " + have + L": " + v + L" is no longer kept", 3};
        case St::Gone: return {v + L" is no longer kept, it stays out", 3};
    }
    return {};
}

inline int settings_of(const Work& w) {
    int n = static_cast<int>(w.live.size());
    for (const actions::Queued& q : w.queue) n += q.kind == actions::Queued::Config ? 1 : 0;
    return n;
}

// One row per profile, after the packs; its id is "profile:<id>", so it never matches a pack.
inline const wchar_t* kRowPrefix = L"profile:";
inline std::vector<list::Mod> profile_rows(const std::vector<cindex::Pack>& profiles) {
    std::vector<list::Mod> out;
    for (const cindex::Pack& p : profiles) {
        list::Mod m;
        m.id = kRowPrefix + browse::wide(p.id);
        m.name = browse::utf8_wide(p.name);
        m.description = browse::utf8_wide(p.description);
        m.size_text = L"PROFILE";
        m.mark = list::Mark::Unknown;
        out.push_back(std::move(m));
    }
    return out;
}

template <typename WidthFn>
std::vector<list::InfoRow> info_rows(const cindex::Pack& prof, const std::vector<Step>& steps, const Work& work,
                                     const std::function<std::wstring(const std::string&)>& name_of, float max_width, WidthFn width_of) {
    std::vector<list::InfoRow> out;
    auto text_row = [&](const std::wstring& t, int kind) { out.push_back({{{{t, kind}}}, L"", {}}); };
    text_row(L"Local profile", 1);
    text_row(packs::mods_text(prof.mods.size()), 1);
    if (!prof.description.empty()) {
        out.push_back({{}, L"", {}});
        for (const std::wstring& l : text::wrap(browse::utf8_wide(prof.description), max_width, width_of)) text_row(l, 0);
    }
    out.push_back({{}, L"", {}});
    out.push_back({{}, L"RESTORE:", {}});
    for (const Step& s : steps) {
        const packs::OpText t = step_text(s);
        const std::wstring name = name_of(s.id);
        if (width_of(name + L"  " + t.text) <= max_width) {
            out.push_back({{{{name, 0}, {L"  ", 0}, {t.text, t.kind}}}, L"", {}});
            continue;
        }
        text_row(name, 0);
        for (const std::wstring& l : text::wrap(t.text, max_width - width_of(L"  "), width_of)) text_row(L"  " + l, t.kind);
    }
    const int n = settings_of(work);
    text_row(std::to_wstring(n) + (n == 1 ? L" setting is set back" : L" settings are set back"), n > 0 ? 2 : 1);
    return out;
}

}  // namespace profile
