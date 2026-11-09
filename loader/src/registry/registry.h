// Content registries: namespaced ids -> runtime numbers, freeze, snapshots, per-save id maps.
#pragma once
#include "common.h"

#include <map>
#include <mutex>

namespace kyubu {

struct RegEntry {
    std::string id;           // namespace:name
    std::string owner;        // mod id or "cubeworld" / "kyubu"
    std::vector<uint8_t> payload;
    uint32_t number = 0;
    bool vanilla = false;
    bool placeholder = false; // reserved number whose owner mod is missing
};

struct Registry {
    std::string name;
    std::string owner;
    uint32_t schema_version = 1;
    bool persistent = true;
    uint32_t first_mod_number = 0;           // numbers below are vanilla
    uint32_t max_number = 0xFFFFFFFFu;       // mod entries that would pass it are dropped at the freeze
    bool define_only = false;                // entries come from the registry's own definition call; add and remap refuse
    std::vector<RegEntry> entries;
    std::map<std::string, size_t> by_id;
    std::map<std::string, std::string> aliases;   // alternate id -> canonical id
    std::map<std::string, std::pair<std::string, std::vector<uint8_t>>> remaps;   // id -> (mod, payload)
    bool frozen = false;

    const RegEntry* find(const std::string& id) const;
    const RegEntry* by_number(uint32_t n) const;
};

class Registries {
public:
    static Registries& get();

    void init_builtin();   // kyubu:models (+ vanilla), kyubu:sounds, kyubu:icons, kyubu:text, kyubu:abilities (+ vanilla)

    KyubuResult create(const std::string& owner, const std::string& name, uint32_t schema, bool persistent);
    KyubuResult add(const std::string& owner, const std::string& reg, const std::string& id, const void* payload,
                    uint32_t len, uint32_t* out_number);
    // Adds an entry to a define_only registry (the path of ability_define). Same rules as add() otherwise.
    KyubuResult add_defined(const std::string& owner, const std::string& reg, const std::string& id, const void* payload,
                            uint32_t len);
    KyubuResult remap(const std::string& owner, const std::string& reg, const std::string& id, const void* payload,
                      uint32_t len);
    KyubuResult lookup(const std::string& reg, const std::string& id, uint32_t* out);
    KyubuResult name_of(const std::string& reg, uint32_t number, std::string* out);
    KyubuResult payload(const std::string& reg, const std::string& id, std::vector<uint8_t>* out);
    KyubuResult count(const std::string& reg, uint32_t* out);
    KyubuResult entry(const std::string& reg, uint32_t index, std::string* id, uint32_t* number);
    KyubuResult hash(const std::string& reg, std::string* hex);

    // Freeze all registries and allocate numbers deterministically (mod order, then id).
    void freeze(const std::function<int(const std::string&)>& order_of);
    bool frozen() const { return frozen_; }

    // Per-save id maps. apply() renumbers mod entries to match the map; returns missing mod ids.
    std::string idmap_json();
    std::vector<std::string> apply_idmap(const std::string& json_text, std::vector<std::pair<std::string, std::string>>* moved);

    std::vector<std::string> names();
    Registry* get_registry(const std::string& name);
    std::recursive_mutex& mutex() { return mu_; }

    // Canonical snapshot text of one registry (sorted "id=number" lines).
    std::string snapshot(const std::string& reg);

private:
    KyubuResult add_impl(const std::string& owner, const std::string& reg, const std::string& id, const void* payload,
                         uint32_t len, uint32_t* out_number, bool defining);

    std::recursive_mutex mu_;
    std::map<std::string, Registry> regs_;
    bool frozen_ = false;
};

bool valid_entry_name(const std::string& name);   // after the namespace

}  // namespace kyubu
