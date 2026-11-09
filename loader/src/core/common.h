// Shared helpers for the Kyubu loader.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include <algorithm>

#include "kyubu_api.h"

namespace kyubu {

std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view s);
std::string lower(std::string s);
bool starts_with(std::string_view s, std::string_view p);
bool ends_with(std::string_view s, std::string_view p);
std::vector<std::string> split(std::string_view s, char sep);
std::string trim(std::string_view s);
std::string hex(uint32_t v, int width = 8);
bool glob_match(std::string_view pattern, std::string_view text);   // '*' and '?'

// Copies s into a C buffer. Returns KY_E_BUFFER if it does not fit (buffer gets truncated, NUL terminated).
KyubuResult copy_out(const std::string& s, char* buf, uint32_t cap);

// File helpers (UTF-8 paths)
bool file_exists(const std::string& path);
bool dir_exists(const std::string& path);
bool make_dirs(const std::string& path);
std::optional<std::vector<uint8_t>> read_file(const std::string& path);
bool write_file(const std::string& path, const void* data, size_t len);
bool write_text(const std::string& path, const std::string& text);
std::vector<std::string> list_dir(const std::string& path);           // names only, no . / ..
uint64_t file_mtime(const std::string& path);                          // 0 when missing
std::string join_path(const std::string& a, const std::string& b);
std::string dir_of(const std::string& path);
std::string file_name(const std::string& path);
std::string now_stamp();                                                // 2026-09-25_12-00-00

// Formats a code address as "module+0xOFFSET" (or raw hex).
std::string describe_address(uintptr_t addr);
HMODULE module_of(uintptr_t addr);

}  // namespace kyubu
