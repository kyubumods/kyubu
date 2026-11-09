// Pure dependency resolution and load ordering (no I/O) so it can be unit tested exhaustively.
#pragma once
#include "manifest.h"

namespace kyubu {

struct Candidate {
    std::string path;                 // folder or zip
    std::optional<Manifest> manifest;
    std::vector<std::string> errors;  // manifest problems
};

enum class ResolveStatus { Load, Disabled, Failed, SkippedSide };
const char* resolve_status_name(ResolveStatus s);

struct ResolvedMod {
    std::string id;           // may be empty for broken manifests
    std::string path;
    ResolveStatus status = ResolveStatus::Load;
    std::string reason;
    const Manifest* manifest = nullptr;
};

struct Resolution {
    std::vector<ResolvedMod> mods;       // one per candidate, in discovery order
    std::vector<std::string> order;      // ids with status Load, in load order
    const ResolvedMod* find(const std::string& id) const;
};

Resolution resolve_mods(const std::vector<Candidate>& candidates, const std::function<bool(const std::string&)>& enabled,
                        uint32_t host_sides, const Version& api_version);

}  // namespace kyubu
