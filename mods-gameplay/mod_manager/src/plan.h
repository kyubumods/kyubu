// The planner (design D8): from the installed mods, the catalog, the loader's API version and a goal it returns the ops to
// queue, or the reason it cannot. No Windows or game calls, so unit tests cover it. It picks a version only for a mod the goal
// names without one (the newest that fits the API and the requirements); a pack's versions are exact and never chosen here.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace plan {

struct Installed {
    std::string id, version;
    bool enabled = false;
    bool verified = false;   // its hash is that version's in the catalog index (design D5)
    std::map<std::string, std::string> needs;   // required dependencies: id -> requirement ("*" for any)
};

struct CatalogVersion {
    std::string version;
    std::string kyubu = "*";   // the loader API requirement of this version
    std::map<std::string, std::string> needs;
    long size = 0;
};

struct CatalogMod {
    std::string id;
    std::vector<CatalogVersion> versions;
};

struct World {
    std::vector<Installed> installed;
    std::vector<CatalogMod> catalog;
    std::string api;                    // the loader's API version, "1.2.0"
    std::string self = "mod_manager";   // never disabled or replaced by a plan
};

enum class Kind { Keep, Download, Enable, Disable };

struct Op {
    Kind kind = Kind::Keep;
    std::string id;
    std::string version;   // Download: the version to fetch; Keep, Enable, Disable: the installed one
    std::string from;      // Download: the installed version it replaces, empty for a new mod
    long size = 0;         // Download
    bool also = false;     // Disable: a dependent taken along, not the goal itself
};

struct Result {
    bool ok = false;
    std::string reason;                       // when !ok
    std::vector<Op> ops;                      // when ok: dependencies before dependents; Disable: dependents first
    std::vector<std::string> also_disables;   // ids of the Disable ops that are `also`
};

// Install (or enable) these mods with the dependencies they need. A Download is enabled by the caller when it is queued.
Result install(const World& w, const std::vector<std::string>& ids);
// Disable one mod and every enabled mod that needs it.
Result disable(const World& w, const std::string& id);

struct Pin {
    std::string id, version;
};
// Apply a pack: exactly these versions, every other enabled mod except `self` disabled.
Result apply_pack(const World& w, const std::vector<Pin>& pins);

}  // namespace plan
