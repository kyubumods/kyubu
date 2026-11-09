#pragma once
#include "common.h"

#include <map>

namespace kyubu {

// kyubu/config.toml
//   [loader]  dev_mode = false, asset_trace = false, log_level = "info", console_key = "grave"
//   [abilities] placeholder = true   (false: slot 4 stays empty unless a mod claims it)
//               skill_tree = true    (the skill tree teaches the fourth ability, whose rank is the character's skills[9]; false: rank 1 from the start, from the ranks file)
//   [crash]   on_mod_crash = "ask" | "disable" | "ignore"
//   [mods.<id>] enabled = true
struct Config {
    bool dev_mode = false;
    bool asset_trace = false;
    std::string log_level = "info";
    std::string console_key = "grave";
    std::string on_mod_crash = "ask";
    bool ability_placeholder = true;
    bool ability_skill_tree = true;
    std::map<std::string, bool> mod_enabled;   // explicit entries only
    std::vector<std::string> errors;           // parse problems, logged at boot

    bool is_enabled(const std::string& id) const {
        auto it = mod_enabled.find(id);
        return it == mod_enabled.end() ? true : it->second;
    }

    static Config load(const std::string& path);
    // Rewrites only [mods.<id>] enabled in the file, preserving everything else.
    static bool set_mod_enabled(const std::string& path, const std::string& id, bool enabled);
};

}  // namespace kyubu
