// The checks of a downloaded zip before it is staged (design D6): mod.toml at the root or in one top-level folder, with the
// id and version the index promised; no entry with an absolute path, a drive or a ".." part; the unpacked size under 512 MB. Pure
// (miniz only), so unit tests cover it. The loader checks the id and version again when it installs the zip at boot.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "miniz.h"

namespace zipcheck {

constexpr uint64_t kMaxUnpacked = 512ull << 20;
constexpr size_t kMaxManifest = 64u << 10;

struct Result {
    bool ok = false;
    std::string reason;   // when !ok, one sentence for the player
};

// A zip entry name that stays inside the folder it is unpacked to: relative, no drive, no "." or ".." part.
inline bool safe_name(const std::string& n) {
    if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i < n.size()) {
        size_t j = n.find_first_of("/\\", i);
        if (j == std::string::npos) j = n.size();
        const std::string part = n.substr(i, j - i);
        if (part == ".." || part == ".") return false;
        if (part.empty() && j != n.size()) return false;   // "a//b"
        i = j + 1;
    }
    return true;
}

// The value of a top-level `key = "value"` of a TOML text (before the first table). Enough for id and version.
inline std::string top_string(const std::string& text, const std::string& key) {
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#') continue;
        if (line[a] == '[') break;
        if (line.compare(a, key.size(), key) != 0) continue;
        size_t b = line.find_first_not_of(" \t", a + key.size());
        if (b == std::string::npos || line[b] != '=') continue;
        b = line.find_first_not_of(" \t", b + 1);
        if (b == std::string::npos || (line[b] != '"' && line[b] != '\'')) return "";
        const char q = line[b];
        const size_t c = line.find(q, b + 1);
        return c == std::string::npos ? "" : line.substr(b + 1, c - b - 1);
    }
    return "";
}

inline Result fail(const std::string& why) {
    Result r;
    r.reason = why;
    return r;
}

// Checks an opened archive. `id` and `version` are what the index promised.
inline Result check_archive(mz_zip_archive& z, const std::string& id, const std::string& version) {
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    if (n == 0) return fail("The download is an empty zip.");
    uint64_t total = 0;
    std::string top;          // the one top-level folder, when every entry is under it
    bool one_top = true;
    int root_manifest = -1, top_manifest = -1;
    std::vector<std::string> names;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) return fail("The download is not a readable zip.");
        const std::string name = st.m_filename;
        if (!safe_name(name)) return fail("The zip has an entry outside its folder: " + name);
        total += st.m_uncomp_size;
        if (total >= kMaxUnpacked) return fail("The zip unpacks to more than 512 MB.");
        const size_t slash = name.find_first_of("/\\");
        if (slash == std::string::npos) {
            one_top = false;   // a file at the root
            if (name == "mod.toml") root_manifest = static_cast<int>(i);
        } else {
            const std::string first = name.substr(0, slash);
            if (top.empty()) top = first;
            else if (first != top) one_top = false;
            const std::string rest = name.substr(slash + 1);
            if (rest == "mod.toml") top_manifest = static_cast<int>(i);
        }
    }
    int manifest = root_manifest;
    if (manifest < 0 && one_top) manifest = top_manifest;
    if (manifest < 0) return fail("The zip has no mod.toml at its root or in one top-level folder.");
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&z, static_cast<mz_uint>(manifest), &st) || st.m_uncomp_size > kMaxManifest)
        return fail("The zip's mod.toml is too large.");
    size_t size = 0;
    void* p = mz_zip_reader_extract_to_heap(&z, static_cast<mz_uint>(manifest), &size, 0);
    if (!p) return fail("The zip's mod.toml cannot be read.");
    const std::string text(static_cast<const char*>(p), size);
    mz_free(p);
    const std::string got_id = top_string(text, "id"), got_version = top_string(text, "version");
    if (got_id != id) return fail("The zip holds '" + got_id + "', not '" + id + "'.");
    if (got_version != version) return fail("The zip holds version " + got_version + ", not " + version + ".");
    Result r;
    r.ok = true;
    return r;
}

inline Result check_memory(const void* data, size_t size, const std::string& id, const std::string& version) {
    mz_zip_archive z = {};
    if (!mz_zip_reader_init_mem(&z, data, size, 0)) return fail("The download is not a readable zip.");
    Result r = check_archive(z, id, version);
    mz_zip_reader_end(&z);
    return r;
}

inline Result check_file(const std::string& path, const std::string& id, const std::string& version) {
    mz_zip_archive z = {};
    if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) return fail("The download is not a readable zip.");
    Result r = check_archive(z, id, version);
    mz_zip_reader_end(&z);
    return r;
}

}  // namespace zipcheck
