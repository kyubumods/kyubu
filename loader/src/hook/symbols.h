// Data-driven symbol maps: name -> RVA + signature, verified against the running image.
#pragma once
#include "common.h"

#include <map>
#include <mutex>
#include <set>

namespace kyubu {

struct Symbol {
    std::string name;
    std::string owner;           // "kyubu" or mod id
    std::string source;          // file:line for error messages
    std::optional<uint32_t> rva;
    std::vector<int> signature;  // bytes, -1 = wildcard
    std::string convention = "cdecl";
    std::string status = "candidate";   // verified | candidate
    std::string notes;
    bool global = false;         // kind = "global": a variable anywhere in the image, never hooked

    // resolution state
    enum class State { Unresolved, Ok, Mismatch, Ambiguous, NotFound, Invalid } state = State::Unresolved;
    uintptr_t address = 0;
    std::string problem;
};

const char* symbol_state_name(Symbol::State s);

class SymbolTable {
public:
    static SymbolTable& get();

    void set_image(uintptr_t base, uint32_t preferred_base = 0x400000);
    // Loads every *.toml in dir whose `build` matches build_id. Returns number of symbols added.
    int load_dir(const std::string& dir, const std::string& build_id, const std::string& owner);
    // Parses one file (text). Errors appended to `errors`.
    int load_text(const std::string& text, const std::string& source, const std::string& build_id,
                  const std::string& owner, std::vector<std::string>& errors);

    // Resolves (and verifies on first use). Returns KY_OK, KY_E_NOT_FOUND or KY_E_UNAVAILABLE.
    KyubuResult resolve(const std::string& name, uintptr_t* out, std::string* why = nullptr);
    // Resolve without logging, for required loader symbols. Returns 0 when unavailable.
    uintptr_t addr(const std::string& name);
    const Symbol* find(const std::string& name);
    std::vector<Symbol> all();
    void verify_all();

    void clear();   // forgets every symbol (tests start from an empty map)

    uintptr_t rva_to_addr(uint32_t rva) const { return base_ + rva; }
    uint32_t addr_to_rva(uintptr_t a) const { return (uint32_t)(a - base_); }
    uintptr_t base() const { return base_; }

    // Signature helpers (public for tests)
    static std::vector<int> parse_signature(const std::string& s, bool* ok);
    // Byte of the image as it is in the file (undoing ASLR relocations and ignoring hooks).
    int original_byte(uintptr_t addr) const;
    bool match_at(uintptr_t addr, const std::vector<int>& sig) const;
    std::vector<uintptr_t> scan(const std::vector<int>& sig, size_t max_hits = 2) const;

private:
    void verify(Symbol& s);
    void load_relocations();

    std::recursive_mutex mu_;
    std::map<std::string, Symbol> syms_;
    uintptr_t base_ = 0;
    uint32_t preferred_ = 0x400000;
    std::set<uint32_t> reloc_rvas_;   // RVAs of relocated dwords
    uint32_t image_size_ = 0;
    uintptr_t text_start_ = 0;
    uint32_t text_size_ = 0;
    std::vector<uint8_t> text_copy_;   // .text as it was when set_image ran (before hooks)
};

// The loader's own hooks and the game API use only these: a symbol counts when it is `verified` in the symbol map
// and resolves on the running build.
// The address, or 0 with the reason in `why` (not in the map, candidate, or failed verification).
uintptr_t verified_symbol(const char* name, std::string* why = nullptr);
// Hooks a verified symbol with a loader-owned hook. `quiet` leaves reporting an unverified symbol to the caller.
bool hook_verified_symbol(const char* name, void* handler, void** next, bool quiet);
template <typename F>
bool hook_sym(const char* name, F* handler, F** next, bool quiet = false) {
    return hook_verified_symbol(name, (void*)handler, (void**)next, quiet);
}

}  // namespace kyubu
