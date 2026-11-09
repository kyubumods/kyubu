// Layouts of game-side (MSVC 2012 / VC110) types the loader touches.
#pragma once
#include "common.h"

namespace kyubu {

// std::string, VC110 x86 layout: 16-byte SSO buffer, size, capacity.
struct VcString {
    union { char buf[16]; char* ptr; };
    uint32_t size;
    uint32_t cap;
    const char* c_str() const { return cap >= 16 ? ptr : buf; }
    std::string str() const { return std::string(c_str(), size); }
};
static_assert(sizeof(VcString) == 24, "VC110 std::string is 24 bytes");

// std::vector<T*>, VC110 layout.
struct VcPtrVector {
    void** begin;
    void** end;
    void** cap;
    uint32_t size() const { return (uint32_t)(end - begin); }
};

// The game's allocator (MSVCR110 operator new/delete).
void* game_new(size_t n);
void game_delete(void* p);

// Builds/destroys a VcString in caller-provided storage using the game allocator for long strings.
void vc_string_init(VcString* s, const std::string& v);
void vc_string_free(VcString* s);

}  // namespace kyubu
