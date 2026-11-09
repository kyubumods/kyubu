#include "registry.h"

#include <algorithm>
#include <set>

#include "json.hpp"
#include "log.h"
#include "sha256.h"

namespace kyubu {

namespace {
struct VanillaModel { uint32_t slot; const char* key; };
const VanillaModel kVanillaModels[] = {
#include "vanilla_models.inc"
};
struct VanillaAbility { uint32_t number; const char* name; };
const VanillaAbility kVanillaAbilities[] = {
#include "vanilla_abilities.inc"
};
constexpr uint32_t kPlaceholderAbility = 111;   // Kyubu's own, fixed so the cap can never drop it
constexpr uint32_t kMaxAbility = 255;           // current_mode is one byte
}  // namespace

bool valid_entry_name(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/' || c == '.' || c == '#'))
            return false;
    return true;
}

const RegEntry* Registry::find(const std::string& id) const {
    auto it = by_id.find(id);
    if (it == by_id.end()) {
        auto a = aliases.find(id);
        if (a == aliases.end()) return nullptr;
        it = by_id.find(a->second);
        if (it == by_id.end()) return nullptr;
    }
    return &entries[it->second];
}

const RegEntry* Registry::by_number(uint32_t n) const {
    for (auto& e : entries) if (e.number == n) return &e;
    return nullptr;
}

Registries& Registries::get() {
    static Registries* r = new Registries();
    return *r;
}

void Registries::init_builtin() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (regs_.count("kyubu:models")) return;
    auto make = [&](const char* name) -> Registry& {
        Registry& r = regs_[name];
        r.name = name;
        r.owner = "kyubu";
        return r;
    };
    Registry& models = make("kyubu:models");
    // Vanilla model slots: files loaded into several slots get "#slot" ids for every slot and the
    // plain id as an alias of the lowest slot.
    std::map<std::string, std::vector<uint32_t>> slots_of;
    for (auto& v : kVanillaModels) slots_of[v.key].push_back(v.slot);
    uint32_t max_slot = 0;
    for (auto& v : kVanillaModels) {
        std::string key = v.key;
        std::string stem = lower(key.substr(0, key.size() - 4));
        std::string id = "cubeworld:" + stem;
        auto& slots = slots_of[key];
        if (slots.size() > 1) id += "#" + std::to_string(v.slot);
        RegEntry e;
        e.id = id;
        e.owner = "cubeworld";
        e.payload.assign(key.begin(), key.end());
        e.number = v.slot;
        e.vanilla = true;
        if (models.by_id.count(id)) continue;
        models.by_id[id] = models.entries.size();
        models.entries.push_back(e);
        if (slots.size() > 1 && v.slot == *std::min_element(slots.begin(), slots.end()))
            models.aliases["cubeworld:" + stem] = id;
        max_slot = std::max(max_slot, v.slot);
    }
    models.first_mod_number = 2569;   // the game's vector holds slots 0..2568
    // The placeholder for content whose mod is missing.
    RegEntry miss;
    miss.id = "kyubu:missing";
    miss.owner = "kyubu";
    std::string mk = "kyubu-missing.cub";
    miss.payload.assign(mk.begin(), mk.end());
    models.by_id[miss.id] = models.entries.size();
    models.entries.push_back(miss);

    for (const char* n : {"kyubu:sounds", "kyubu:icons", "kyubu:text"}) make(n);

    // Abilities: vanilla 0..110 for look-ups, the placeholder on 111 (both have fixed numbers, so both count as
    // vanilla here), mods from 112 to 255. No per-save id map: numbers are assigned again at every start.
    Registry& ab = make("kyubu:abilities");
    ab.persistent = false;
    ab.define_only = true;
    ab.first_mod_number = kPlaceholderAbility + 1;
    ab.max_number = kMaxAbility;
    auto add_fixed = [&](const std::string& id, const char* owner, uint32_t number) {
        RegEntry e;
        e.id = id;
        e.owner = owner;
        e.number = number;
        e.vanilla = true;
        ab.by_id[id] = ab.entries.size();
        ab.entries.push_back(std::move(e));
    };
    for (auto& v : kVanillaAbilities) add_fixed(std::string("cubeworld:") + v.name, "cubeworld", v.number);
    add_fixed("kyubu:placeholder", "kyubu", kPlaceholderAbility);
}

