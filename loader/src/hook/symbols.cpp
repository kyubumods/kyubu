#include "symbols.h"

#include "guard.h"
#include "hooks.h"
#include "host.h"
#include "log.h"
#include "toml_config.h"

namespace kyubu {

const char* symbol_state_name(Symbol::State s) {
    switch (s) {
        case Symbol::State::Unresolved: return "unresolved";
        case Symbol::State::Ok: return "ok";
        case Symbol::State::Mismatch: return "mismatch";
        case Symbol::State::Ambiguous: return "ambiguous";
        case Symbol::State::NotFound: return "not found";
        case Symbol::State::Invalid: return "invalid";
    }
    return "?";
}

SymbolTable& SymbolTable::get() {
    static SymbolTable* t = new SymbolTable();
    return *t;
}

void SymbolTable::set_image(uintptr_t base, uint32_t preferred) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    base_ = base;
    preferred_ = preferred;
    auto nt = nt_headers(base);
    image_size_ = nt ? nt->OptionalHeader.SizeOfImage : 0;
    section_range(base, ".text", &text_start_, &text_size_);
    // Snapshot the code before any hook is installed, so signatures keep matching hooked functions.
    text_copy_.assign((const uint8_t*)text_start_, (const uint8_t*)text_start_ + text_size_);
    load_relocations();
}

void SymbolTable::load_relocations() {
    reloc_rvas_.clear();
    auto nt = nt_headers(base_);
    if (!nt) return;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!dir.VirtualAddress || !dir.Size) return;
    auto p = (const uint8_t*)(base_ + dir.VirtualAddress);
    auto end = p + dir.Size;
    while (p + 8 <= end) {
        auto blk = (const IMAGE_BASE_RELOCATION*)p;
        if (blk->SizeOfBlock < 8) break;
        auto entries = (const uint16_t*)(p + 8);
        size_t n = (blk->SizeOfBlock - 8) / 2;
        for (size_t i = 0; i < n; ++i) {
            if ((entries[i] >> 12) == IMAGE_REL_BASED_HIGHLOW) reloc_rvas_.insert(blk->VirtualAddress + (entries[i] & 0xFFF));
        }
        p += blk->SizeOfBlock;
    }
}

int SymbolTable::original_byte(uintptr_t addr) const {
    uint8_t b;
    const bool in_text = addr >= text_start_ && addr < text_start_ + text_size_ && !text_copy_.empty();
    if (in_text) b = text_copy_[addr - text_start_];   // pristine copy: hooks do not change it
    else if (!safe_read(addr, &b, 1)) return -1;
    if (base_ == preferred_ || reloc_rvas_.empty()) return b;
    uint32_t rva = (uint32_t)(addr - base_);
    // Is this byte part of a relocated dword starting at rva-0..3?
    for (uint32_t k = 0; k < 4; ++k) {
        if (rva < k) break;
        if (reloc_rvas_.count(rva - k)) {
            uint32_t v;
            if (!text_copy_.empty() && addr - k >= text_start_ && addr - k + 4 <= text_start_ + text_size_)
                memcpy(&v, text_copy_.data() + (addr - k - text_start_), 4);
            else if (!safe_read(addr - k, &v, 4)) return -1;
            v -= (uint32_t)(base_ - preferred_);
            return (v >> (8 * k)) & 0xFF;
        }
    }
    return b;
}

bool SymbolTable::match_at(uintptr_t addr, const std::vector<int>& sig) const {
    for (size_t i = 0; i < sig.size(); ++i) {
        if (sig[i] < 0) continue;
        if (original_byte(addr + i) != sig[i]) return false;
    }
    return !sig.empty();
}

std::vector<uintptr_t> SymbolTable::scan(const std::vector<int>& sig, size_t max_hits) const {
    std::vector<uintptr_t> hits;
    if (sig.empty() || !text_size_) return hits;
    // Anchor on the first non-wildcard byte that is not inside a relocation for speed.
    size_t anchor = 0;
    while (anchor < sig.size() && sig[anchor] < 0) ++anchor;
    if (anchor == sig.size()) return hits;
    auto p = text_copy_.empty() ? (const uint8_t*)text_start_ : text_copy_.data();
    for (uint32_t i = 0; i + sig.size() <= text_size_; ++i) {
        if (base_ == preferred_ && p[i + anchor] != sig[anchor]) continue;
        if (match_at(text_start_ + i, sig)) {
            hits.push_back(text_start_ + i);
            if (hits.size() >= max_hits) break;
        }
    }
    return hits;
}

