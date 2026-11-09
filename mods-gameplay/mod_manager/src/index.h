// The catalog index (design D4, D5): parse index.json, check its Ed25519 signature, and decide which copy to trust
// (the fetched one, or the saved one when the catalog is out of reach). No Windows calls, so unit tests cover it.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"
#include "keys.h"
#include "monocypher-ed25519.h"

namespace cindex {

struct Version {
    std::string version, kyubu = "*", url, sha256, published, changes;
    std::vector<std::string> sides;
    std::map<std::string, std::string> needs;
    long size = 0;
};

struct Mod {
    std::string id, name, description;
    std::vector<std::string> authors;
    std::vector<Version> versions;
};

struct Pack {
    std::string id, name, version, description, kyubu = "*";
    std::vector<std::string> authors;
    std::map<std::string, std::string> mods;   // id -> exact version
    std::string config;                        // the "config" object as JSON text
    bool reset_unlisted = true;
};

struct Index {
    int format = 0;
    std::string generated, name;
    std::vector<Mod> mods;
    std::vector<Pack> packs;
};

struct Parsed {
    bool ok = false;
    std::string error;
    Index index;
};

namespace detail {
using json = nlohmann::json;

inline bool is_id(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

inline bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// A package address inside the catalog: relative, forward slashes, no "..", no drive or scheme.
inline bool is_rel_url(const std::string& s) {
    if (s.empty() || s.size() > 200 || s[0] == '/' || s.find('\\') != std::string::npos || s.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();
        const std::string part = s.substr(i, j - i);
        if (part.empty() || part == "." || part == "..") return false;
        i = j + 1;
    }
    return true;
}

inline std::string str(const json& o, const char* k, const std::string& def = "") {
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : def;
}

inline std::vector<std::string> strs(const json& o, const char* k) {
    std::vector<std::string> v;
    auto it = o.find(k);
    if (it != o.end() && it->is_array())
        for (const auto& e : *it)
            if (e.is_string()) v.push_back(e.get<std::string>());
    return v;
}

inline bool str_map(const json& o, const char* k, std::map<std::string, std::string>& out) {
    auto it = o.find(k);
    if (it == o.end()) return true;
    if (!it->is_object()) return false;
    for (auto e = it->begin(); e != it->end(); ++e) {
        if (!e.value().is_string() || !is_id(e.key())) return false;
        out[e.key()] = e.value().get<std::string>();
    }
    return true;
}
}  // namespace detail

// Reads the index. A malformed entry rejects the whole index: a signed file from kcat should never have one.
inline Parsed parse(const std::string& text) {
    using namespace detail;
    Parsed r;
    auto fail = [&](const std::string& why) { r.ok = false; r.error = why; r.index = Index(); return r; };
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return fail("index.json is not valid JSON");
    auto f = doc.find("format");
    if (f == doc.end() || !f->is_number_integer()) return fail("index.json has no format number");
    r.index.format = f->get<int>();
    if (r.index.format != 1) return fail("index format " + std::to_string(r.index.format) + " is not supported");
    r.index.generated = str(doc, "generated");
    r.index.name = str(doc, "name");
    std::map<std::string, bool> seen;
    auto mods = doc.find("mods");
    if (mods != doc.end()) {
        if (!mods->is_array()) return fail("\"mods\" is not a list");
        for (const auto& m : *mods) {
            if (!m.is_object()) return fail("a mod entry is not an object");
            Mod mod;
            mod.id = str(m, "id");
            if (!is_id(mod.id)) return fail("bad mod id \"" + mod.id + "\"");
            if (seen[mod.id]) return fail("mod " + mod.id + " is listed twice");
            seen[mod.id] = true;
            mod.name = str(m, "name", mod.id);
            mod.description = str(m, "description");
            mod.authors = strs(m, "authors");
            auto vs = m.find("versions");
            if (vs == m.end() || !vs->is_array() || vs->empty()) return fail("mod " + mod.id + " has no versions");
            std::map<std::string, bool> vseen;
            for (const auto& v : *vs) {
                if (!v.is_object()) return fail("a version of " + mod.id + " is not an object");
                Version ver;
                ver.version = str(v, "version");
                if (ver.version.empty() || ver.version.size() > 40) return fail("bad version of " + mod.id);
                if (vseen[ver.version]) return fail(mod.id + " " + ver.version + " is listed twice");
                vseen[ver.version] = true;
                ver.kyubu = str(v, "kyubu", "*");
                ver.url = str(v, "url");
                ver.sha256 = str(v, "sha256");
                ver.published = str(v, "published");
                ver.changes = str(v, "changes");
                ver.sides = strs(v, "sides");
                auto sz = v.find("size");
                if (sz == v.end() || !sz->is_number_integer() || sz->get<long long>() <= 0 || sz->get<long long>() > 0x7fffffffLL)
                    return fail("bad size for " + mod.id + " " + ver.version);
                ver.size = sz->get<long>();
                if (!is_rel_url(ver.url)) return fail("bad url for " + mod.id + " " + ver.version);
                if (!is_hex64(ver.sha256)) return fail("bad sha256 for " + mod.id + " " + ver.version);
                if (!str_map(v, "dependencies", ver.needs)) return fail("bad dependencies for " + mod.id + " " + ver.version);
                mod.versions.push_back(ver);
            }
            r.index.mods.push_back(mod);
        }
    }
    auto packs = doc.find("packs");
    if (packs != doc.end()) {
        if (!packs->is_array()) return fail("\"packs\" is not a list");
        std::map<std::string, bool> pseen;
        for (const auto& p : *packs) {
            if (!p.is_object()) return fail("a pack entry is not an object");
            Pack pk;
            pk.id = str(p, "id");
            if (!is_id(pk.id)) return fail("bad pack id \"" + pk.id + "\"");
            if (pseen[pk.id]) return fail("pack " + pk.id + " is listed twice");
            pseen[pk.id] = true;
            pk.name = str(p, "name", pk.id);
            pk.version = str(p, "version");
            pk.description = str(p, "description");
            pk.kyubu = str(p, "kyubu", "*");
            pk.authors = strs(p, "authors");
            if (!str_map(p, "mods", pk.mods) || pk.mods.empty()) return fail("bad mods in pack " + pk.id);
            auto c = p.find("config");
            if (c != p.end()) {
                if (!c->is_object()) return fail("bad config in pack " + pk.id);
                pk.config = c->dump();
            }
            auto ru = p.find("reset_unlisted");
            if (ru != p.end()) {
                if (!ru->is_boolean()) return fail("bad reset_unlisted in pack " + pk.id);
                pk.reset_unlisted = ru->get<bool>();
            }
            r.index.packs.push_back(pk);
        }
    }
    r.ok = true;
    return r;
}

// True when `sig` (64 bytes) is a valid Ed25519 signature of exactly `data` under one of `trusted`.
inline bool verify(const std::string& data, const std::string& sig, const std::vector<keys::Key>& trusted) {
    if (sig.size() != 64) return false;
    for (const auto& k : trusted)
        if (crypto_ed25519_check(reinterpret_cast<const uint8_t*>(sig.data()), k.data(),
                                 reinterpret_cast<const uint8_t*>(data.data()), data.size()) == 0)
            return true;
    return false;
}

// What one attempt to reach the catalog brought.
struct Fetched {
    bool reached = false;       // the server (or folder) answered
    bool not_modified = false;  // 304: the saved copy is current
    std::string index, sig, etag;
};

enum class State { None, Online, Offline, Rejected };

struct Loaded {
    State state = State::None;
    std::string message;     // what the page says
    Index index;             // Online and Offline only
    bool save = false;       // write `index`, `sig` and `etag` below as the new saved copy
    std::string index_text, sig_text, etag;
};

// The decision (spec "Signed index"): a reached catalog is used only when its signature holds and its index parses, else the whole
// index is rejected and the saved copy is not replaced; an unreachable catalog falls back to the saved copy, checked again.
inline Loaded resolve(const Fetched& net, const std::string& saved_index, const std::string& saved_sig, const std::string& saved_etag,
                      const std::vector<keys::Key>& trusted) {
    Loaded out;
    auto from_saved = [&](State st) {
        if (saved_index.empty() || !verify(saved_index, saved_sig, trusted)) return false;
        Parsed p = parse(saved_index);
        if (!p.ok) return false;
        out.state = st;
        out.index = p.index;
        return true;
    };
    if (net.reached && net.not_modified) {
        if (from_saved(State::Online)) return out;
        out.state = State::None;
        out.message = "The saved copy of the catalog is damaged.";
        return out;
    }
    if (net.reached) {
        if (!verify(net.index, net.sig, trusted)) {
            out.state = State::Rejected;
            out.message = "The catalog could not be verified.";
            return out;
        }
        Parsed p = parse(net.index);
        if (!p.ok) {
            out.state = State::Rejected;
            out.message = "The catalog could not be read: " + p.error + ".";
            return out;
        }
        out.state = State::Online;
        out.index = p.index;
        out.save = true;
        out.index_text = net.index;
        out.sig_text = net.sig;
        out.etag = net.etag;
        return out;
    }
    if (from_saved(State::Offline)) return out;
    out.state = State::None;
    out.message = "The catalog could not be reached.";
    (void)saved_etag;
    return out;
}

}  // namespace cindex