KyubuResult Registries::create(const std::string& owner, const std::string& name, uint32_t schema, bool persistent) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (frozen_) return KY_E_PHASE;
    auto colon = name.find(':');
    if (colon == std::string::npos || name.substr(0, colon) != owner || !valid_entry_name(name.substr(colon + 1)))
        return KY_E_NAMESPACE;
    if (auto it = regs_.find(name); it != regs_.end()) {
        // Creating the same registry again from the owner's other side is fine; anything else is a clash.
        const Registry& e = it->second;
        return e.owner == owner && e.schema_version == schema && e.persistent == persistent ? KY_OK : KY_E_DUPLICATE;
    }
    Registry& r = regs_[name];
    r.name = name;
    r.owner = owner;
    r.schema_version = schema;
    r.persistent = persistent;
    return KY_OK;
}

KyubuResult Registries::add(const std::string& owner, const std::string& reg, const std::string& id,
                            const void* payload, uint32_t len, uint32_t* out_number) {
    return add_impl(owner, reg, id, payload, len, out_number, false);
}

KyubuResult Registries::add_defined(const std::string& owner, const std::string& reg, const std::string& id,
                                    const void* payload, uint32_t len) {
    return add_impl(owner, reg, id, payload, len, nullptr, true);
}

KyubuResult Registries::add_impl(const std::string& owner, const std::string& reg, const std::string& id,
                                 const void* payload, uint32_t len, uint32_t* out_number, bool defining) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (frozen_) return KY_E_PHASE;
    auto it = regs_.find(reg);
    if (it == regs_.end()) return KY_E_NOT_FOUND;
    if (it->second.define_only && !defining) {
        Log::get().write(KY_LOG_ERROR, owner, KY_SIDE_NONE,
                         "registry " + reg + " takes entries only from its definition call (ability_define); add of " + id + " refused");
        return KY_E_INVALID_ARG;
    }
    auto colon = id.find(':');
    if (colon == std::string::npos || !valid_entry_name(id.substr(colon + 1))) return KY_E_INVALID_ARG;
    if (id.substr(0, colon) != owner) return KY_E_NAMESPACE;
    Registry& r = it->second;
    if (auto e = r.by_id.find(id); e != r.by_id.end()) {
        // Registries are process-wide; code that runs once per side (single-player) may register twice.
        const RegEntry& x = r.entries[e->second];
        std::vector<uint8_t> p = payload && len ? std::vector<uint8_t>((const uint8_t*)payload, (const uint8_t*)payload + len)
                                                : std::vector<uint8_t>();
        return x.owner == owner && x.payload == p ? KY_OK : KY_E_DUPLICATE;
    }
    if (r.aliases.count(id)) return KY_E_DUPLICATE;
    RegEntry e;
    e.id = id;
    e.owner = owner;
    if (payload && len) e.payload.assign((const uint8_t*)payload, (const uint8_t*)payload + len);
    r.by_id[id] = r.entries.size();
    r.entries.push_back(std::move(e));
    if (out_number) *out_number = 0;   // numbers are assigned at freeze
    return KY_OK;
}

KyubuResult Registries::remap(const std::string& owner, const std::string& reg, const std::string& id,
                              const void* payload, uint32_t len) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (frozen_) return KY_E_PHASE;
    auto it = regs_.find(reg);
    if (it == regs_.end()) return KY_E_NOT_FOUND;
    if (it->second.define_only) {
        Log::get().write(KY_LOG_ERROR, owner, KY_SIDE_NONE,
                         "registry " + reg + " has no remap; define abilities with the ability call (remap of " + id + " refused)");
        return KY_E_INVALID_ARG;
    }
    const RegEntry* e = it->second.find(id);
    if (!e) return KY_E_NOT_FOUND;
    std::vector<uint8_t> p;
    if (payload && len) p.assign((const uint8_t*)payload, (const uint8_t*)payload + len);
    auto& rm = it->second.remaps;
    if (auto prev = rm.find(e->id); prev != rm.end() && prev->second.first != owner)
        Log::get().write(KY_LOG_INFO, owner, KY_SIDE_NONE,
                         "remap of " + e->id + " overrides the remap by " + prev->second.first + " (later mod wins)");
    rm[e->id] = {owner, p};
    return KY_OK;
}

