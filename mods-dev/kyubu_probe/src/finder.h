// kyubu_probe field finder: snapshots of a memory range, ranked by what changed between them (live-probes,
// "Field finder"). Thread-safe: triggers may fire from hooks on any thread.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "kyubu_probe.h"

namespace kprobe {

enum class VType { I8, U8, I16, U16, I32, U32, F32, F64 };
bool parse_vtype(const std::string& s, VType* out);
const char* vtype_name(VType t);
uint32_t vtype_size(VType t);

enum class Op { Changed, Unchanged, Increased, Decreased, Equal, Delta };
bool parse_op(const std::string& s, Op* out);

constexpr uint32_t kMaxRange = 16u << 20;   // 16 MB per search

class Finder {
public:
    explicit Finder(const KyubuApi* api) : api_(api) {}
    const KyubuApi* api() const { return api_; }

    // A new search; returns its id (> 0) or 0 with *err set.
    int create(uintptr_t base, uint32_t len, VType type, uint32_t align, std::string* err);
    bool destroy(int id);
    // Reads the range; returns the candidates left, or -1 with *err set.
    long snapshot(int id, std::string* err);
    long filter(int id, Op op, double value, std::string* err);
    // Ranked: most often changed first, then by offset.
    std::vector<KyubuProbeHit> results(int id, size_t max, std::string* err);
    bool hit(int id, uint32_t rank, KyubuProbeHit* out);

    struct Info { uintptr_t base; uint32_t len; VType type; uint32_t align; int snapshots; size_t candidates; };
    bool info(int id, Info* out);
    std::vector<int> ids();

private:
    struct Search {
        uintptr_t base = 0;
        uint32_t len = 0;
        VType type = VType::I32;
        uint32_t align = 4;
        int snapshots = 0;
        std::vector<uint8_t> prev, cur;          // the last two snapshots
        std::vector<uint8_t> prev_ok, cur_ok;    // readable, per 4 KB page
        std::vector<uint32_t> cand;              // candidate offsets
        std::vector<uint32_t> changes;           // per candidate
        std::vector<uint32_t> ranked;            // indexes into cand, filled lazily
        bool ranked_ok = false;
    };
    double value(const Search& s, const std::vector<uint8_t>& buf, uint32_t off) const;
    bool readable(const Search& s, const std::vector<uint8_t>& ok, uint32_t off) const;
    void rank(Search& s);
    KyubuProbeHit make_hit(const Search& s, size_t i) const;

    const KyubuApi* api_;
    std::mutex mu_;
    std::map<int, Search> searches_;
    int next_ = 1;
};

// The published interface (KYUBU_PROBE_FINDER): a C table over one Finder (the last one passed).
const KyubuProbeFinder* finder_interface(Finder* f);

}  // namespace kprobe