std::vector<int> SymbolTable::parse_signature(const std::string& s, bool* ok) {
    std::vector<int> out;
    *ok = true;
    for (auto& tok : split(s, ' ')) {
        std::string t = trim(tok);
        if (t.empty()) continue;
        if (t == "?" || t == "??") { out.push_back(-1); continue; }
        if (t.size() != 2 || !isxdigit((unsigned char)t[0]) || !isxdigit((unsigned char)t[1])) { *ok = false; return {}; }
        out.push_back((int)strtoul(t.c_str(), nullptr, 16));
    }
    return out;
}

int SymbolTable::load_text(const std::string& text, const std::string& source, const std::string& build_id,
                           const std::string& owner, std::vector<std::string>& errors) {
    auto res = toml::parse(text, source);
    if (!res) { errors.push_back(fmt_toml_error(res.error())); return 0; }
    auto& t = res.table();
    std::string build = t["build"].value_or(std::string());
    if (build != build_id) return 0;
    int added = 0;
    std::lock_guard<std::recursive_mutex> g(mu_);
    // [[symbol]] holds functions and sites; [[global]] holds variables (as does a [[symbol]] with kind = "global").
    // The 1.0.1 loader reads only [[symbol]], so it skips the globals quietly.
    for (const char* key : {"symbol", "global"}) {
        auto arr = t[key].as_array();
        if (!arr) continue;
        const bool global_array = std::string(key) == "global";
        for (auto& node : *arr) {
            auto st = node.as_table();
            if (!st) continue;
            Symbol s;
            s.owner = owner;
            s.source = source + ":" + std::to_string(st->source().begin.line);
            s.name = (*st)["name"].value_or(std::string());
            if (s.name.empty()) { errors.push_back(s.source + ": symbol without name"); continue; }
            if (owner != "kyubu" && !starts_with(s.name, owner + ":")) {
                errors.push_back(s.source + ": mod symbols must be named '" + owner + ":<Name>' (got '" + s.name + "')");
                continue;
            }
            if (auto r = (*st)["rva"].value<int64_t>()) s.rva = (uint32_t)*r;
            else if (auto a = (*st)["address"].value<int64_t>()) s.rva = (uint32_t)(*a - preferred_);
            std::string sig = (*st)["signature"].value_or(std::string());
            if (!sig.empty()) {
                bool ok;
                s.signature = parse_signature(sig, &ok);
                if (!ok) { errors.push_back(s.source + ": malformed signature for " + s.name); continue; }
            }
            s.global = global_array || (*st)["kind"].value_or(std::string("function")) == "global";
            if (s.global && !s.rva) { errors.push_back(s.source + ": global " + s.name + " needs rva/address"); continue; }
            if (!s.rva && s.signature.empty()) { errors.push_back(s.source + ": " + s.name + " needs rva/address or signature"); continue; }
            s.convention = (*st)["convention"].value_or(std::string("cdecl"));
            s.status = (*st)["status"].value_or(std::string("candidate"));
            s.notes = (*st)["notes"].value_or(std::string());
            if (s.status != "verified" && s.status != "candidate") {
                errors.push_back(s.source + ": status must be 'verified' or 'candidate'");
                continue;
            }
            auto it = syms_.find(s.name);
            if (it != syms_.end()) {
                errors.push_back(s.source + ": duplicate symbol " + s.name + " (first defined at " + it->second.source + ", kept)");
                continue;
            }
            syms_.emplace(s.name, std::move(s));
            ++added;
        }
    }
    return added;
}

int SymbolTable::load_dir(const std::string& dir, const std::string& build_id, const std::string& owner) {
    int total = 0;
    for (auto& f : list_dir(dir)) {
        if (!ends_with(lower(f), ".toml")) continue;
        std::string path = join_path(dir, f);
        auto data = read_file(path);
        if (!data) continue;
        std::vector<std::string> errors;
        int n = load_text(std::string(data->begin(), data->end()), path, build_id, owner, errors);
        for (auto& e : errors) Log::get().write(KY_LOG_ERROR, owner, KY_SIDE_NONE, "symbol map: " + e);
        total += n;
    }
    return total;
}

