// Reaching the catalog (design D4): index.json and index.json.sig from an https:// address with WinHTTP, or from a
// file:/// folder (tests). Blocking; call it on a worker thread. It decides nothing about trust: index.h does.
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <string>

#include "index.h"

namespace fetch {

constexpr size_t kMaxBytes = 4u << 20;   // an index or signature larger than this is not one

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring w(static_cast<size_t>(n), L' ');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

inline bool read_file(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
        if (out.size() > kMaxBytes) { std::fclose(f); return false; }
    }
    std::fclose(f);
    return true;
}

inline std::string with_slash(std::string url) {
    if (!url.empty() && url.back() != '/') url += '/';
    return url;
}

// "file:///D:/x/" -> "D:/x/" (the three slashes are the scheme's; a drive letter follows).
inline bool file_base(const std::string& url, std::string& dir) {
    static const std::string p = "file:///";
    if (url.compare(0, p.size(), p) != 0) return false;
    dir = with_slash(url.substr(p.size()));
    return true;
}

enum class Get { Ok, NotModified, Failed };

// One GET. `etag` in: sent as If-None-Match when not empty. `etag_out`: the response's ETag.
inline Get https_get(const std::string& url, const std::string& etag, std::string& body, std::string& etag_out) {
    body.clear();
    etag_out.clear();
    const std::wstring wurl = widen(url);
    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return Get::Failed;
    Get result = Get::Failed;
    HINTERNET ses = WinHttpOpen(L"Kyubu-ModManager/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return Get::Failed;
    WinHttpSetTimeouts(ses, 8000, 8000, 15000, 15000);
    // Redirects may not leave HTTPS (the default policy).
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req) {
        const std::wstring hdr = etag.empty() ? L"" : L"If-None-Match: " + widen(etag);
        if (WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), hdr.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, sz = sizeof status;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            if (status == 304) {
                result = Get::NotModified;
            } else if (status == 200) {
                wchar_t tag[256] = {};
                DWORD tsz = sizeof tag;
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, tag, &tsz, WINHTTP_NO_HEADER_INDEX)) {
                    for (const wchar_t* c = tag; *c; ++c) etag_out += *c < 128 ? static_cast<char>(*c) : '?';
                }
                bool ok = true;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
                    if (avail == 0) break;
                    if (body.size() + avail > kMaxBytes) { ok = false; break; }
                    const size_t at = body.size();
                    body.resize(at + avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(req, &body[at], avail, &got)) { ok = false; break; }
                    body.resize(at + got);
                }
                if (ok) result = Get::Ok;
            }
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return result;
}

// Fetches the index and its signature from `base`. `etag` is the saved copy's; a 304 answers without a body.
inline cindex::Fetched fetch_index(const std::string& base, const std::string& etag) {
    cindex::Fetched f;
    std::string dir;
    if (file_base(base, dir)) {
        f.reached = read_file(dir + "index.json", f.index);
        if (f.reached && !read_file(dir + "index.json.sig", f.sig)) f.sig.clear();
        return f;
    }
    const std::string root = with_slash(base);
    std::string tag;
    switch (https_get(root + "index.json", etag, f.index, tag)) {
        case Get::Failed: return f;
        case Get::NotModified: f.reached = true; f.not_modified = true; return f;
        case Get::Ok: break;
    }
    f.reached = true;
    f.etag = tag;
    std::string unused;
    if (https_get(root + "index.json.sig", "", f.sig, unused) != Get::Ok) f.sig.clear();
    return f;
}

}  // namespace fetch
