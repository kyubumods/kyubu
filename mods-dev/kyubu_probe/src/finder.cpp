#include "finder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "util.h"

namespace kprobe {

namespace {
constexpr uint32_t kPage = 4096;
const char* const kTypeNames[] = {"i8", "u8", "i16", "u16", "i32", "u32", "f32", "f64"};
const uint32_t kTypeSizes[] = {1, 1, 2, 2, 4, 4, 4, 8};
const char* const kOpNames[] = {"changed", "unchanged", "increased", "decreased", "equal", "delta"};

bool close_to(double a, double b, bool fp) {
    if (!fp) return a == b;
    return std::fabs(a - b) <= 1e-4 * std::max(1.0, std::fabs(b));
}
}  // namespace

bool parse_vtype(const std::string& s, VType* out) {
    for (int i = 0; i < 8; ++i)
        if (s == kTypeNames[i]) { *out = (VType)i; return true; }
    return false;
}
const char* vtype_name(VType t) { return kTypeNames[(int)t]; }
uint32_t vtype_size(VType t) { return kTypeSizes[(int)t]; }

bool parse_op(const std::string& s, Op* out) {
    for (int i = 0; i < 6; ++i)
        if (s == kOpNames[i]) { *out = (Op)i; return true; }
    return false;
}

int Finder::create(uintptr_t base, uint32_t len, VType type, uint32_t align, std::string* err) {
    uint32_t size = vtype_size(type);
    if (!align) align = size;
    if (len < size) { *err = strf("the range must hold at least one %s (%u bytes)", vtype_name(type), size); return 0; }
    if (len > kMaxRange) { *err = strf("the range is limited to %u MB", kMaxRange >> 20); return 0; }
    if (base + len < base) { *err = "the range wraps around the address space"; return 0; }
    std::lock_guard<std::mutex> g(mu_);
    Search s;
    s.base = base;
    s.len = len;
    s.type = type;
    s.align = align;
    int id = next_++;
    searches_.emplace(id, std::move(s));
    return id;
}

bool Finder::destroy(int id) {
    std::lock_guard<std::mutex> g(mu_);
    return searches_.erase(id) != 0;
}

double Finder::value(const Search& s, const std::vector<uint8_t>& buf, uint32_t off) const {
    const uint8_t* p = buf.data() + off;
    switch (s.type) {
        case VType::I8: return (int8_t)p[0];
        case VType::U8: return p[0];
        case VType::I16: { int16_t v; memcpy(&v, p, 2); return v; }
        case VType::U16: { uint16_t v; memcpy(&v, p, 2); return v; }
        case VType::I32: { int32_t v; memcpy(&v, p, 4); return v; }
        case VType::U32: { uint32_t v; memcpy(&v, p, 4); return v; }
        case VType::F32: { float v; memcpy(&v, p, 4); return v; }
        case VType::F64: { double v; memcpy(&v, p, 8); return v; }
    }
    return 0;
}

bool Finder::readable(const Search& s, const std::vector<uint8_t>& ok, uint32_t off) const {
    uint32_t first = (uint32_t)((s.base + off) / kPage - s.base / kPage);
    uint32_t last = (uint32_t)((s.base + off + vtype_size(s.type) - 1) / kPage - s.base / kPage);
    for (uint32_t p = first; p <= last; ++p)
        if (!ok[p]) return false;
    return true;
}

long Finder::snapshot(int id, std::string* err) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = searches_.find(id);
    if (it == searches_.end()) { *err = strf("no search %d", id); return -1; }
    Search& s = it->second;
    std::vector<uint8_t> buf(s.len), ok((s.base + s.len - 1) / kPage - s.base / kPage + 1);
    // Read page by page, so that one unreadable page only removes the candidates on it.
    uint32_t off = 0;
    bool any = false;
    for (size_t p = 0; p < ok.size(); ++p) {
        uintptr_t page_end = (s.base / kPage + p + 1) * kPage;
        uint32_t n = (uint32_t)std::min<uintptr_t>(page_end - (s.base + off), s.len - off);
        ok[p] = api_->mem_read(s.base + off, buf.data() + off, n) == KY_OK;
        any |= ok[p] != 0;
        off += n;
    }
    if (!any) { *err = strf("the range at 0x%08X is not readable", (unsigned)s.base); return -1; }
    s.prev.swap(s.cur);
    s.prev_ok.swap(s.cur_ok);
    s.cur.swap(buf);
    s.cur_ok.swap(ok);
    uint32_t size = vtype_size(s.type);
    bool fp = s.type == VType::F32 || s.type == VType::F64;
    if (s.snapshots == 0) {
        for (uint32_t o = 0; o + size <= s.len; o += s.align) {
            if (!readable(s, s.cur_ok, o)) continue;
            if (fp && !std::isfinite(value(s, s.cur, o))) continue;   // NaN never compares; skip it
            s.cand.push_back(o);
        }
        s.changes.assign(s.cand.size(), 0);
    } else {
        size_t w = 0;
        for (size_t i = 0; i < s.cand.size(); ++i) {
            uint32_t o = s.cand[i];
            if (!readable(s, s.cur_ok, o)) continue;    // the page went away: the offset can't be judged
            if (fp && !std::isfinite(value(s, s.cur, o))) continue;
            uint32_t c = s.changes[i] + (memcmp(s.prev.data() + o, s.cur.data() + o, size) != 0 ? 1 : 0);
            s.cand[w] = o;
            s.changes[w++] = c;
        }
        s.cand.resize(w);
        s.changes.resize(w);
    }
    ++s.snapshots;
    s.ranked_ok = false;
    return (long)s.cand.size();
}