void Registries::freeze(const std::function<int(const std::string&)>& order_of) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (frozen_) return;
    for (auto& [name, r] : regs_) {
        std::vector<size_t> mod_entries;
        for (size_t i = 0; i < r.entries.size(); ++i) if (!r.entries[i].vanilla) mod_entries.push_back(i);
        std::sort(mod_entries.begin(), mod_entries.end(), [&](size_t a, size_t b) {
            int oa = order_of(r.entries[a].owner), ob = order_of(r.entries[b].owner);
            if (oa != ob) return oa < ob;
            return r.entries[a].id < r.entries[b].id;
        });
        uint32_t next = r.first_mod_number;
        std::vector<size_t> over_cap;
        for (size_t i : mod_entries) {
            if (next > r.max_number) { over_cap.push_back(i); continue; }
            r.entries[i].number = next++;
        }
        if (!over_cap.empty()) {
            // Left out: look-ups of them fail as not found.
            std::set<size_t> drop(over_cap.begin(), over_cap.end());
            for (size_t i : over_cap)
                Log::get().write(KY_LOG_ERROR, r.entries[i].owner, KY_SIDE_NONE,
                                 "registry " + name + " is full (numbers up to " + std::to_string(r.max_number) + "): " +
                                     r.entries[i].id + " is left out");
            std::vector<RegEntry> kept;
            for (size_t i = 0; i < r.entries.size(); ++i) if (!drop.count(i)) kept.push_back(std::move(r.entries[i]));
            r.entries = std::move(kept);
            r.by_id.clear();
            for (size_t i = 0; i < r.entries.size(); ++i) r.by_id[r.entries[i].id] = i;
        }
        // apply remaps to payloads
        for (auto& [id, rp] : r.remaps) {
            auto it = r.by_id.find(id);
            if (it != r.by_id.end()) r.entries[it->second].payload = rp.second;
        }
        r.frozen = true;
    }
    frozen_ = true;
}

KyubuResult Registries::lookup(const std::string& reg, const std::string& id, uint32_t* out) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = regs_.find(reg);
    if (it == regs_.end()) return KY_E_NOT_FOUND;
    const RegEntry* e = it->second.find(id);
    if (!e) return KY_E_NOT_FOUND;
    if (!e->vanilla && !frozen_) return KY_E_PHASE;
    if (out) *out = e->number;
    return KY_OK;
}

KyubuResult Registries::name_of(const std::string& reg, uint32_t number, std::string* out) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = regs_.find(reg);
    if (it == regs_.end()) return KY_E_NOT_FOUND;
    for (auto& e : it->second.entries)
        if (e.number == number && (e.vanilla || frozen_)) { *out = e.id; return KY_OK; }
    return KY_E_NOT_FOUND;
}

KyubuResult Registries::payload(const std::string& reg, const std::string& id, std::vector<uint8_t>* out) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = regs_.find(reg);
    if (it == regs_.end()) return KY_E_NOT_FOUND;
    const RegEntry* e = it->second.find(id);
    if (!e) return KY_E_NOT_FOUND;
    *out = e->payload;
    if (!frozen_) if (auto rm = it->second.remaps.find(e->id); rm != it->second.remaps.end()) *out = rm->second.second;
    return KY_OK;
}

KyubuResult Registries::count(const std::string& reg, uint32_t* out) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = regs_.find(reg);
    if (it == regs_.end()) return KY_E_NOT_FOUND;
    *out = (uint32_t)it->second.entries.size();
    return KY_OK;
}

