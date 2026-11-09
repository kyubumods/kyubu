// kyubu_probe: small helpers shared by the tools (address expressions, formatting, parsing).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "kyubu_api.h"

namespace kprobe {

// Mapping addresses (mapping/, docs/game/) assume the image at its preferred base.
constexpr uint32_t kMappingBase = 0x400000;

std::string strf(const char* f, ...);
std::string trim(std::string_view s);
std::vector<std::string> split(std::string_view s);   // on whitespace
bool parse_number(std::string_view s, double* out);   // 12, -3.5, 0x1F
bool parse_u32(std::string_view s, uint32_t* out);    // 12, 0x1F

// ECX of the latest recorded call of a traced function (by its trace label), for ecx(<label>).
using EcxLookup = std::function<bool(const std::string& label, uintptr_t* out, std::string* err)>;

// Evaluates an address expression:
//   0x1234ABCD, 1234    a runtime address or number
//   @0x4ED0EA           a mapping address (image base 0x400000), moved to where the game is loaded
//   cube::Database::get a symbol from the symbol maps (any name resolve_symbol takes)
//   ecx(<label>)        the this (ECX) of the latest call of a traced function; needs `ecx`
//   [x]                 the 32-bit value stored at x (a pointer)
//   a + b, a - b        sums
bool eval_address(const KyubuApi* api, std::string_view text, uintptr_t* out, std::string* err,
                  const EcxLookup* ecx = nullptr);

// "0x0078D0EA (@0x4ED0EA)" for an address inside the game image, else "0x12345678".
std::string describe(const KyubuApi* api, uintptr_t addr);

}  // namespace kprobe
