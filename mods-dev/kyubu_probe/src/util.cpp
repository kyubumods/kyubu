#include "util.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace kprobe {

std::string strf(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace((unsigned char)s[j])) ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool parse_number(std::string_view s, double* out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    bool neg = t[0] == '-';
    std::string body = neg || t[0] == '+' ? t.substr(1) : t;
    if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
        unsigned long long v = std::strtoull(body.c_str() + 2, &end, 16);
        if (*end || end == body.c_str() + 2) return false;
        *out = neg ? -(double)v : (double)v;
        return true;
    }
    double v = std::strtod(t.c_str(), &end);
    if (*end) return false;
    *out = v;
    return true;
}

bool parse_u32(std::string_view s, uint32_t* out) {
    double v = 0;
    if (!parse_number(s, &v) || v < 0 || v > 4294967295.0 || v != (double)(uint32_t)v) return false;
    *out = (uint32_t)v;
    return true;
}

namespace {

struct Parser {
    const KyubuApi* api;
    std::string_view s;
    size_t i = 0;
    std::string* err;
    const EcxLookup* ecx = nullptr;

    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    bool fail(const std::string& why) {
        if (err && err->empty()) *err = why;
        return false;
    }

    bool expr(uintptr_t* out) {
        uintptr_t v = 0;
        if (!term(&v)) return false;
        for (;;) {
            ws();
            if (i >= s.size() || (s[i] != '+' && s[i] != '-')) break;
            char op = s[i++];
            uintptr_t r = 0;
            if (!term(&r)) return false;
            v = op == '+' ? v + r : v - r;
        }
        *out = v;
        return true;
    }

    bool term(uintptr_t* out) {
        ws();
        if (i >= s.size()) return fail("address expression ends too early");
        if (s[i] == '[') {
            ++i;
            uintptr_t p = 0;
            if (!expr(&p)) return false;
            ws();
            if (i >= s.size() || s[i] != ']') return fail("missing ]");
            ++i;
            uint32_t v = 0;
            if (api->mem_read(p, &v, 4) != KY_OK) return fail(strf("cannot read the pointer at 0x%08X", (unsigned)p));
            *out = v;
            return true;
        }
        bool mapping = s[i] == '@';
        if (mapping) ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace((unsigned char)s[j]) && s[j] != '+' && s[j] != '[' && s[j] != ']' &&
               !(s[j] == '-' && j > i))
            ++j;
        std::string tok(s.substr(i, j - i));
        i = j;
        if (tok.empty()) return fail("expected a number, a symbol or [");
        if (std::isdigit((unsigned char)tok[0])) {
            uint32_t v = 0;
            if (!parse_u32(tok, &v)) return fail("not a number: " + tok);
            if (mapping) {
                if (v < kMappingBase) return fail(strf("@0x%X is below the image base 0x%X", v, kMappingBase));
                *out = api->rva_to_addr(v - kMappingBase);
            } else {
                *out = v;
            }
            return true;
        }
        if (mapping) return fail("@ takes a mapping address, e.g. @0x4ED0EA");
        if (tok.size() > 5 && tok.compare(0, 4, "ecx(") == 0 && tok.back() == ')') {
            if (!ecx) return fail("ecx(...) is not available here");
            std::string why;
            if (!(*ecx)(tok.substr(4, tok.size() - 5), out, &why)) return fail(why);
            return true;
        }
        uintptr_t a = 0;
        KyubuResult r = api->resolve_symbol(tok.c_str(), &a);
        if (r != KY_OK)
            return fail(r == KY_E_UNAVAILABLE ? tok + " is not available on this build (see the log)"
                                              : "unknown symbol " + tok);
        *out = a;
        return true;
    }
};

}  // namespace

bool eval_address(const KyubuApi* api, std::string_view text, uintptr_t* out, std::string* err,
                  const EcxLookup* ecx) {
    if (err) err->clear();
    Parser p{api, text, 0, err, ecx};
    uintptr_t v = 0;
    if (!p.expr(&v)) return false;
    p.ws();
    if (p.i != text.size()) return p.fail("unexpected text: " + std::string(text.substr(p.i)));
    *out = v;
    return true;
}

std::string describe(const KyubuApi* api, uintptr_t addr) {
    KyubuHostInfo h{};
    h.size = sizeof h;
    if (api->host_info(&h) == KY_OK && addr >= h.image_base && addr - h.image_base < h.image_size)
        return strf("0x%08X (@0x%X)", (unsigned)addr, (unsigned)(addr - h.image_base + kMappingBase));
    return strf("0x%08X", (unsigned)addr);
}

}  // namespace kprobe
