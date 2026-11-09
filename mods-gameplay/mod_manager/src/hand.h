// Mods put into mods\ by hand (design D15): which ids the page has seen arrive by hand, so that they stay marked Unverified after
// the restart that loads them. No game calls here, so unit tests cover it. Until the catalog (section 7) says otherwise, a package is
// Unverified exactly when its id is in this set.
#pragma once

#include <set>
#include <string>

namespace hand {

constexpr const char* kReason = "Not from the catalog. Installed by hand.";

// One id per line; blank lines and anything that is not a mod id are ignored.
inline std::set<std::string> parse(const std::string& text) {
    std::set<std::string> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        bool ok = !line.empty() && line.size() <= 32;
        for (char c : line) ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
        if (ok) out.insert(line);
        at = end + 1;
    }
    return out;
}

inline std::string write(const std::set<std::string>& ids) {
    std::string out;
    for (const std::string& id : ids) out += id + "\n";
    return out;
}

}  // namespace hand
