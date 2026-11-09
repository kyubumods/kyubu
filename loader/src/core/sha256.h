#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace kyubu {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    void final(uint8_t out[32]);
    static std::string hex_of(const void* data, size_t len);
    static std::string to_hex(const uint8_t digest[32]);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t total_ = 0;
    size_t used_ = 0;
};

}  // namespace kyubu
