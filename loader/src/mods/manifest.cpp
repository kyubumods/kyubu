#include "manifest.h"

#include "toml_config.h"

namespace kyubu {

bool valid_mod_id(const std::string& id) {
    if (id.size() < 2 || id.size() > 32) return false;
    if (!(id[0] >= 'a' && id[0] <= 'z')) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

uint32_t parse_sides(const std::vector<std::string>& names, bool* ok) {
    uint32_t m = 0;
    *ok = !names.empty();
    for (auto& n : names) {
        if (n == "client") m |= KY_SIDE_CLIENT;
        else if (n == "server") m |= KY_SIDE_SERVER;
        else *ok = false;
    }
    return m;
}

static std::vector<std::string> strings(toml::node_view<toml::node> n) {
    std::vector<std::string> out;
    if (auto a = n.as_array())
        for (auto& e : *a) if (auto s = e.value<std::string>()) out.push_back(*s);
    return out;
}

static bool parse_deps(toml::table* t, const char* key, std::vector<Dependency>& out, std::vector<std::string>& errors,
                       const std::string& src) {
    if (!t) return true;
    bool ok = true;
    for (auto& [k, v] : *t) {
        Dependency d;
        d.id = std::string(k.str());
        std::string req;
        if (auto s = v.value<std::string>()) req = *s;
        else if (auto dt = v.as_table()) {
            req = (*dt)["version"].value_or(std::string("*"));
            d.optional = (*dt)["optional"].value_or(false);
        } else {
            errors.push_back(src + ": key '" + key + "." + d.id + "': expected a version string or table");
            ok = false;
            continue;
        }
        auto r = VersionReq::parse(req);
        if (!r) {
            errors.push_back(src + ": key '" + key + "." + d.id + "': invalid version requirement '" + req + "'");
            ok = false;
            continue;
        }
        d.req = *r;
        out.push_back(d);
    }
    return ok;
}

std::optional<Manifest> Manifest::parse(const std::string& text, const std::string& src,
                                        std::vector<std::string>& errors) {
    auto res = toml::parse(text, src);
    if (!res) { errors.push_back(fmt_toml_error(res.error())); return std::nullopt; }
    auto& t = res.table();
    Manifest m;
    size_t before = errors.size();

    m.id = t["id"].value_or(std::string());
    if (m.id.empty()) errors.push_back(src + ": key 'id' is required");
    else if (!valid_mod_id(m.id))
        errors.push_back(src + ": key 'id': '" + m.id + "' must match ^[a-z][a-z0-9_]{1,31}$");

    m.name = t["name"].value_or(std::string());
    if (m.name.empty()) errors.push_back(src + ": key 'name' is required");

    std::string ver = t["version"].value_or(std::string());
    if (auto v = Version::parse(ver)) m.version = *v;
    else errors.push_back(src + ": key 'version': '" + ver + "' is not a semantic version (e.g. 1.0.0)");

    std::string api = t["kyubu"].value_or(std::string());
    if (auto r = VersionReq::parse(api)) m.api_req = *r;
    else errors.push_back(src + ": key 'kyubu': required loader API version range missing or invalid ('" + api + "')");

    bool sides_ok;
    m.sides = parse_sides(strings(t["sides"]), &sides_ok);
    if (!sides_ok) errors.push_back(src + ": key 'sides': expected a non-empty array of \"client\" and/or \"server\"");

    m.authors = strings(t["authors"]);
    m.description = t["description"].value_or(std::string());
    m.load_after = strings(t["load_after"]);
    m.load_before = strings(t["load_before"]);
    m.requires_server = t["requires_server"].value_or(false);
    m.assets = t["assets"].value_or(std::string("assets"));
    m.symbols = t["symbols"].value_or(std::string("symbols"));
    m.trust = t["trust"].value_or(std::string("full"));
    if (m.trust != "full" && m.trust != "restricted") errors.push_back(src + ": key 'trust': must be \"full\" or \"restricted\"");
    m.gate = t["gate"].value_or(false);
    m.manage_mods = t["manage_mods"].value_or(false);   // 1.2: an optional bool, false by default (add-mod-manager)

    parse_deps(t["dependencies"].as_table(), "dependencies", m.dependencies, errors, src);
    parse_deps(t["incompatible"].as_table(), "incompatible", m.incompatible, errors, src);

    for (const char* sect : {"native", "scripts"}) {
        if (auto nt = t[sect].as_table()) {
            for (auto& [k, v] : *nt) {
                std::string key(k.str());
                if (key != "client" && key != "server" && key != "common") {
                    errors.push_back(src + ": key '" + sect + "." + key + "': must be client, server or common");
                    continue;
                }
                auto s = v.value<std::string>();
                if (!s) { errors.push_back(src + ": key '" + sect + "." + key + "': expected a path"); continue; }
                (strcmp(sect, "native") == 0 ? m.native : m.scripts)[key] = *s;
            }
        }
    }
    if (errors.size() != before) return std::nullopt;
    return m;
}

}  // namespace kyubu
