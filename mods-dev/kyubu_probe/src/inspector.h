// kyubu_probe inspector: a game object shown field by field, typed by the generated headers (cubeworld/layout.h),
// with each field's mapping status (live-probes, "Object inspector"). Every read goes through the API's guarded
// mem_read, so an unmapped address shows as unreadable instead of faulting.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "kyubu_api.h"

struct CwLayoutType;
struct CwLayoutField;
struct CwLayoutEnum;

namespace kprobe {

// The layout tables the inspector uses. By default those of cubeworld/layout.h; tests pass their own.
struct Layout {
    const CwLayoutType* types = nullptr;
    size_t type_count = 0;
    const CwLayoutEnum* enums = nullptr;
    size_t enum_count = 0;

    static Layout generated();
    const CwLayoutType* find(const std::string& name) const;    // C name or mapped name
    const CwLayoutEnum* find_enum(const std::string& c_name) const;
    // The field `field` of the type `type` (mapped names), or nullptr when either isn't described.
    const CwLayoutField* field(const std::string& type, const std::string& field) const;
};

// One element of a std::map, read from its tree node: the key at node+0x10, the value after it.
struct MapEntry {
    uintptr_t node = 0;
    uint64_t key = 0;
    uint32_t value = 0;
};

struct Row {
    int depth = 0;
    std::string name;           // "pos.x", "equipment[2].level"
    std::string type;           // the mapped type
    std::string value;          // formatted, or "unreadable"
    std::string status;         // "verified", "candidate", or "" (padding)
    uint32_t offset = 0;        // from the start of the object
    uintptr_t address = 0;
    uint32_t size = 0;
    bool readable = true;
    bool editable = false;      // a scalar field (dev mode only: the probe runs only there)
    int kind = 0;               // CW_KIND_* of the value (0 for padding and object rows)
    const char* follow = nullptr;   // a pointer to a described struct: its C name
    uintptr_t target = 0;           // the pointer's value
};

class Inspector {
public:
    Inspector(const KyubuApi* api, Layout layout) : api_(api), layout_(layout) {}
    const Layout& layout() const { return layout_; }

    // The rows of the object at `addr`. The first row describes the object itself: when the object can't be read,
    // it says so and is the only row. `max_elems` limits the rows of each array; `gaps` adds hex rows for the
    // bytes no field describes.
    std::vector<Row> rows(uintptr_t addr, const CwLayoutType* type, uint32_t max_elems = 4, bool gaps = true) const;

    // The name of the field that covers `off` in `type` ("pos.x", "equipment[2].level"), or "" in a gap.
    std::string field_at(const CwLayoutType* type, uint32_t off) const;

    // Writes `text` into an editable row (numbers for integers, floats and enums, true/false, 0x... for pointers).
    bool edit(KyubuModHandle self, const Row& row, const std::string& text, std::string* err) const;

    // Copies the row's value from `src` (the same type and size, e.g. the same field of another object), plus
    // `delta` (0 for none): `inspect set pos.x from <address> plus <n>` puts one creature next to another.
    bool copy(KyubuModHandle self, const Row& row, uintptr_t src, double delta, std::string* err) const;

    // The elements of the VC110 std::map at `map` (std::map in mapping/types/vc110.toml), in key order: a
    // `key_size`-byte key at node+0x10 and a 32-bit value at node+`value_off`. The walk follows the tree as the
    // game's iterators do, with guarded reads. It fails, keeping what it read, on an unreadable node, or when the
    // number of nodes disagrees with the map's size (the game changed the map during the walk).
    bool map_entries(uintptr_t map, uint32_t key_size, uint32_t value_off, std::vector<MapEntry>* out,
                     std::string* err) const;

    // The MSVC RTTI name of the polymorphic object at `obj` (".?AVCreature@cube@@"), or "" when it has none that
    // can be read: the vtable at +0, its complete object locator at slot -1, the type descriptor at the locator's
    // +0xC, the name at the descriptor's +8.
    std::string rtti_name(uintptr_t obj) const;

private:
    void add_fields(std::vector<Row>& out, const uint8_t* obj, uintptr_t addr, uint32_t base, const CwLayoutType* t,
                    const std::string& prefix, int depth, uint32_t max_elems, bool gaps) const;
    void add_value(std::vector<Row>& out, const uint8_t* obj, uintptr_t addr, uint32_t off, const CwLayoutField& f,
                   const std::string& name, int depth, uint32_t elem_size, uint32_t max_elems, bool gaps) const;
    std::string format(const uint8_t* p, const CwLayoutField& f, uint32_t elem_size) const;

    const KyubuApi* api_;
    Layout layout_;
};

}  // namespace kprobe
