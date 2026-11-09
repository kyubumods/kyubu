#include "common.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace kyubu {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}

std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(s.size() - p.size()) == p; }

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) { out.emplace_back(s.substr(start, i - start)); start = i + 1; }
    }
    return out;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return std::string(s.substr(a, b - a));
}

std::string hex(uint32_t v, int width) {
    char b[16];
    snprintf(b, sizeof b, "0x%0*X", width, v);
    return b;
}

bool glob_match(std::string_view p, std::string_view t) {
    size_t pi = 0, ti = 0, star = std::string_view::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) { ++pi; ++ti; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ti; }
        else if (star != std::string_view::npos) { pi = star + 1; ti = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

KyubuResult copy_out(const std::string& s, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return KY_E_BUFFER;
    size_t n = std::min<size_t>(s.size(), cap - 1);
    memcpy(buf, s.data(), n);
    buf[n] = 0;
    return s.size() < cap ? KY_OK : KY_E_BUFFER;
}

bool file_exists(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dirs(const std::string& path) {
    if (path.empty() || dir_exists(path)) return true;
    std::string parent = dir_of(path);
    if (!parent.empty() && parent != path) make_dirs(parent);
    return CreateDirectoryW(widen(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::optional<std::vector<uint8_t>> read_file(const std::string& path) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::vector<uint8_t> data((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = data.empty() || (ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr) && got == data.size());
    CloseHandle(h);
    if (!ok) return std::nullopt;
    return data;
}

bool write_file(const std::string& path, const void* data, size_t len) {
    make_dirs(dir_of(path));
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = len == 0 || (WriteFile(h, data, (DWORD)len, &put, nullptr) && put == len);
    CloseHandle(h);
    return ok;
}

bool write_text(const std::string& path, const std::string& text) { return write_file(path, text.data(), text.size()); }

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(join_path(path, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        if (n != L"." && n != L"..") out.push_back(narrow(n));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

uint64_t file_mtime(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d)) return 0;
    return (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

std::string dir_of(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? std::string() : path.substr(0, p);
}

std::string file_name(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string now_stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    snprintf(b, sizeof b, "%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

HMODULE module_of(uintptr_t addr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &m))
        return nullptr;
    return m;
}

std::string describe_address(uintptr_t addr) {
    HMODULE m = module_of(addr);
    if (!m) return hex((uint32_t)addr);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(m, path, MAX_PATH);
    char b[64];
    snprintf(b, sizeof b, "+0x%X", (unsigned)(addr - (uintptr_t)m));
    return file_name(narrow(path)) + b;
}

}  // namespace kyubu
