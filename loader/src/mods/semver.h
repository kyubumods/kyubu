#pragma once
#include <optional>
#include <string>
#include <vector>

namespace kyubu {

struct Version {
    int major = 0, minor = 0, patch = 0;
    std::string pre;   // pre-release tag (sorts before the release)
    static std::optional<Version> parse(const std::string& s);
    std::string str() const;
    int compare(const Version& o) const;
    bool operator<(const Version& o) const { return compare(o) < 0; }
    bool operator==(const Version& o) const { return compare(o) == 0; }
};

// Cargo-style requirement: "*", "1.2.3" (= ^1.2.3), "^1.2", "~1.2.3", ">=1.0 <2.0", ">=1.0, <2.0", "=1.2.3"
class VersionReq {
public:
    static std::optional<VersionReq> parse(const std::string& s);
    bool matches(const Version& v) const;
    const std::string& text() const { return text_; }

private:
    struct Cmp { enum Op { Eq, Gt, Ge, Lt, Le } op; Version v; };
    std::vector<Cmp> cmps_;
    std::string text_;
};

}  // namespace kyubu