KyubuResult Registries::entry(const std::string& reg, uint32_t index, std::string* id, uint32_t* number) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = regs_.find(reg);
    if (it == regs_.end()) return KY_E_NOT_FOUND;
    if (index >= it->second.entries.size()) return KY_E_NOT_FOUND;
    auto& e = it->second.entries[index];
    *id = e.id;
    *number = e.number;
    return KY_OK;
}

std::string Registries::snapshot(const std::string& reg) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = regs_.find(reg);
    if (it == regs_.end()) return {};
    std::vector<std::pair<std::string, uint32_t>> v;
    for (auto& e : it->second.entries) v.push_back({e.id, e.number});
    std::sort(v.begin(), v.end());
    std::string s;
    for (auto& [id, n] : v) s += id + "=" + std::to_string(n) + "\n";
    return s;
}

KyubuResult Registries::hash(const std::string& reg, std::string* hex) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!regs_.count(reg)) return KY_E_NOT_FOUND;
    std::string s = snapshot(reg);
    *hex = Sha256::hex_of(s.data(), s.size());
    return KY_OK;
}

std::vector<std::string> Registries::names() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<std::string> v;
    for (auto& [k, r] : regs_) v.push_back(k);
    return v;
}

Registry* Registries::get_registry(const std::string& name) {
    auto it = regs_.find(name);
    return it == regs_.end() ? nullptr : &it->second;
}

std::string Registries::idmap_json() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    nlohmann::json j;
    j["format"] = 1;
    for (auto& [name, r] : regs_) {
        if (!r.persistent) continue;
        nlohmann::json m = nlohmann::json::object();
        for (auto& e : r.entries) if (!e.vanilla) m[e.id] = e.number;
        j["registries"][name] = m;
    }
    return j.dump(2);
}

std::vector<std::string> Registries::apply_idmap(const std::string& text,
                                                 std::vector<std::pair<std::string, std::string>>* moved) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::set<std::string> missing_mods;
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.contains("registries")) return {};
    for (auto& [name, r] : regs_) {
        if (!r.persistent || !j["registries"].contains(name)) continue;
        auto& m = j["registries"][name];
        // Drop placeholders from a previous save, keep real entries.
        r.entries.erase(std::remove_if(r.entries.begin(), r.entries.end(), [](const RegEntry& e) { return e.placeholder; }),
                        r.entries.end());
        std::set<uint32_t> used;
        for (auto& e : r.entries) if (e.vanilla) used.insert(e.number);
        // 1. entries named in the map keep their saved numbers
        for (auto& e : r.entries) {
            if (e.vanilla) continue;
            if (m.contains(e.id)) {
                uint32_t n = m[e.id].get<uint32_t>();
                if (n != e.number && moved) moved->push_back({e.id, std::to_string(e.number) + "->" + std::to_string(n)});
                e.number = n;
                used.insert(n);
            }
        }
        // 2. ids in the map without an entry: reserve as placeholders
        for (auto& [id, num] : m.items()) {
            if (r.by_id.count(id)) continue;
            RegEntry p;
            p.id = id;
            p.owner = id.substr(0, id.find(':'));
            p.number = num.get<uint32_t>();
            p.placeholder = true;
            used.insert(p.number);
            r.entries.push_back(p);
            missing_mods.insert(p.owner);
        }
        // 3. new entries get numbers not used by the map
        uint32_t next = r.first_mod_number;
        for (auto& e : r.entries) {
            if (e.vanilla || e.placeholder || m.contains(e.id)) continue;
            while (used.count(next)) ++next;
            if (e.number != next && moved) moved->push_back({e.id, std::to_string(e.number) + "->" + std::to_string(next)});
            e.number = next;
            used.insert(next++);
        }
        r.by_id.clear();
        for (size_t i = 0; i < r.entries.size(); ++i) r.by_id[r.entries[i].id] = i;
    }
    return std::vector<std::string>(missing_mods.begin(), missing_mods.end());
}

}  // namespace kyubu