void SymbolTable::verify(Symbol& s) {
    if (s.state != Symbol::State::Unresolved) return;
    if (s.global) {   // data has no signature: the address only has to lie inside the image
        if (!image_size_ || *s.rva >= image_size_) {
            s.state = Symbol::State::Invalid;
            s.problem = "address " + hex(*s.rva + preferred_) + " is outside the image";
            return;
        }
        s.address = base_ + *s.rva;
        s.state = Symbol::State::Ok;
        return;
    }
    if (s.rva) {
        uintptr_t a = base_ + *s.rva;
        if (!text_size_ || a < text_start_ || a >= text_start_ + text_size_) {
            s.state = Symbol::State::Invalid;
            s.problem = "address " + hex(*s.rva + preferred_) + " is outside the code section";
            return;
        }
        if (!s.signature.empty() && !match_at(a, s.signature)) {
            s.state = Symbol::State::Mismatch;
            std::string actual;
            for (size_t i = 0; i < s.signature.size(); ++i) {
                int b = original_byte(a + i);
                actual += fmt("%02X ", b < 0 ? 0 : b);
            }
            s.problem = "signature mismatch at " + hex(*s.rva + preferred_) + "; actual bytes: " + trim(actual);
            return;
        }
        s.address = a;
        s.state = Symbol::State::Ok;
        return;
    }
    auto hits = scan(s.signature, 2);
    if (hits.empty()) { s.state = Symbol::State::NotFound; s.problem = "signature not found in the code section"; }
    else if (hits.size() > 1) {
        s.state = Symbol::State::Ambiguous;
        s.problem = "signature matches more than once (" + hex((uint32_t)(hits[0] - base_ + preferred_)) + ", " +
                    hex((uint32_t)(hits[1] - base_ + preferred_)) + ")";
    } else { s.address = hits[0]; s.state = Symbol::State::Ok; }
}

KyubuResult SymbolTable::resolve(const std::string& name, uintptr_t* out, std::string* why) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = syms_.find(name);
    if (it == syms_.end()) { if (why) *why = "no such symbol"; return KY_E_NOT_FOUND; }
    Symbol& s = it->second;
    bool first = s.state == Symbol::State::Unresolved;
    verify(s);
    if (s.state != Symbol::State::Ok) {
        if (first) KERROR("symbol %s (%s) unavailable: %s", s.name.c_str(), s.source.c_str(), s.problem.c_str());
        if (why) *why = s.problem;
        return KY_E_UNAVAILABLE;
    }
    if (out) *out = s.address;
    return KY_OK;
}

uintptr_t SymbolTable::addr(const std::string& name) {
    uintptr_t a = 0;
    return resolve(name, &a) == KY_OK ? a : 0;
}

const Symbol* SymbolTable::find(const std::string& name) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = syms_.find(name);
    return it == syms_.end() ? nullptr : &it->second;
}

std::vector<Symbol> SymbolTable::all() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<Symbol> v;
    for (auto& [k, s] : syms_) v.push_back(s);
    return v;
}

void SymbolTable::clear() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    syms_.clear();
}

void SymbolTable::verify_all() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (auto& [k, s] : syms_) {
        bool first = s.state == Symbol::State::Unresolved;
        verify(s);
        if (first && s.state != Symbol::State::Ok) {
            if (s.status == "verified")
                KERROR("verified symbol %s failed verification: %s", s.name.c_str(), s.problem.c_str());
            else
                KWARN("candidate symbol %s: %s", s.name.c_str(), s.problem.c_str());
        }
    }
}

uintptr_t verified_symbol(const char* n, std::string* why) {
    auto& t = SymbolTable::get();
    const Symbol* s = t.find(n);
    if (!s) { if (why) *why = "not in the symbol map"; return 0; }
    if (s->status != "verified") { if (why) *why = s->status + ", not verified"; return 0; }
    uintptr_t a = 0;
    std::string problem;
    if (t.resolve(n, &a, &problem) != KY_OK) { if (why) *why = problem; return 0; }
    return a;
}

bool hook_verified_symbol(const char* sym, void* handler, void** next, bool quiet) {
    uintptr_t a = verified_symbol(sym);
    if (!a) {
        if (!quiet) KWARN("%s is not a verified symbol on this build; related events are unavailable", sym);
        return false;
    }
    std::string err;
    if (Hooks::get().hook_function("kyubu", a, handler, kLoaderPriority, next, nullptr, &err) != KY_OK) {
        KERROR("cannot hook %s: %s", sym, err.c_str());
        return false;
    }
    return true;
}

}  // namespace kyubu
