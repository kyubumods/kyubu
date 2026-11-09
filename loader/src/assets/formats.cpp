#include "formats.h"

#include <set>

#include "log.h"

namespace kyubu {

static const uint32_t kKey[44] = {4242, 9551, 840, 84800, 9242, 9846, 127, 9, 9483, 394, 123, 4834, 32444,
                                  24355, 2433, 17, 34234, 42342, 4243, 14, 184934, 1987, 3094, 1901, 89409,
                                  4813, 37, 143, 3490, 19483, 1343, 432, 84732, 9184, 9612, 1233, 3434, 1839,
                                  2984, 1993, 2984, 4895, 816583, 13};

std::vector<uint8_t> asset_decode(const std::vector<uint8_t>& enc) {
    std::vector<uint8_t> d = enc;
    size_t n = d.size();
    for (size_t i = n; i-- > 0;) {
        size_t j = (kKey[i % 44] + i) % n;
        std::swap(d[i], d[j]);
    }
    for (auto& b : d) b = uint8_t(0xFF - b);
    return d;
}

std::vector<uint8_t> asset_encode(const std::vector<uint8_t>& plain) {
    std::vector<uint8_t> d = plain;
    for (auto& b : d) b = uint8_t(0xFF - b);
    size_t n = d.size();
    for (size_t i = 0; i < n; ++i) {
        size_t j = (kKey[i % 44] + i) % n;
        std::swap(d[i], d[j]);
    }
    return d;
}

static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

std::optional<VoxelModel> parse_cub(const std::vector<uint8_t>& d, std::string* err) {
    if (d.size() < 12) { if (err) *err = "file shorter than the 12-byte header"; return std::nullopt; }
    VoxelModel m;
    m.sx = (int)rd32(d.data()); m.sy = (int)rd32(d.data() + 4); m.sz = (int)rd32(d.data() + 8);
    if (m.sx <= 0 || m.sy <= 0 || m.sz <= 0 || m.sx > 1024 || m.sy > 1024 || m.sz > 1024) {
        if (err) *err = fmt("invalid size %dx%dx%d", m.sx, m.sy, m.sz);
        return std::nullopt;
    }
    size_t need = 12 + (size_t)m.sx * m.sy * m.sz * 3;
    if (d.size() != need) { if (err) *err = fmt("size mismatch: %u bytes, expected %u", (unsigned)d.size(), (unsigned)need); return std::nullopt; }
    m.rgb.assign(d.begin() + 12, d.end());
    return m;
}

std::vector<uint8_t> write_cub(const VoxelModel& m) {
    std::vector<uint8_t> out(12);
    auto put = [&](int off, uint32_t v) { for (int i = 0; i < 4; ++i) out[off + i] = uint8_t(v >> (8 * i)); };
    put(0, m.sx); put(4, m.sy); put(8, m.sz);
    out.insert(out.end(), m.rgb.begin(), m.rgb.end());
    return out;
}

std::optional<VoxelModel> parse_vox(const std::vector<uint8_t>& d, std::string* err) {
    auto fail = [&](const std::string& e) -> std::optional<VoxelModel> { if (err) *err = e; return std::nullopt; };
    if (d.size() < 20 || memcmp(d.data(), "VOX ", 4) != 0) return fail("not a MagicaVoxel file (missing 'VOX ' header)");
    if (memcmp(d.data() + 8, "MAIN", 4) != 0) return fail("missing MAIN chunk");
    size_t pos = 20;
    int sx = 0, sy = 0, sz = 0;
    bool have_size = false, have_xyzi = false, have_rgba = false;
    std::vector<uint8_t> xyzi;
    uint8_t palette[256][3];
    while (pos + 12 <= d.size()) {
        const uint8_t* c = d.data() + pos;
        uint32_t n = rd32(c + 4), m = rd32(c + 8);
        if (pos + 12 + (size_t)n > d.size()) return fail("truncated chunk " + std::string((const char*)c, 4));
        const uint8_t* body = c + 12;
        if (!memcmp(c, "SIZE", 4) && !have_size) {
            if (n < 12) return fail("SIZE chunk too short");
            sx = (int)rd32(body); sy = (int)rd32(body + 4); sz = (int)rd32(body + 8);
            have_size = true;
        } else if (!memcmp(c, "XYZI", 4) && !have_xyzi) {
            if (n < 4) return fail("XYZI chunk too short");
            uint32_t cnt = rd32(body);
            if (4 + (size_t)cnt * 4 > n) return fail("XYZI chunk truncated");
            xyzi.assign(body + 4, body + 4 + cnt * 4);
            have_xyzi = true;
        } else if (!memcmp(c, "RGBA", 4)) {
            if (n < 1024) return fail("RGBA chunk too short");
            for (int i = 0; i < 256; ++i) { palette[i][0] = body[i * 4]; palette[i][1] = body[i * 4 + 1]; palette[i][2] = body[i * 4 + 2]; }
            have_rgba = true;
        }
        pos += 12 + n;
        (void)m;   // children of non-MAIN chunks are empty in practice; nodes follow sequentially
    }
    if (!have_size || !have_xyzi) return fail("missing SIZE or XYZI chunk");
    if (!have_rgba) return fail("no RGBA palette chunk (the default palette is not supported)");
    if (sx <= 0 || sy <= 0 || sz <= 0 || sx > 256 || sy > 256 || sz > 256) return fail(fmt("invalid size %dx%dx%d", sx, sy, sz));
    VoxelModel mdl;
    mdl.sx = sx; mdl.sy = sy; mdl.sz = sz;
    mdl.rgb.assign((size_t)sx * sy * sz * 3, 0);
    for (size_t i = 0; i + 3 < xyzi.size(); i += 4) {
        int x = xyzi[i], y = xyzi[i + 1], z = xyzi[i + 2], ci = xyzi[i + 3];
        if (x >= sx || y >= sy || z >= sz || ci == 0) continue;
        uint8_t r = palette[ci - 1][0], g = palette[ci - 1][1], b = palette[ci - 1][2];
        if (!r && !g && !b) r = g = b = 1;
        size_t o = (((size_t)z * sy + y) * sx + x) * 3;
        mdl.rgb[o] = r; mdl.rgb[o + 1] = g; mdl.rgb[o + 2] = b;
    }
    return mdl;
}

std::string validate_model(const VoxelModel& m) {
    if (m.sx > 256 || m.sy > 256 || m.sz > 256) return fmt("size %dx%dx%d exceeds 256 on an axis", m.sx, m.sy, m.sz);
    bool any = false;
    for (size_t i = 0; i + 2 < m.rgb.size(); i += 3) if (m.rgb[i] | m.rgb[i + 1] | m.rgb[i + 2]) { any = true; break; }
    if (!any) return "model is empty";
    return {};
}

std::string validate_wav(const std::vector<uint8_t>& d) {
    if (d.size() < 44 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) return "not a RIFF/WAVE file";
    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        uint32_t n = rd32(d.data() + pos + 4);
        if (!memcmp(d.data() + pos, "fmt ", 4)) {
            if (pos + 8 + 16 > d.size()) return "truncated fmt chunk";
            const uint8_t* f = d.data() + pos + 8;
            uint16_t format = f[0] | (f[1] << 8), bits = f[14] | (f[15] << 8);
            if (!((format == 1 && bits == 16) || (format == 3 && bits == 32)))
                return fmt("unsupported sample format %u/%u-bit (need PCM 16-bit or IEEE float 32-bit)", format, bits);
            return {};
        }
        pos += 8 + n + (n & 1);
    }
    return "no fmt chunk";
}

