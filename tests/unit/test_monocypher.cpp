#include "doctest.h"

#include <cstdint>
#include <string>
#include <vector>

#include "monocypher-ed25519.h"

namespace {
std::vector<uint8_t> hex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(std::stoi(s.substr(i, 2), nullptr, 16)));
    return out;
}

struct Vector { const char *seed, *pub, *msg, *sig; };

// RFC 8032 section 7.1, tests 1 and 2.
const Vector kVectors[] = {
    {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
};
}  // namespace

TEST_CASE("monocypher: RFC 8032 Ed25519 vectors") {
    for (const Vector& v : kVectors) {
        auto seed = hex(v.seed), pub = hex(v.pub), msg = hex(v.msg), sig = hex(v.sig);
        uint8_t sk[64], pk[32];
        crypto_ed25519_key_pair(sk, pk, seed.data());   // wipes the seed
        CHECK(std::vector<uint8_t>(pk, pk + 32) == pub);

        CHECK(crypto_ed25519_check(sig.data(), pub.data(), msg.data(), msg.size()) == 0);

        uint8_t mine[64];
        crypto_ed25519_sign(mine, sk, msg.data(), msg.size());
        CHECK(std::vector<uint8_t>(mine, mine + 64) == sig);

        auto bad = sig;
        bad[10] ^= 1;
        CHECK(crypto_ed25519_check(bad.data(), pub.data(), msg.data(), msg.size()) != 0);
        auto other = msg;
        other.push_back(0);
        CHECK(crypto_ed25519_check(sig.data(), pub.data(), other.data(), other.size()) != 0);
    }
}
