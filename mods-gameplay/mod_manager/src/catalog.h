// The catalog on a worker thread: fetch_index, then cindex::resolve against the saved copy in the mod's data folder
// (index.json, index.json.sig, index.etag), then save a new verified copy. The frame code reads the result with get().
#pragma once

#include <windows.h>

#include <string>

#include "fetch.h"
#include "index.h"
#include "keys.h"

namespace catalog {

struct Job {
    std::string url, dir;
};

struct Shared {
    CRITICAL_SECTION cs;
    bool inited = false, running = false, unlogged = false;
    unsigned runs = 0;
    cindex::Loaded loaded;
    std::string url;
};

inline Shared& shared() {
    static Shared s;
    return s;
}

inline bool write_file(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

inline DWORD WINAPI worker(LPVOID p) {
    Job* job = static_cast<Job*>(p);
    std::string saved, sig, etag;
    fetch::read_file(job->dir + "\\index.json", saved);
    fetch::read_file(job->dir + "\\index.json.sig", sig);
    fetch::read_file(job->dir + "\\index.etag", etag);
    // The saved ETag only counts with a saved index to fall back on.
    const cindex::Fetched net = fetch::fetch_index(job->url, saved.empty() ? "" : etag);
    cindex::Loaded out = cindex::resolve(net, saved, sig, etag, keys::trusted());
    if (out.save) {
        CreateDirectoryA(job->dir.c_str(), nullptr);
        write_file(job->dir + "\\index.json", out.index_text);
        write_file(job->dir + "\\index.json.sig", out.sig_text);
        write_file(job->dir + "\\index.etag", out.etag);
    }
    Shared& s = shared();
    EnterCriticalSection(&s.cs);
    s.loaded = out;
    s.running = false;
    s.unlogged = true;
    s.runs++;
    LeaveCriticalSection(&s.cs);
    delete job;
    return 0;
}

// Starts a fetch of `url` unless one is running. Call from the game thread only.
inline bool start(const std::string& url, const std::string& dir) {
    Shared& s = shared();
    if (!s.inited) { InitializeCriticalSection(&s.cs); s.inited = true; }
    EnterCriticalSection(&s.cs);
    const bool busy = s.running;
    if (!busy) { s.running = true; s.url = url; }
    LeaveCriticalSection(&s.cs);
    if (busy) return false;
    HANDLE h = CreateThread(nullptr, 0, worker, new Job{url, dir}, 0, nullptr);
    if (!h) {
        EnterCriticalSection(&s.cs);
        s.running = false;
        LeaveCriticalSection(&s.cs);
        return false;
    }
    CloseHandle(h);
    return true;
}

// A copy of the last result. `fresh` is true once per finished fetch (for logging).
inline cindex::Loaded get(bool* fresh = nullptr, unsigned* runs = nullptr, bool* running = nullptr) {
    Shared& s = shared();
    if (!s.inited) { if (fresh) *fresh = false; if (runs) *runs = 0; if (running) *running = false; return cindex::Loaded(); }
    EnterCriticalSection(&s.cs);
    cindex::Loaded l = s.loaded;
    if (fresh) { *fresh = s.unlogged; s.unlogged = false; }
    if (runs) *runs = s.runs;
    if (running) *running = s.running;
    LeaveCriticalSection(&s.cs);
    return l;
}

}  // namespace catalog