long Finder::filter(int id, Op op, double v, std::string* err) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = searches_.find(id);
    if (it == searches_.end()) { *err = strf("no search %d", id); return -1; }
    Search& s = it->second;
    if (s.snapshots == 0) { *err = "take a snapshot first"; return -1; }
    if (op != Op::Equal && s.snapshots < 2) { *err = strf("'%s' compares two snapshots: take another one first", kOpNames[(int)op]); return -1; }
    bool fp = s.type == VType::F32 || s.type == VType::F64;
    size_t w = 0;
    for (size_t i = 0; i < s.cand.size(); ++i) {
        uint32_t o = s.cand[i];
        double now = value(s, s.cur, o), before = op == Op::Equal ? now : value(s, s.prev, o);
        bool keep = false;
        switch (op) {
            case Op::Changed: keep = memcmp(s.prev.data() + o, s.cur.data() + o, vtype_size(s.type)) != 0; break;
            case Op::Unchanged: keep = memcmp(s.prev.data() + o, s.cur.data() + o, vtype_size(s.type)) == 0; break;
            case Op::Increased: keep = now > before; break;
            case Op::Decreased: keep = now < before; break;
            case Op::Equal: keep = close_to(now, v, fp); break;
            case Op::Delta: keep = close_to(now - before, v, fp); break;
        }
        if (keep) {
            s.cand[w] = o;
            s.changes[w++] = s.changes[i];
        }
    }
    s.cand.resize(w);
    s.changes.resize(w);
    s.ranked_ok = false;
    return (long)w;
}

void Finder::rank(Search& s) {
    if (s.ranked_ok) return;
    s.ranked.resize(s.cand.size());
    for (uint32_t i = 0; i < s.ranked.size(); ++i) s.ranked[i] = i;
    std::stable_sort(s.ranked.begin(), s.ranked.end(),
                     [&](uint32_t a, uint32_t b) { return s.changes[a] > s.changes[b]; });
    s.ranked_ok = true;
}

KyubuProbeHit Finder::make_hit(const Search& s, size_t i) const {
    KyubuProbeHit h{};
    h.offset = s.cand[i];
    h.address = (uint32_t)(s.base + s.cand[i]);
    h.value = value(s, s.cur, s.cand[i]);
    h.previous = s.snapshots > 1 ? value(s, s.prev, s.cand[i]) : h.value;
    h.changes = s.changes[i];
    return h;
}

