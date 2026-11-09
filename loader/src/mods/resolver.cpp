#include "resolver.h"

#include <algorithm>
#include <map>
#include <set>

namespace kyubu {

const char* resolve_status_name(ResolveStatus s) {
    switch (s) {
        case ResolveStatus::Load: return "loaded";
        case ResolveStatus::Disabled: return "disabled";
        case ResolveStatus::Failed: return "failed";
        case ResolveStatus::SkippedSide: return "skipped (side)";
    }
    return "?";
}

const ResolvedMod* Resolution::find(const std::string& id) const {
    for (auto& m : mods) if (m.id == id && m.status != ResolveStatus::Failed) return &m;
    for (auto& m : mods) if (m.id == id) return &m;
    return nullptr;
}

Resolution resolve_mods(const std::vector<Candidate>& cands, const std::function<bool(const std::string&)>& enabled,
                        uint32_t host_sides, const Version& api) {
    Resolution r;
    std::map<std::string, std::vector<size_t>> by_id;
    for (size_t i = 0; i < cands.size(); ++i) {
        ResolvedMod m;
        m.path = cands[i].path;
        if (!cands[i].manifest) {
            m.status = ResolveStatus::Failed;
            m.reason = cands[i].errors.empty() ? "invalid mod.toml" : cands[i].errors.front();
            for (size_t k = 1; k < cands[i].errors.size(); ++k) m.reason += "; " + cands[i].errors[k];
        } else {
            m.manifest = &*cands[i].manifest;
            m.id = m.manifest->id;
            by_id[m.id].push_back(i);
        }
        r.mods.push_back(m);
    }
    // duplicate ids: none of them load
    for (auto& [id, idx] : by_id) {
        if (idx.size() < 2) continue;
        std::string paths;
        for (auto i : idx) paths += (paths.empty() ? "" : ", ") + cands[i].path;
        for (auto i : idx) { r.mods[i].status = ResolveStatus::Failed; r.mods[i].reason = "duplicate mod id '" + id + "' in: " + paths; }
    }
    for (auto& m : r.mods) {
        if (m.status != ResolveStatus::Load) continue;
        if (!enabled(m.id)) { m.status = ResolveStatus::Disabled; m.reason = "disabled in kyubu/config.toml"; continue; }
        if (!m.manifest->api_req.matches(api)) {
            m.status = ResolveStatus::Failed;
            m.reason = "requires loader API " + m.manifest->api_req.text() + " but this loader implements " + api.str();
        }
    }
    auto live = [&](const std::string& id) -> ResolvedMod* {
        for (auto& m : r.mods) if (m.id == id && (m.status == ResolveStatus::Load || m.status == ResolveStatus::SkippedSide)) return &m;
        return nullptr;
    };
    // incompatibilities (declared by the mod that refuses to load)
    for (auto& m : r.mods) {
        if (m.status != ResolveStatus::Load) continue;
        for (auto& inc : m.manifest->incompatible) {
            if (auto other = live(inc.id); other && inc.req.matches(other->manifest->version)) {
                m.status = ResolveStatus::Failed;
                m.reason = "incompatible with " + inc.id + " " + other->manifest->version.str() + " (declared " + inc.req.text() + ")";
                break;
            }
        }
    }
    // dependencies, cascading until nothing changes
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& m : r.mods) {
            if (m.status != ResolveStatus::Load) continue;
            for (auto& d : m.manifest->dependencies) {
                if (d.optional) continue;
                const ResolvedMod* dep = nullptr;
                for (auto& x : r.mods) if (x.id == d.id) { dep = &x; if (x.status == ResolveStatus::Load) break; }
                std::string why;
                if (!dep) why = m.id + " needs " + d.id + " " + d.req.text() + ", which is not installed";
                else if (dep->status == ResolveStatus::Disabled) why = m.id + " needs " + d.id + ", which is disabled";
                else if (dep->status == ResolveStatus::Failed) why = m.id + " needs " + d.id + ", which failed to load (" + dep->reason + ")";
                else if (!d.req.matches(dep->manifest->version))
                    why = m.id + " needs " + d.id + " " + d.req.text() + " but " + dep->manifest->version.str() + " is installed";
                if (!why.empty()) { m.status = ResolveStatus::Failed; m.reason = why; changed = true; break; }
            }
        }
    }
    // side filtering (after dependency checks so side-skipped mods still satisfy dependencies)
    for (auto& m : r.mods)
        if (m.status == ResolveStatus::Load && !(m.manifest->sides & host_sides)) {
            m.status = ResolveStatus::SkippedSide;
            m.reason = "no code for the sides of this host";
        }
    // ordering: Kahn's algorithm, ties broken by id; cycles fail their members and we retry
    for (;;) {
        std::set<std::string> nodes;
        for (auto& m : r.mods) if (m.status == ResolveStatus::Load) nodes.insert(m.id);
        std::map<std::string, std::set<std::string>> after;   // node -> must come after these
        auto edge = [&](const std::string& first, const std::string& then) {
            if (nodes.count(first) && nodes.count(then) && first != then) after[then].insert(first);
        };
        for (auto& m : r.mods) {
            if (m.status != ResolveStatus::Load) continue;
            for (auto& d : m.manifest->dependencies) edge(d.id, m.id);
            for (auto& a : m.manifest->load_after) edge(a, m.id);
            for (auto& b : m.manifest->load_before) edge(m.id, b);
        }
        std::vector<std::string> order;
        std::set<std::string> done;
        std::set<std::string> ready;
        for (auto& n : nodes) if (after[n].empty()) ready.insert(n);
        while (!ready.empty()) {
            std::string n = *ready.begin();
            ready.erase(ready.begin());
            order.push_back(n);
            done.insert(n);
            for (auto& o : nodes) {
                if (done.count(o) || ready.count(o)) continue;
                bool ok = true;
                for (auto& p : after[o]) if (!done.count(p)) { ok = false; break; }
                if (ok) ready.insert(o);
            }
        }
        if (order.size() == nodes.size()) { r.order = order; break; }
        // find one cycle among remaining nodes
        std::set<std::string> rest;
        for (auto& n : nodes) if (!done.count(n)) rest.insert(n);
        std::string start = *rest.begin();
        std::vector<std::string> path{start};
        std::map<std::string, size_t> pos{{start, 0}};
        std::string cur = start;
        std::vector<std::string> cycle;
        for (;;) {
            std::string next;
            for (auto& p : after[cur]) if (rest.count(p)) { next = p; break; }
            if (next.empty()) break;
            if (pos.count(next)) { cycle.assign(path.begin() + pos[next], path.end()); break; }
            pos[next] = path.size();
            path.push_back(next);
            cur = next;
        }
        if (cycle.empty()) cycle = {start};
        std::string desc;   // "a → b → a": a loads after b, b loads after a
        for (auto& c : cycle) desc += c + " \xE2\x86\x92 ";
        desc += cycle.front();
        for (auto& m : r.mods)
            if (m.status == ResolveStatus::Load && std::find(cycle.begin(), cycle.end(), m.id) != cycle.end()) {
                m.status = ResolveStatus::Failed;
                m.reason = "load order cycle: " + desc;
            }
        // cascade to dependants of the failed cycle members
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& m : r.mods) {
                if (m.status != ResolveStatus::Load) continue;
                for (auto& d : m.manifest->dependencies) {
                    if (d.optional) continue;
                    auto dep = r.find(d.id);
                    if (dep && dep->status == ResolveStatus::Failed) {
                        m.status = ResolveStatus::Failed;
                        m.reason = m.id + " needs " + d.id + ", which failed to load (" + dep->reason + ")";
                        changed = true;
                        break;
                    }
                }
            }
        }
    }
    return r;
}

}  // namespace kyubu
