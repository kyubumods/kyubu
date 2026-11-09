// Downloads (design D6): one worker thread fetches the queued packages one at a time into
// kyubu\staging\<id>-<version>.zip.part, from an https:// catalog with WinHTTP or from a file:/// folder (tests), hashing as it writes.
// The part file is renamed to <id>-<version>.zip only when its size and SHA-256 equal the index's and the zip passes zipcheck;
// otherwise it is deleted and the item says why. Cancel deletes the part file and every staged file of the list.
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/sha256.h"
#include "fetch.h"
#include "zipcheck.h"

namespace download {

constexpr const char* kMismatch = "The download did not match the catalog.";

enum class St { Waiting, Running, Staged, Failed };

struct Item {
    std::string id, version, url, sha256;   // url: absolute (https:// or file:///)
    long size = 0;
    int group = 0;                          // the Download action it belongs to (a mod and its dependencies)
    St st = St::Waiting;
    long got = 0;
    std::string reason;                     // when Failed
    bool cancelled = false;                 // Running when cancel_all ran: the worker deletes its files
};

struct Shared {
    CRITICAL_SECTION cs;
    bool inited = false, thread = false;
    std::vector<Item> items;
    std::string staging;                    // the full path of kyubu\staging
};

inline Shared& shared() {
    static Shared s;
    return s;
}

inline std::string part_path(const std::string& dir, const Item& it) { return dir + "\\" + it.id + "-" + it.version + ".zip.part"; }
inline std::string zip_path(const std::string& dir, const Item& it) { return dir + "\\" + it.id + "-" + it.version + ".zip"; }
// The `from` of the install op (game-relative, '/').
inline std::string staged_rel(const std::string& id, const std::string& version) { return "kyubu/staging/" + id + "-" + version + ".zip"; }

// Progress of the running item, read by the game thread; the worker's checks of `cancelled` go through the lock too.
inline void set_got(const std::string& id, long got) {
    Shared& s = shared();
    EnterCriticalSection(&s.cs);
    for (Item& it : s.items) if (it.id == id && it.st == St::Running) it.got = got;
    LeaveCriticalSection(&s.cs);
}

inline bool is_cancelled(const std::string& id) {
    Shared& s = shared();
    EnterCriticalSection(&s.cs);
    bool c = true;
    for (const Item& it : s.items) if (it.id == id && it.st == St::Running) c = it.cancelled;
    LeaveCriticalSection(&s.cs);
    return c;
}

// Writes the body into `f`, hashing it. Fails past `limit` bytes or when cancelled.
struct Sink {
    FILE* f = nullptr;
    kyubu::Sha256 sha;
    long got = 0, limit = 0;
    bool over = false;   // more bytes than the index's size
    std::string id;
    bool put(const void* p, size_t n) {
        if (got + static_cast<long>(n) > limit) { over = true; return false; }
        if (std::fwrite(p, 1, n, f) != n) return false;
        sha.update(p, n);
        got += static_cast<long>(n);
        set_got(id, got);
        return !is_cancelled(id);
    }
};

inline bool from_file(const std::string& path, Sink& out) {
    FILE* in = std::fopen(path.c_str(), "rb");
    if (!in) return false;
    char buf[16384];
    size_t n;
    bool ok = true;
    while (ok && (n = std::fread(buf, 1, sizeof buf, in)) > 0) ok = out.put(buf, n);
    std::fclose(in);
    return ok;
}

inline bool from_https(const std::string& url, Sink& out) {
    const std::wstring wurl = fetch::widen(url);
    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"Kyubu-ModManager/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 8000, 8000, 15000, 30000);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            ok = true;
            char buf[16384];
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(req, buf, sizeof buf, &got)) { ok = false; break; }
                if (got == 0) break;
                if (!out.put(buf, got)) { ok = false; break; }
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

// Fetches one item into its part file and checks it. Returns the reason of a failure, empty when staged.
inline std::string run_one(const std::string& dir, const Item& it) {
    const std::string part = part_path(dir, it), zip = zip_path(dir, it);
    DeleteFileA(part.c_str());
    Sink sink;
    sink.f = std::fopen(part.c_str(), "wb");
    if (!sink.f) return "Cannot write to kyubu\\staging.";
    sink.limit = it.size;
    sink.id = it.id;
    std::string base;
    const bool fetched = fetch::file_base(it.url, base) ? from_file(base.substr(0, base.size() - 1), sink) : from_https(it.url, sink);
    std::fclose(sink.f);
    if (is_cancelled(it.id)) { DeleteFileA(part.c_str()); return "Cancelled."; }
    if (!fetched) {
        DeleteFileA(part.c_str());
        return sink.over ? kMismatch : (sink.got == 0 ? "The catalog could not be reached." : "The download was cut off.");
    }
    uint8_t digest[32];
    sink.sha.final(digest);
    if (sink.got != it.size || kyubu::Sha256::to_hex(digest) != it.sha256) { DeleteFileA(part.c_str()); return kMismatch; }
    const zipcheck::Result zc = zipcheck::check_file(part, it.id, it.version);
    if (!zc.ok) { DeleteFileA(part.c_str()); return zc.reason; }
    if (!MoveFileExA(part.c_str(), zip.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(part.c_str()); return "Cannot write to kyubu\\staging."; }
    return "";
}

inline DWORD WINAPI worker(LPVOID) {
    Shared& s = shared();
    for (;;) {
        EnterCriticalSection(&s.cs);
        Item job;
        bool found = false;
        for (Item& it : s.items)
            if (it.st == St::Waiting) { it.st = St::Running; it.got = 0; job = it; found = true; break; }
        if (!found) s.thread = false;
        const std::string dir = s.staging;
        LeaveCriticalSection(&s.cs);
        if (!found) return 0;
        const std::string why = run_one(dir, job);
        EnterCriticalSection(&s.cs);
        for (size_t i = 0; i < s.items.size(); ++i) {
            Item& it = s.items[i];
            if (it.id != job.id || it.st != St::Running) continue;
            if (it.cancelled) {   // cancel_all ran meanwhile: nothing of it stays
                DeleteFileA(zip_path(dir, it).c_str());
                s.items.erase(s.items.begin() + static_cast<long>(i));
            } else if (why.empty()) { it.st = St::Staged; it.got = it.size; }
            else { it.st = St::Failed; it.reason = why; }
            break;
        }
        LeaveCriticalSection(&s.cs);
    }
}

inline void init(const std::string& staging) {
    Shared& s = shared();
    if (!s.inited) { InitializeCriticalSection(&s.cs); s.inited = true; }
    EnterCriticalSection(&s.cs);
    s.staging = staging;
    LeaveCriticalSection(&s.cs);
}

// Adds an item (game thread) and starts the worker when it is idle. False when the thread cannot start.
inline bool add(const Item& item) {
    Shared& s = shared();
    if (!s.inited) return false;
    EnterCriticalSection(&s.cs);
    CreateDirectoryA(s.staging.c_str(), nullptr);
    s.items.push_back(item);
    const bool start = !s.thread;
    if (start) s.thread = true;
    LeaveCriticalSection(&s.cs);
    if (!start) return true;
    HANDLE h = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!h) {
        EnterCriticalSection(&s.cs);
        s.thread = false;
        for (Item& it : s.items) if (it.st == St::Waiting) { it.st = St::Failed; it.reason = "The download could not start."; }
        LeaveCriticalSection(&s.cs);
        return false;
    }
    CloseHandle(h);
    return true;
}

inline std::vector<Item> snapshot() {
    Shared& s = shared();
    if (!s.inited) return {};
    EnterCriticalSection(&s.cs);
    std::vector<Item> v = s.items;
    LeaveCriticalSection(&s.cs);
    return v;
}

// Drops the items of `group` (all items when group < 0) and deletes their staged files. A running one is flagged and removed by
// the worker when its read loop sees the flag.
inline void cancel(int group) {
    Shared& s = shared();
    if (!s.inited) return;
    EnterCriticalSection(&s.cs);
    for (size_t i = 0; i < s.items.size();) {
        Item& it = s.items[i];
        if (group >= 0 && it.group != group) { ++i; continue; }
        if (it.st == St::Running) { it.cancelled = true; ++i; continue; }
        DeleteFileA(zip_path(s.staging, it).c_str());
        DeleteFileA(part_path(s.staging, it).c_str());
        s.items.erase(s.items.begin() + static_cast<long>(i));
    }
    LeaveCriticalSection(&s.cs);
}

// Forgets the items without touching their files: Apply has queued them, so the loader moves them at the next boot.
inline void forget_all() {
    Shared& s = shared();
    if (!s.inited) return;
    EnterCriticalSection(&s.cs);
    for (size_t i = 0; i < s.items.size();) {
        if (s.items[i].st == St::Running) { ++i; continue; }
        s.items.erase(s.items.begin() + static_cast<long>(i));
    }
    LeaveCriticalSection(&s.cs);
}

}  // namespace download
