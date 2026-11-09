#include "semver.h"

#include <cctype>
#include <cstdlib>

namespace kyubu {

static bool parse_int(const std::string& s, int* out) {
    if (s.empty() || s.size() > 9) return false;
    for (char c : s) if (!isdigit((unsigned char)c)) return false;
    *out = atoi(s.c_str());
    return true;
}

std::optional<Version> Version::parse(const std::string& in) {
    std::string s = in;
    Version v;
    size_t dash = s.find('-');
    size_t plus = s.find('+');
    if (plus != std::string::npos) s = s.substr(0, plus);
    if (dash != std::string::npos) { v.pre = s.substr(dash + 1); s = s.substr(0, dash); }
    std::vector<std::string> parts;
    size_t st = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '.') { parts.push_back(s.substr(st, i - st)); st = i + 1; }
    if (parts.empty() || parts.size() > 3) return std::nullopt;
    int* f[3] = {&v.major, &v.minor, &v.patch};
    for (size_t i = 0; i < parts.size(); ++i) if (!parse_int(parts[i], f[i])) return std::nullopt;
    return v;
}

std::string Version::str() const {
    std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (!pre.empty()) s += "-" + pre;
    return s;
}

int Version::compare(const Version& o) const {
    if (major != o.major) return major < o.major ? -1 : 1;
    if (minor != o.minor) return minor < o.minor ? -1 : 1;
    if (patch != o.patch) return patch < o.patch ? -1 : 1;
    if (pre == o.pre) return 0;
    if (pre.empty()) return 1;       // release > pre-release
    if (o.pre.empty()) return -1;
    return pre < o.pre ? -1 : 1;
}

std::optional<VersionReq> VersionReq::parse(const std::string& in) {
    VersionReq r;
    r.text_ = in;
    std::string s;
    for (char c : in) s += (c == ',') ? ' ' : c;
    std::vector<std::string> toks;
    std::string cur;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ' ') { if (!cur.empty()) toks.push_back(cur); cur.clear(); }
        else cur += s[i];
    }
    if (toks.empty()) return std::nullopt;
    // Join operators separated from their versions by spaces (">= 1.0").
    std::vector<std::string> merged;
    for (size_t i = 0; i < toks.size(); ++i) {
        const std::string& t = toks[i];
        if ((t == ">=" || t == "<=" || t == ">" || t == "<" || t == "=" || t == "^" || t == "~") && i + 1 < toks.size())
            merged.push_back(t + toks[++i]);
        else merged.push_back(t);
    }
    for (auto& t : merged) {
        if (t == "*") continue;
        std::string op, ver;
        size_t k = 0;
        while (k < t.size() && (t[k] == '>' || t[k] == '<' || t[k] == '=' || t[k] == '^' || t[k] == '~')) ++k;
        op = t.substr(0, k);
        ver = t.substr(k);
        // count given components to build caret/tilde bounds
        int comps = 1;
        for (char c : ver) if (c == '.') ++comps;
        auto v = Version::parse(ver);
        if (!v) return std::nullopt;
        if (op.empty() || op == "^") {
            Version upper;
            if (v->major > 0 || comps == 1) upper = Version{v->major + 1, 0, 0, ""};
            else if (v->minor > 0 || comps == 2) upper = Version{0, v->minor + 1, 0, ""};
            else upper = Version{0, 0, v->patch + 1, ""};
            r.cmps_.push_back({Cmp::Ge, *v});
            r.cmps_.push_back({Cmp::Lt, upper});
        } else if (op == "~") {
            Version upper = comps == 1 ? Version{v->major + 1, 0, 0, ""} : Version{v->major, v->minor + 1, 0, ""};
            r.cmps_.push_back({Cmp::Ge, *v});
            r.cmps_.push_back({Cmp::Lt, upper});
        } else if (op == "=") r.cmps_.push_back({Cmp::Eq, *v});
        else if (op == ">") r.cmps_.push_back({Cmp::Gt, *v});
        else if (op == ">=") r.cmps_.push_back({Cmp::Ge, *v});
        else if (op == "<") r.cmps_.push_back({Cmp::Lt, *v});
        else if (op == "<=") r.cmps_.push_back({Cmp::Le, *v});
        else return std::nullopt;
    }
    return r;
}

bool VersionReq::matches(const Version& v) const {
    for (auto& c : cmps_) {
        int d = v.compare(c.v);
        switch (c.op) {
            case Cmp::Eq: if (d != 0) return false; break;
            case Cmp::Gt: if (d <= 0) return false; break;
            case Cmp::Ge: if (d < 0) return false; break;
            case Cmp::Lt: if (d >= 0) return false; break;
            case Cmp::Le: if (d > 0) return false; break;
        }
    }
    return true;
}

}  // namespace kyubu