std::string validate_png(const std::vector<uint8_t>& d) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (d.size() < 8 || memcmp(d.data(), sig, 8)) return "not a PNG file";
    return {};
}

// ---------------------------------------------------------------- dictionary merge
namespace {
struct Entry { size_t start, end; std::string key; };   // [start,end) covers "<tag ...>...</tag>"

std::vector<Entry> find_entries(const std::string& s) {
    std::vector<Entry> out;
    size_t pos = 0;
    while (true) {
        size_t lt = s.find('<', pos);
        if (lt == std::string::npos) break;
        std::string tag;
        for (const char* t : {"name", "speech", "landscape"}) {
            size_t n = strlen(t);
            if (s.compare(lt + 1, n, t) == 0 && lt + 1 + n < s.size() && (s[lt + 1 + n] == ' ' || s[lt + 1 + n] == '\t')) { tag = t; break; }
        }
        if (tag.empty()) { pos = lt + 1; continue; }
        size_t gt = s.find('>', lt);
        if (gt == std::string::npos) break;
        std::string head = s.substr(lt, gt - lt);
        size_t k = head.find("key=\"");
        if (k == std::string::npos) { pos = gt; continue; }
        size_t ke = head.find('"', k + 5);
        std::string key = head.substr(k + 5, ke - (k + 5));
        std::string close = "</" + tag + ">";
        size_t ce = s.find(close, gt);
        if (ce == std::string::npos) break;
        out.push_back({lt, ce + close.size(), key});
        pos = ce + close.size();
    }
    return out;
}

std::string strip_bom(const std::string& s) {
    return (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) ? s.substr(3) : s;
}

std::string to_crlf(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') continue;
        if (s[i] == '\n') o += "\r\n";
        else o += s[i];
    }
    return o;
}
}  // namespace

std::string merge_dictionary(const std::string& base_in, const std::string& patch_in, std::vector<std::string>* replaced,
                             std::vector<std::string>* added) {
    std::string base = strip_bom(base_in);
    std::string patch = to_crlf(strip_bom(patch_in));
    for (auto& pe : find_entries(patch)) {
        std::string block = patch.substr(pe.start, pe.end - pe.start);
        auto be = find_entries(base);
        bool done = false;
        for (auto& b : be) {
            if (b.key == pe.key) {
                base.replace(b.start, b.end - b.start, block);
                if (replaced) replaced->push_back(pe.key);
                done = true;
                break;
            }
        }
        if (!done) {
            size_t root = base.rfind("</root>");
            if (root == std::string::npos) root = base.size();
            base.insert(root, block + "\r\n\r\n");
            if (added) added->push_back(pe.key);
        }
    }
    return "\xEF\xBB\xBF" + base;
}

VoxelModel missing_model() {
    VoxelModel m;
    m.sx = m.sy = m.sz = 8;
    m.rgb.assign(8 * 8 * 8 * 3, 0);
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                bool edge = (x == 0 || x == 7) + (y == 0 || y == 7) + (z == 0 || z == 7) >= 1;
                if (!edge) continue;
                bool check = ((x / 2) + (y / 2) + (z / 2)) % 2;
                size_t o = (((size_t)z * 8 + y) * 8 + x) * 3;
                m.rgb[o] = check ? 255 : 20; m.rgb[o + 1] = check ? 0 : 20; m.rgb[o + 2] = check ? 255 : 20;
            }
    return m;
}

}  // namespace kyubu