std::vector<KyubuProbeHit> Finder::results(int id, size_t max, std::string* err) {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<KyubuProbeHit> out;
    auto it = searches_.find(id);
    if (it == searches_.end()) { *err = strf("no search %d", id); return out; }
    Search& s = it->second;
    rank(s);
    for (size_t r = 0; r < s.ranked.size() && r < max; ++r) out.push_back(make_hit(s, s.ranked[r]));
    return out;
}

bool Finder::hit(int id, uint32_t rank_no, KyubuProbeHit* out) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = searches_.find(id);
    if (it == searches_.end()) return false;
    Search& s = it->second;
    rank(s);
    if (rank_no >= s.ranked.size()) return false;
    *out = make_hit(s, s.ranked[rank_no]);
    return true;
}

bool Finder::info(int id, Info* out) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = searches_.find(id);
    if (it == searches_.end()) return false;
    const Search& s = it->second;
    *out = {s.base, s.len, s.type, s.align, s.snapshots, s.cand.size()};
    return true;
}

std::vector<int> Finder::ids() {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<int> v;
    for (auto& [id, s] : searches_) v.push_back(id);
    return v;
}

// ------------------------------------------------------------------ the C interface
namespace {
Finder* g_finder = nullptr;
std::mutex g_err_mu;
std::string g_err;

int32_t set_error(int32_t code, const std::string& why) {
    std::lock_guard<std::mutex> g(g_err_mu);
    g_err = why;
    return code;
}

int32_t KYUBU_CALL i_create(const char* address, uint32_t length, const char* value_type, uint32_t align) {
    if (!g_finder || !address || !value_type) return set_error(KY_E_INVALID_ARG, "address and value_type are required");
    VType t;
    if (!parse_vtype(value_type, &t)) return set_error(KY_E_INVALID_ARG, std::string("unknown value type ") + value_type);
    uintptr_t a = 0;
    std::string err;
    if (!eval_address(g_finder->api(), address, &a, &err)) return set_error(KY_E_INVALID_ARG, err);
    int id = g_finder->create(a, length, t, align, &err);
    return id ? id : set_error(KY_E_INVALID_ARG, err);
}
int32_t KYUBU_CALL i_snapshot(int32_t id) {
    std::string err;
    long n = g_finder ? g_finder->snapshot(id, &err) : -1;
    return n >= 0 ? (int32_t)n : set_error(err.rfind("no search", 0) == 0 ? KY_E_NOT_FOUND : KY_E_ACCESS, err);
}
int32_t KYUBU_CALL i_filter(int32_t id, const char* op, double value) {
    Op o;
    if (!op || !parse_op(op, &o)) return set_error(KY_E_INVALID_ARG, std::string("unknown filter ") + (op ? op : "(null)"));
    std::string err;
    long n = g_finder ? g_finder->filter(id, o, value, &err) : -1;
    return n >= 0 ? (int32_t)n : set_error(err.rfind("no search", 0) == 0 ? KY_E_NOT_FOUND : KY_E_PHASE, err);
}
int32_t KYUBU_CALL i_result(int32_t id, uint32_t rank_no, KyubuProbeHit* out) {
    if (!out) return KY_E_INVALID_ARG;
    return g_finder && g_finder->hit(id, rank_no, out) ? KY_OK : KY_E_NOT_FOUND;
}
int32_t KYUBU_CALL i_destroy(int32_t id) { return g_finder && g_finder->destroy(id) ? KY_OK : KY_E_NOT_FOUND; }
int32_t KYUBU_CALL i_error(char* buf, uint32_t cap) {
    std::lock_guard<std::mutex> g(g_err_mu);
    if (!buf || !cap) return KY_E_INVALID_ARG;
    if (g_err.size() + 1 > cap) return KY_E_BUFFER;
    memcpy(buf, g_err.c_str(), g_err.size() + 1);
    return KY_OK;
}
}  // namespace

const KyubuProbeFinder* finder_interface(Finder* f) {
    static KyubuProbeFinder table = {sizeof(KyubuProbeFinder), i_create, i_snapshot, i_filter, i_result, i_destroy, i_error};
    g_finder = f;
    return &table;
}

}  // namespace kprobe
