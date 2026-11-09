#include "config.h"

#include <sstream>

#include "toml_config.h"

namespace kyubu {

Config Config::load(const std::string& path) {
    Config c;
    auto data = read_file(path);
    if (!data) return c;
    std::string text(data->begin(), data->end());
    auto res = toml::parse(text, path);
    if (!res) {
        c.errors.push_back(fmt_toml_error(res.error()));
        return c;
    }
    toml::table& t = res.table();
    c.dev_mode = t["loader"]["dev_mode"].value_or(false);
    c.asset_trace = t["loader"]["asset_trace"].value_or(false);
    c.log_level = t["loader"]["log_level"].value_or(std::string("info"));
    c.console_key = t["loader"]["console_key"].value_or(std::string("grave"));
    c.ability_placeholder = t["abilities"]["placeholder"].value_or(true);
    c.ability_skill_tree = t["abilities"]["skill_tree"].value_or(true);
    c.on_mod_crash = t["crash"]["on_mod_crash"].value_or(std::string("ask"));
    if (auto mods = t["mods"].as_table()) {
        for (auto& [k, v] : *mods) {
            if (auto mt = v.as_table()) {
                if (auto e = (*mt)["enabled"].value<bool>()) c.mod_enabled[std::string(k.str())] = *e;
            }
        }
    }
    return c;
}

bool Config::set_mod_enabled(const std::string& path, const std::string& id, bool enabled) {
    toml::table t;
    if (auto data = read_file(path)) {
        std::string text(data->begin(), data->end());
        auto res = toml::parse(text, path);
        if (!res) return false;
        t = std::move(res.table());
    }
    if (!t["mods"].as_table()) t.insert_or_assign("mods", toml::table{});
    auto mods = t["mods"].as_table();
    if (!(*mods)[id].as_table()) mods->insert_or_assign(id, toml::table{});
    (*mods)[id].as_table()->insert_or_assign("enabled", enabled);
    std::ostringstream os;
    os << t << "\n";
    return write_text(path, os.str());
}

}  // namespace kyubu
