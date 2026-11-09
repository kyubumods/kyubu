// The catalog side of the page (design D5, D9): the Install tab's list and details, the Verified / Unverified mark of an
// installed package, and the "update to" of a My mods row. Pure: the glue (main.cpp) passes in the index, the installed versions and
// the package hashes, so unit tests cover it.
#pragma once

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "index.h"
#include "list.h"
#include "mods/semver.h"
#include "plan.h"
#include "text.h"

namespace browse {

constexpr const char* kUnknownIdReason = "Not from the catalog. Installed by hand.";
constexpr const char* kChangedReason = "Changed since it was verified, or from another source.";
constexpr const wchar_t* kRejectedText = L"The catalog could not be verified.";

inline std::wstring wide(const std::string& s) {
    std::wstring w;
    for (unsigned char c : s) w.push_back(c);   // ids, versions and hashes are ASCII; names go through `utf8_wide`
    return w;
}

// UTF-8 to wide for the names and descriptions the signed index carries (no Windows call, so tests can use it).
inline std::wstring utf8_wide(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned cp = c;
        int n = 0;
        if (c >= 0xF0) { cp = c & 0x07; n = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; n = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; n = 1; }
        ++i;
        for (; n > 0 && i < s.size(); --n, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        w.push_back(cp > 0xFFFF ? L'?' : static_cast<wchar_t>(cp));
    }
    return w;
}

inline std::wstring size_text(long bytes) {
    wchar_t buf[24];
    if (bytes < 1024) std::swprintf(buf, 24, L"%ld B", bytes);
    else if (bytes < 1024 * 1024) std::swprintf(buf, 24, L"%ld KB", (bytes + 512) / 1024);
    else std::swprintf(buf, 24, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

inline bool satisfies(const std::string& version, const std::string& req) {
    auto v = kyubu::Version::parse(version);
    auto r = kyubu::VersionReq::parse(req);
    return v && r && r->matches(*v);
}

// The newest release in `m` that fits the loader's API; null when none does. Pre-releases are never offered.
inline const cindex::Version* latest_fit(const cindex::Mod& m, const std::string& api) {
    const cindex::Version* best = nullptr;
    kyubu::Version best_v;
    for (const cindex::Version& c : m.versions) {
        auto v = kyubu::Version::parse(c.version);
        if (!v || !v->pre.empty() || !satisfies(api, c.kyubu)) continue;
        if (!best || best_v < *v) { best = &c; best_v = *v; }
    }
    return best;
}

inline const cindex::Mod* find_mod(const cindex::Index& ix, const std::string& id) {
    for (const cindex::Mod& m : ix.mods)
        if (m.id == id) return &m;
    return nullptr;
}

// The catalog as the planner reads it.
inline std::vector<plan::CatalogMod> plan_catalog(const cindex::Index& ix) {
    std::vector<plan::CatalogMod> out;
    for (const cindex::Mod& m : ix.mods) {
        plan::CatalogMod c;
        c.id = m.id;
        for (const cindex::Version& v : m.versions) c.versions.push_back({v.version, v.kyubu, v.needs, v.size});
        out.push_back(std::move(c));
    }
    return out;
}

inline std::string hex(const unsigned char (&h)[32]) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (unsigned char b : h) { s.push_back(d[b >> 4]); s.push_back(d[b & 15]); }
    return s;
}

struct Verdict {
    list::Mark mark = list::Mark::Unknown;
    std::wstring reason;   // for Unverified
};

// D5's table. `ix` is null when no signed index is known: the mark stays Unknown ("Not checked").
inline Verdict mark_of(const cindex::Index* ix, const std::string& id, const std::string& sha256_hex) {
    Verdict v;
    if (!ix) return v;
    const cindex::Mod* m = find_mod(*ix, id);
    if (!m) { v.mark = list::Mark::Unverified; v.reason = wide(kUnknownIdReason); return v; }
    for (const cindex::Version& c : m->versions)
        if (c.sha256 == sha256_hex) { v.mark = list::Mark::Verified; return v; }
    v.mark = list::Mark::Unverified;
    v.reason = wide(kChangedReason);
    return v;
}

// The version a My mods row can update to: the newest that fits the API and is newer than the installed one; empty for none.
inline std::string update_for(const cindex::Index* ix, const std::string& id, const std::string& installed, const std::string& api) {
    if (!ix) return "";
    const cindex::Mod* m = find_mod(*ix, id);
    const cindex::Version* best = m ? latest_fit(*m, api) : nullptr;
    auto have = kyubu::Version::parse(installed);
    auto want = best ? kyubu::Version::parse(best->version) : std::nullopt;
    if (!want || !have || !(*have < *want)) return "";
    return best->version;
}

inline std::wstring join(const std::vector<std::string>& v) {
    std::wstring out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? L", " : L"") + utf8_wide(v[i]);
    return out;
}

inline std::wstring needs_text(const std::map<std::string, std::string>& needs) {
    std::wstring out;
    for (const auto& [id, req] : needs) out += (out.empty() ? L"" : L", ") + wide(id) + (req.empty() || req == "*" ? L"" : L" " + wide(req));
    return out;
}

// One row per catalog mod, in the index's order. `installed` maps an id to its installed version. A mod with no version that fits
// this loader is still listed (its version column says so); it just cannot be downloaded.
inline std::vector<list::Mod> catalog_mods(const cindex::Index& ix, const std::map<std::string, std::string>& installed, const std::string& api) {
    std::vector<list::Mod> out;
    for (const cindex::Mod& cm : ix.mods) {
        list::Mod m;
        m.id = wide(cm.id);
        m.name = cm.name.empty() ? m.id : utf8_wide(cm.name);
        m.authors = join(cm.authors);
        m.description = utf8_wide(cm.description);
        const cindex::Version* best = latest_fit(cm, api);
        m.version = best ? wide(best->version) : L"-";
        m.size_text = best ? size_text(best->size) : L"";
        std::vector<const cindex::Version*> order;
        for (const cindex::Version& v : cm.versions) order.push_back(&v);
        std::stable_sort(order.begin(), order.end(), [](const cindex::Version* a, const cindex::Version* b) {
            auto x = kyubu::Version::parse(a->version), y = kyubu::Version::parse(b->version);
            return x && y && *y < *x;
        });
        for (const cindex::Version* v : order) {
            list::Ver lv;
            lv.version = wide(v->version);
            lv.size = size_text(v->size);
            lv.published = wide(v->published);
            lv.changes = utf8_wide(v->changes);
            lv.needs = needs_text(v->needs);
            lv.fits = satisfies(api, v->kyubu);
            m.vers.push_back(std::move(lv));
        }
        auto it = installed.find(cm.id);
        if (it == installed.end()) m.offer = best ? list::Offer::Get : list::Offer::None;
        else {
            m.have = wide(it->second);
            const std::string up = update_for(&ix, cm.id, it->second, api);
            m.offer = up.empty() ? list::Offer::Installed : list::Offer::Update;
            m.update_to = wide(up);
        }
        out.push_back(std::move(m));
    }
    return out;
}

// The word at the right of a row.
inline const wchar_t* offer_label(list::Offer o) {
    switch (o) {
        case list::Offer::Get: return L"GET";
        case list::Offer::Update: return L"UPDATE";
        case list::Offer::Installed: return L"INSTALLED";
        case list::Offer::None: break;
    }
    return L"-";
}

// The details under the header: authors, what this player can do, the description, then the versions with their changes and needs.
template <typename WidthFn>
std::vector<list::InfoRow> info_rows(const list::Mod& m, float max_width, WidthFn width_of) {
    std::vector<list::InfoRow> out;
    auto text_row = [&](const std::wstring& t, int kind) { out.push_back({{{{t, kind}}}, L"", {}}); };
    text_row(m.authors.empty() ? L"No author given" : L"by " + m.authors, 1);
    std::wstring status;
    int kind = 1;
    switch (m.offer) {
        case list::Offer::Get: status = L"Not installed"; break;
        case list::Offer::Installed: status = L"Installed " + m.have; kind = 4; break;
        case list::Offer::Update: status = L"Installed " + m.have + L", update to " + m.update_to; kind = 2; break;
        case list::Offer::None: status = L"No version fits this loader"; kind = 3; break;
    }
    out.push_back({{{{L"Verified", 4}, {L"  -  ", 1}, {status, kind}}}, L"", {}});
    if (!m.description.empty()) {
        out.push_back({{}, L"", {}});
        for (const std::wstring& l : text::wrap(m.description, max_width, width_of)) text_row(l, 0);
    }
    out.push_back({{}, L"", {}});
    out.push_back({{}, L"VERSIONS:", {}});
    for (const list::Ver& v : m.vers) {
        std::wstring head = v.version + L"   " + v.size;
        if (!v.published.empty()) head += L"   " + v.published;
        text_row(head, v.fits ? 0 : 1);
        if (!v.fits) text_row(L"  needs a newer loader", 3);
        if (!v.changes.empty())
            for (const std::wstring& l : text::wrap(L"  " + v.changes, max_width, width_of)) text_row(l, 1);
        if (!v.needs.empty())
            for (const std::wstring& l : text::wrap(L"  needs " + v.needs, max_width, width_of)) text_row(l, 1);
    }
    return out;
}

// Rows of the Actions menu of a catalog mod (download.h does the download itself).
struct Menu {
    bool download_ok = false;
    std::wstring download_label, tip;
};

inline Menu menu_of(const list::Mod& m, bool online) {
    Menu r;
    switch (m.offer) {
        case list::Offer::Get: r.download_label = L"Download " + m.version; r.download_ok = true; break;
        case list::Offer::Update: r.download_label = L"Update to " + m.update_to; r.download_ok = true; break;
        case list::Offer::Installed: r.download_label = L"Installed"; r.tip = L"The newest version is installed."; break;
        case list::Offer::None: r.download_label = L"Download"; r.tip = L"No version fits this loader."; break;
    }
    if (r.download_ok && !online) { r.download_ok = false; r.tip = L"The catalog is offline. Downloads wait for it."; }
    return r;
}

// The catalog line at the top of the page: "catalog: online", or why there is no list.
inline std::wstring banner(cindex::State st) {
    switch (st) {
        case cindex::State::Online: return L"CATALOG: ONLINE";
        case cindex::State::Offline: return L"CATALOG: OFFLINE";
        case cindex::State::Rejected: return L"CATALOG: NOT VERIFIED";
        case cindex::State::None: break;
    }
    return L"CATALOG: NOT LOADED";
}

}  // namespace browse
