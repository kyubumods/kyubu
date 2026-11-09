// The collected changes of the My mods tab (design D2, D8, D14): what the Actions menu offers for a mod, what a click on a row
// collects (enable, disable with the mods that need it, delete), the ops that Apply queues, and the record that Undo last apply reads.
// No game calls here, so unit tests cover it. The glue (main.cpp) turns the ops into mods_queue calls and the options into
// mods_option_set calls.
#pragma once

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "mods/semver.h"
#include "plan.h"

namespace actions {

// What the planner does not know about a package. The backup name of a deleted one is "<id>-<version>", plus ".zip" for a zip (D2).
struct Pkg {
    std::string version;
    bool zip = false;
    bool problem = false;       // failed, crashed, skipped or missing: it cannot be switched on from here
    bool has_options = false;
    bool options_changed = false;   // some option differs from its default
};

// A download collected by the Install tab: staged in kyubu\staging\ by the worker, installed at the next boot.
struct Dl {
    std::string version;
    std::string prev;                             // the installed version it replaces, empty for a new mod
    std::map<std::string, std::string> needs;     // of the catalog version
    long size = 0;
    int group = 0;                                // the Download action that asked for it
};

struct Collected {
    std::map<std::string, bool> enable;   // id -> the wanted state, only where it differs from the installed one
    std::set<std::string> del;
    std::map<std::string, Dl> dl;         // id -> the download
    int next_group = 1;
    int count() const { return static_cast<int>(enable.size() + del.size() + dl.size()); }
    void clear() { enable.clear(); del.clear(); dl.clear(); }
};

inline const plan::Installed* find(const plan::World& w, const std::string& id) {
    for (const plan::Installed& m : w.installed) if (m.id == id) return &m;
    return nullptr;
}

// The installed mods as they would be after the collected changes; a collected download counts as installed and on.
inline plan::World effective(const plan::World& w, const Collected& c) {
    plan::World e = w;
    for (plan::Installed& m : e.installed) {
        auto it = c.enable.find(m.id);
        if (it != c.enable.end()) m.enabled = it->second;
    }
    for (const auto& [id, d] : c.dl) {
        plan::Installed* m = nullptr;
        for (plan::Installed& x : e.installed) if (x.id == id) m = &x;
        if (!m) { e.installed.push_back(plan::Installed()); m = &e.installed.back(); m->id = id; }
        m->version = d.version;
        m->enabled = true;
        m->verified = true;
        m->needs = d.needs;
    }
    return e;
}

inline bool wanted(const plan::World& w, const Collected& c, const std::string& id) {
    auto it = c.enable.find(id);
    if (it != c.enable.end()) return it->second;
    const plan::Installed* m = find(w, id);
    return m && m->enabled;
}

inline void set_wanted(const plan::World& w, Collected& c, const std::string& id, bool on) {
    const plan::Installed* m = find(w, id);
    if (m && m->enabled == on) c.enable.erase(id);
    else c.enable[id] = on;
}

struct Outcome {
    bool ok = false;
    std::string reason;              // when !ok
    std::vector<std::string> also;   // the other mods switched with it
};

// Switches a mod: off together with every enabled mod that needs it, on together with the installed mods it needs.
inline Outcome toggle(const plan::World& w, Collected& c, const std::string& id) {
    Outcome o;
    const plan::World e = effective(w, c);
    const bool turn_off = wanted(w, c, id);
    const plan::Result r = turn_off ? plan::disable(e, id) : plan::install(e, {id});
    if (!r.ok) { o.reason = r.reason; return o; }
    for (const plan::Op& op : r.ops)
        if (op.kind == plan::Kind::Download) { o.reason = op.id + " " + op.version + " has to be downloaded first."; return o; }
    for (const plan::Op& op : r.ops) {
        if (op.kind == plan::Kind::Disable) set_wanted(w, c, op.id, false);
        else if (op.kind == plan::Kind::Enable) set_wanted(w, c, op.id, true);
        else continue;
        if (op.id != id) o.also.push_back(op.id);
    }
    o.ok = true;
    return o;
}

// Marks a mod for deletion; a mod that is on is switched off first, with the mods that need it.
inline Outcome remove(const plan::World& w, Collected& c, const std::string& id) {
    Outcome o;
    if (id == w.self) { o.reason = id + " cannot be deleted."; return o; }
    if (wanted(w, c, id)) {
        o = toggle(w, c, id);
        if (!o.ok) return o;
    }
    c.del.insert(id);
    o.ok = true;
    return o;
}

inline void cancel_delete(Collected& c, const std::string& id) { c.del.erase(id); }

// Collects the download of `id` with the dependencies it is missing: the newest version that fits, or, for an installed
// mod, its update. Installed dependencies that are off are switched on. `fetch` receives the downloads to start, dependencies first.
inline Outcome download(const plan::World& w, Collected& c, const std::string& id, std::vector<plan::Op>& fetch) {
    Outcome o;
    fetch.clear();
    if (c.dl.count(id)) { o.reason = id + " is already downloaded."; return o; }
    plan::World e = effective(w, c);
    std::string prev;
    for (size_t i = 0; i < e.installed.size(); ++i)
        if (e.installed[i].id == id) { prev = e.installed[i].version; e.installed.erase(e.installed.begin() + static_cast<long>(i)); break; }
    const plan::Result r = plan::install(e, {id});
    if (!r.ok) { o.reason = r.reason; return o; }
    bool goal = false;
    for (const plan::Op& op : r.ops) if (op.id == id && op.kind == plan::Kind::Download) goal = true;
    if (!goal) { o.reason = id + " has nothing newer to download."; return o; }
    if (!prev.empty()) {   // an update must be newer than what is installed
        for (const plan::Op& op : r.ops) {
            if (op.id != id) continue;
            const auto have = kyubu::Version::parse(prev), want = kyubu::Version::parse(op.version);
            if (!have || !want || !(*have < *want)) { o.reason = id + " has nothing newer to download."; return o; }
        }
    }
    const int group = c.next_group++;
    for (const plan::Op& op : r.ops) {
        if (op.kind == plan::Kind::Enable) {
            set_wanted(w, c, op.id, true);
            o.also.push_back(op.id);
        }
        if (op.kind != plan::Kind::Download) continue;
        Dl d;
        d.version = op.version;
        d.prev = op.id == id ? prev : op.from;
        d.size = op.size;
        d.group = group;
        for (const plan::CatalogMod& m : w.catalog)
            if (m.id == op.id)
                for (const plan::CatalogVersion& v : m.versions)
                    if (v.version == op.version) d.needs = v.needs;
        c.dl[op.id] = d;
        c.del.erase(op.id);
        c.enable.erase(op.id);
        fetch.push_back(op);
        if (op.id != id) o.also.push_back(op.id);
    }
    o.ok = true;
    return o;
}

// Drops the downloads of one Download action (a failed or cancelled one). Returns their ids.
inline std::vector<std::string> drop_group(Collected& c, int group) {
    std::vector<std::string> ids;
    for (auto it = c.dl.begin(); it != c.dl.end();) {
        if (it->second.group == group) { ids.push_back(it->first); it = c.dl.erase(it); }
        else ++it;
    }
    return ids;
}

enum class Act { Toggle, ResetOptions, Delete, Undo, Download, ShowInMyMods, ApplyPack, Restore, DeleteProfile };
struct Row {
    Act act = Act::Toggle;
    std::string label;
    int kind = 0;       // 0 plain, 1 grey (a tooltip says why), 2 danger
    std::string tip;
};

// The Actions menu of the selected mod. `undo` says that a last-apply record exists.
inline std::vector<Row> menu(const plan::World& w, const Collected& c, const std::string& id, const Pkg& pkg, bool undo) {
    std::vector<Row> rows;
    const bool self = id == w.self;
    const bool deleted = c.del.count(id) != 0;
    const bool on = wanted(w, c, id);
    Row t;
    t.act = Act::Toggle;
    t.label = on ? "Disable" : "Enable";
    if (self) { t.kind = 1; t.tip = "It cannot switch itself off."; }
    else if (deleted) { t.kind = 1; t.tip = "It is marked for deletion."; }
    else if (!on && pkg.problem) { t.kind = 1; t.tip = "It did not load; see the reason in the details."; }
    rows.push_back(t);
    Row r;
    r.act = Act::ResetOptions;
    r.label = "Reset options";
    if (!pkg.has_options) { r.kind = 1; r.tip = "It has no options."; }
    else if (!pkg.options_changed) { r.kind = 1; r.tip = "Every option has its default."; }
    rows.push_back(r);
    Row d;
    d.act = Act::Delete;
    d.label = deleted ? "Keep it" : "Delete";
    d.kind = deleted ? 0 : 2;
    if (self) { d.kind = 1; d.tip = "It cannot delete itself."; }
    rows.push_back(d);
    Row u;
    u.act = Act::Undo;
    u.label = "Undo last apply";
    if (!undo) { u.kind = 1; u.tip = "Nothing to undo."; }
    rows.push_back(u);
    return rows;
}

// --- Apply ---------------------------------------------------------------------------------------------------------------------

struct Queued {
    enum Kind { Enable, Disable, Delete, Install, Config } kind = Enable;
    std::string id, version, from;   // Install: the version and the staged or backup path ("kyubu/backup/<id>-<version>.zip")
    std::string prev;                // Install from staging: the installed version it replaces, empty for a new mod
    bool was_on = false;             // Install from staging: the replaced version was on
    std::string key, value;          // Config: one option, written at boot after the installs
    bool optional = false;           // Config: a refusal is logged and skipped (a reset of a key the new version may not have)
};

inline std::string staged_from(const std::string& id, const std::string& version) { return "kyubu/staging/" + id + "-" + version + ".zip"; }
inline bool from_staging(const Queued& q) { return q.kind == Queued::Install && q.from.compare(0, 14, "kyubu/staging/") == 0; }

// The ops Apply queues, and the ops that undo them. A deleted mod gets one Delete (its Disable is not queued); the undo of a delete
// installs the package back from kyubu\backup\ (settings are kept by the delete, so nothing else is needed).
inline std::vector<Queued> ops(const plan::World& w, const Collected& c, const std::map<std::string, Pkg>& pkgs) {
    (void)pkgs;
    std::vector<Queued> out;
    for (const auto& [id, on] : c.enable) {
        if (c.del.count(id)) continue;
        const plan::Installed* m = find(w, id);
        if (m && m->enabled == on) continue;
        Queued q;
        q.kind = on ? Queued::Enable : Queued::Disable;
        q.id = id;
        out.push_back(q);
    }
    for (const std::string& id : c.del) {
        Queued q;
        q.kind = Queued::Delete;
        q.id = id;
        out.push_back(q);
    }
    // Downloads: install the staged zip, and switch it on unless the version it replaces is on already.
    for (const auto& [id, d] : c.dl) {
        const plan::Installed* m = find(w, id);
        Queued q;
        q.kind = Queued::Install;
        q.id = id;
        q.version = d.version;
        q.from = staged_from(id, d.version);
        q.prev = d.prev;
        q.was_on = m && m->enabled;
        out.push_back(q);
        if (!q.was_on) {
            Queued e;
            e.kind = Queued::Enable;
            e.id = id;
            out.push_back(e);
        }
    }
    return out;
}

inline std::vector<Queued> reverse(const std::vector<Queued>& done, const std::map<std::string, Pkg>& pkgs) {
    std::vector<Queued> out;
    std::set<std::string> new_installs;   // their undo deletes them, so their Enable needs no undo
    for (const Queued& q : done)
        if (from_staging(q) && q.prev.empty()) new_installs.insert(q.id);
    for (const Queued& q : done) {
        Queued r;
        r.id = q.id;
        if (from_staging(q)) {
            if (q.prev.empty()) r.kind = Queued::Delete;
            else {   // the loader moved the replaced package into kyubu\backup\ as "<id>-<version>"
                auto it = pkgs.find(q.id);
                r.kind = Queued::Install;
                r.version = q.prev;
                r.from = "kyubu/backup/" + q.id + "-" + q.prev + (it == pkgs.end() || it->second.zip ? ".zip" : "");
                if (!q.was_on) {
                    out.push_back(r);
                    r = Queued();
                    r.id = q.id;
                    r.kind = Queued::Disable;
                }
            }
            out.push_back(r);
            continue;
        }
        if (q.kind == Queued::Enable && new_installs.count(q.id)) continue;
        if (q.kind == Queued::Enable) r.kind = Queued::Disable;
        else if (q.kind == Queued::Disable) r.kind = Queued::Enable;
        else if (q.kind == Queued::Delete) {
            auto it = pkgs.find(q.id);
            if (it == pkgs.end()) continue;
            r.kind = Queued::Install;
            r.version = it->second.version;
            r.from = "kyubu/backup/" + q.id + "-" + it->second.version + (it->second.zip ? ".zip" : "");
        } else continue;
        out.push_back(r);
    }
    return out;
}

struct OptionUndo { std::string id, key, value; };   // the value in use before Apply

// The undo record: one line per undo op ("enable id", "disable id", "install id version from") or option ("option id key value").
inline std::string write_record(const std::vector<Queued>& undo_ops, const std::vector<OptionUndo>& options) {
    std::ostringstream os;
    for (const Queued& q : undo_ops) {
        if (q.kind == Queued::Enable) os << "enable " << q.id << "\n";
        else if (q.kind == Queued::Disable) os << "disable " << q.id << "\n";
        else if (q.kind == Queued::Install) os << "install " << q.id << " " << q.version << " " << q.from << "\n";
        else if (q.kind == Queued::Delete) os << "delete " << q.id << "\n";
    }
    for (const OptionUndo& o : options) os << "option " << o.id << " " << o.key << " " << o.value << "\n";
    return os.str();
}

inline void read_record(const std::string& text, std::vector<Queued>& undo_ops, std::vector<OptionUndo>& options) {
    undo_ops.clear();
    options.clear();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string word;
        ls >> word;
        Queued q;
        if (word == "enable" || word == "disable" || word == "delete") {
            q.kind = word == "enable" ? Queued::Enable : (word == "disable" ? Queued::Disable : Queued::Delete);
            ls >> q.id;
            if (!q.id.empty()) undo_ops.push_back(q);
        } else if (word == "install") {
            q.kind = Queued::Install;
            ls >> q.id >> q.version >> q.from;
            if (!q.id.empty() && !q.from.empty()) undo_ops.push_back(q);
        } else if (word == "option") {
            OptionUndo o;
            ls >> o.id >> o.key;
            std::getline(ls, o.value);
            if (!o.value.empty() && o.value.front() == ' ') o.value.erase(0, 1);
            if (!o.id.empty() && !o.key.empty()) options.push_back(o);
        }
    }
}

}  // namespace actions
