#pragma once
#include "common.h"
#include "semver.h"

#include <map>

namespace kyubu {

struct Dependency {
    std::string id;
    VersionReq req;
    bool optional = false;
};

struct Manifest {
    std::string id;
    std::string name;
    Version version;
    VersionReq api_req;
    uint32_t sides = 0;                 // KyubuSide bit mask
    std::vector<std::string> authors;
    std::string description;
    std::vector<Dependency> dependencies;
    std::vector<Dependency> incompatible;
    std::vector<std::string> load_after, load_before;
    bool requires_server = false;
    std::map<std::string, std::string> native;    // client/server/common -> relative path
    std::map<std::string, std::string> scripts;   // client/server/common -> relative path
    std::string assets = "assets";
    std::string symbols = "symbols";
    std::string trust = "full";
    bool gate = false;                   // a gate mod: its id and version feed the join gate (add-skills-unleashed-multiplayer D4)
    bool manage_mods = false;            // 1.2: may call the mod-management members (API 1.2), full trust also required

    // Parses mod.toml text. On failure returns nullopt and appends "key: message" errors.
    static std::optional<Manifest> parse(const std::string& text, const std::string& source,
                                         std::vector<std::string>& errors);
};

bool valid_mod_id(const std::string& id);
uint32_t parse_sides(const std::vector<std::string>& names, bool* ok);

}  // namespace kyubu
