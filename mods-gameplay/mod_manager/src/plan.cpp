#include "plan.h"

#include <functional>
#include <set>

#include "mods/semver.h"

namespace plan {
namespace {

using kyubu::Version;
using kyubu::VersionReq;

const Installed* find_installed(const World& w, const std::string& id) {
    for (const Installed& m : w.installed)
        if (m.id == id) return &m;
    return nullptr;
}

const CatalogMod* find_catalog(const World& w, const std::string& id) {
    for (const CatalogMod& m : w.catalog)
        if (m.id == id) return &m;
    return nullptr;
}

bool same_version(const std::string& a, const std::string& b) {
    auto x = Version::parse(a);
    auto y = Version::parse(b);
    return x && y ? *x == *y : a == b;
}

const CatalogVersion* find_version(const CatalogMod& m, const std::string& v) {
    for (const CatalogVersion& c : m.versions)
        if (same_version(c.version, v)) return &c;
    return nullptr;
}

bool satisfies(const std::string& version, const std::string& req) {
    auto v = Version::parse(version);
    auto r = VersionReq::parse(req);
    return v && r && r->matches(*v);
}

bool api_fits(const World& w, const CatalogVersion& v) { return satisfies(w.api, v.kyubu); }

Result fail(std::string why) {
    Result r;
    r.reason = std::move(why);
    return r;
}

// The newest release in the catalog that fits the API and `req`; null when none does.
const CatalogVersion* newest_fit(const World& w, const CatalogMod& m, const std::string& req) {
    const CatalogVersion* best = nullptr;
    Version best_v;
    for (const CatalogVersion& c : m.versions) {
        auto v = Version::parse(c.version);
        if (!v || !v->pre.empty() || !api_fits(w, c) || !satisfies(c.version, req)) continue;
        if (!best || best_v < *v) {
            best = &c;
            best_v = *v;
        }
    }
    return best;
}

}  // namespace

Result install(const World& w, const std::vector<std::string>& ids) {
    Result out;
    std::set<std::string> done, active;
    std::string error;
    // `req` is what `parent` asks of `id` ("*" for a goal). Post-order: dependencies come before dependents.
    std::function<bool(const std::string&, const std::string&, const std::string&)> visit =
        [&](const std::string& id, const std::string& req, const std::string& parent) {
            if (done.count(id)) {
                for (const Op& op : out.ops)
                    if (op.id == id && !satisfies(op.version, req)) {
                        error = parent + " needs " + id + " " + req + ", but " + op.version + " is planned";
                        return false;
                    }
                return true;
            }
            if (active.count(id)) {
                error = "dependency cycle through " + id;
                return false;
            }
            active.insert(id);
            const Installed* have = find_installed(w, id);
            const CatalogMod* cat = find_catalog(w, id);
            const std::string by = parent.empty() ? "" : " (needed by " + parent + ")";
            Op op;
            op.id = id;
            std::map<std::string, std::string> needs;
            if (have && satisfies(have->version, req)) {
                op.kind = have->enabled ? Kind::Keep : Kind::Enable;
                op.version = have->version;
                needs = have->needs;
            } else {
                if (!cat) {
                    error = have ? id + " " + have->version + " is installed, but " + req + " is needed" + by
                                 : "no mod " + id + " in the catalog" + by;
                    return false;
                }
                const CatalogVersion* v = newest_fit(w, *cat, req);
                if (!v) {
                    error = "no version of " + id + " fits " + req + " and loader API " + w.api + by;
                    return false;
                }
                op.kind = Kind::Download;
                op.version = v->version;
                op.size = v->size;
                if (have) op.from = have->version;
                needs = v->needs;
            }
            for (const auto& [dep, dreq] : needs)
                if (!visit(dep, dreq, id)) return false;
            active.erase(id);
            done.insert(id);
            out.ops.push_back(op);
            return true;
        };
    for (const std::string& id : ids)
        if (!visit(id, "*", "")) return fail(error);
    out.ok = true;
    return out;
}

Result disable(const World& w, const std::string& id) {
    if (id == w.self) return fail(id + " cannot be disabled");
    if (!find_installed(w, id)) return fail("no mod " + id + " is installed");
    Result out;
    std::set<std::string> seen;
    std::function<void(const std::string&)> walk = [&](const std::string& what) {
        if (!seen.insert(what).second) return;
        for (const Installed& m : w.installed)
            if (m.enabled && m.needs.count(what)) walk(m.id);   // dependents first
        const Installed* i = find_installed(w, what);
        if (!i || !i->enabled) return;
        Op op;
        op.kind = Kind::Disable;
        op.id = what;
        op.version = i->version;
        op.also = what != id;
        out.ops.push_back(op);
        if (op.also) out.also_disables.push_back(what);
    };
    walk(id);
    for (const std::string& a : out.also_disables)
        if (a == w.self) return fail(w.self + " needs " + id + " and cannot be disabled");
    out.ok = true;
    return out;
}

Result apply_pack(const World& w, const std::vector<Pin>& pins) {
    Result out;
    std::map<std::string, std::string> pinned;
    for (const Pin& p : pins) pinned[p.id] = p.version;
    std::map<std::string, std::map<std::string, std::string>> needs;   // pinned id -> its requirements
    for (const Pin& p : pins) {
        const Installed* have = find_installed(w, p.id);
        Op op;
        op.id = p.id;
        if (have && have->verified && same_version(have->version, p.version)) {
            op.kind = have->enabled ? Kind::Keep : Kind::Enable;
            op.version = have->version;
            needs[p.id] = have->needs;
        } else {
            const CatalogMod* cat = find_catalog(w, p.id);
            const CatalogVersion* v = cat ? find_version(*cat, p.version) : nullptr;
            if (!v) return fail(p.id + " " + p.version + " is not in the catalog");
            if (!api_fits(w, *v)) return fail(p.id + " " + p.version + " needs loader API " + v->kyubu + ", this is " + w.api);
            op.kind = Kind::Download;
            op.version = v->version;
            op.size = v->size;
            if (have) op.from = have->version;
            needs[p.id] = v->needs;
        }
        out.ops.push_back(op);
    }
    for (const auto& [id, reqs] : needs)
        for (const auto& [dep, req] : reqs) {
            auto it = pinned.find(dep);
            if (it == pinned.end()) return fail(id + " needs " + dep + " " + req + ", which the pack does not list");
            if (!satisfies(it->second, req)) return fail(id + " needs " + dep + " " + req + ", the pack pins " + it->second);
        }
    for (const Installed& m : w.installed) {
        if (m.id == w.self || pinned.count(m.id) || !m.enabled) continue;
        Op op;
        op.kind = Kind::Disable;
        op.id = m.id;
        op.version = m.version;
        out.ops.push_back(op);
    }
    out.ok = true;
    return out;
}

}  // namespace plan
