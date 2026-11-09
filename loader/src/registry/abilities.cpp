#include "abilities.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <memory>

#include "cubeworld/layout.h"
#include "log.h"
#include "registry.h"

namespace kyubu {

namespace {

constexpr size_t kTextIdMax = 255;
constexpr uint8_t kPayloadFormat = 2;
constexpr size_t kIconFileMax = 64;

std::mutex g_mu;
std::map<std::pair<std::string, int>, AbilityCallbacks> g_callbacks;   // (id, side)

const CwLayoutEnum* layout_enum(const char* name) {
    for (auto& e : cw_layout_enums)
        if (strcmp(e.name, name) == 0) return &e;
    return nullptr;
}

void bad(const std::string& mod, const std::string& id, const char* field, const std::string& why) {
    Log::get().write(KY_LOG_ERROR, mod, KY_SIDE_NONE, "ability " + id + ": " + field + " " + why);
}

// An optional text id of the definition: NULL or empty is none, a long one is bad, an unknown one is a warning.
bool check_text(const std::string& mod, const std::string& id, const char* field, const char* v, std::string* out) {
    out->clear();
    if (!v || !*v) return true;
    const std::string s = v;
    if (s.size() > kTextIdMax) {
        bad(mod, id, field, "is longer than " + std::to_string(kTextIdMax) + " characters");
        return false;
    }
    // by existence: lookup() answers KY_E_PHASE for a mod's entry before the freeze, and the text is added in this phase
    bool known = false;
    {
        Registries& regs = Registries::get();
        std::lock_guard<std::recursive_mutex> g(regs.mutex());
        const Registry* texts = regs.get_registry("kyubu:text");
        known = texts && texts->find(s);
    }
    if (!known)
        Log::get().write(KY_LOG_WARN, mod, KY_SIDE_NONE,
                         "ability " + id + ": " + field + " '" + s + "' is not in kyubu:text yet; the HUD shows the id");
    *out = s;
    return true;
}

void put8(std::vector<uint8_t>& b, uint32_t v) { b.push_back((uint8_t)v); }
void put16(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 2; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
void putstr(std::vector<uint8_t>& b, const std::string& s) {
    put16(b, (uint32_t)s.size());
    b.insert(b.end(), s.begin(), s.end());
}

struct Reader {
    const uint8_t* p;
    size_t n, i = 0;
    bool ok = true;
    uint32_t get(int bytes) {
        if (i + bytes > n) { ok = false; return 0; }
        uint32_t v = 0;
        for (int k = 0; k < bytes; ++k) v |= (uint32_t)p[i + k] << (8 * k);
        i += bytes;
        return v;
    }
    std::string str() {
        uint32_t len = get(2);
        if (!ok || i + len > n) { ok = false; return {}; }
        std::string s((const char*)p + i, len);
        i += len;
        return s;
    }
};

}  // namespace

int ability_class_value(const std::string& name) {
    const CwLayoutEnum* e = layout_enum("cube::ClassType");
    if (!e || !e->verified) return -2;
    for (uint32_t i = 0; i < e->value_count; ++i)
        if (lower(e->values[i].name) == lower(name)) return (int)e->values[i].value;
    return -2;
}

std::string placeholder_text(int class_type) {
    static const char* const kNames[] = {"Warrior", "Ranger", "Mage", "Rogue"};
    const std::string name = class_type >= 1 && class_type <= 4 ? kNames[class_type - 1] : "Class " + std::to_string(class_type);
    return name + " ability 4: not made yet";
}

namespace {
std::atomic<NoticeFn> g_notice{nullptr};
}

void ability_set_notice(NoticeFn fn) { g_notice.store(fn); }

void ability_placeholder_started(int class_type) {
    const std::string text = placeholder_text(class_type);
    KINFO("abilities: %s", text.c_str());
    if (NoticeFn fn = g_notice.load()) fn(text, kPlaceholderNoticeSeconds);
}

int ability_specialization_value(int class_type, const std::string& name) {
    if (lower(name) == "any") return -1;
    const CwLayoutEnum* e = layout_enum("cube::Specialization");
    if (!e || !e->verified || class_type < 1 || class_type > 4) return -2;
    // the enum lists two values per class, in class order (BERSERKER, GUARDIAN for the warrior, ...)
    for (uint32_t i = (uint32_t)(class_type - 1) * 2; i < (uint32_t)class_type * 2 && i < e->value_count; ++i)
        if (lower(e->values[i].name) == lower(name)) return (int)e->values[i].value;
    return -2;
}

KyubuResult ability_check(const std::string& mod, const std::string& id, const KyubuAbilityDef* def, AbilityData* out) {
    // `icon_file` was appended to the struct: a caller built before it passes a smaller size and has no custom icon
    if (!def || def->size < offsetof(KyubuAbilityDef, icon_file)) {
        bad(mod, id, "size", "is below the struct's");
        return KY_E_INVALID_ARG;
    }
    AbilityData d;
    if (!check_text(mod, id, "name", def->name, &d.name) || !check_text(mod, id, "description", def->description, &d.description))
        return KY_E_INVALID_ARG;
    if (def->icon && *def->icon) {
        uint32_t n = 0;
        if (Registries::get().lookup("kyubu:abilities", def->icon, &n) != KY_OK || n < 1 || n > kAbilityVanillaLast) {
            bad(mod, id, "icon", std::string("'") + def->icon + "' is not a vanilla ability");
            return KY_E_INVALID_ARG;
        }
        d.icon = def->icon;
    }
    if (def->size >= offsetof(KyubuAbilityDef, icon_file) + sizeof(def->icon_file) && def->icon_file && *def->icon_file) {
        const std::string f = def->icon_file;
        bool ok = f.size() >= 5 && f.size() <= kIconFileMax && f[0] != '.' && lower(f.substr(f.size() - 4)) == ".png";
        for (char c : f) ok = ok && (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_');
        if (!ok) {
            bad(mod, id, "icon_file", "must be a file name of 5 to " + std::to_string(kIconFileMax) +
                                          " characters ending in .png (letters, digits, '.', '-' and '_'), as in assets/icons/");
            return KY_E_INVALID_ARG;
        }
        d.icon_file = f;
    }
    if (def->duration_ms < 1 || def->duration_ms > kAbilityDurationMax) {
        bad(mod, id, "duration_ms", "must be 1 to " + std::to_string(kAbilityDurationMax));
        return KY_E_INVALID_ARG;
    }
    d.duration_ms = def->duration_ms;
    if (!(def->mp_cost >= 0.0f && def->mp_cost <= 1.0f)) {   // also refuses NaN
        bad(mod, id, "mp_cost", "must be 0 to 1");
        return KY_E_INVALID_ARG;
    }
    d.mp_cost = def->mp_cost;
    if (def->max_rank < 1 || def->max_rank > kAbilityRankMax) {
        bad(mod, id, "max_rank", "must be 1 to " + std::to_string(kAbilityRankMax));
        return KY_E_INVALID_ARG;
    }
    d.max_rank = def->max_rank;
    d.cooldown_ms.assign(d.max_rank + 1, 0);
    if (def->cooldown_ms) {
        for (uint32_t r = 0; r <= d.max_rank; ++r) {
            if (def->cooldown_ms[r] > kAbilityCooldownMax) {
                bad(mod, id, "cooldown_ms", "of rank " + std::to_string(r) + " must be 0 to " + std::to_string(kAbilityCooldownMax));
                return KY_E_INVALID_ARG;
            }
            d.cooldown_ms[r] = def->cooldown_ms[r];
        }
    }
    if (def->slot == KY_ABILITY_SLOT_FOURTH) {
        if (def->class_type < 1 || def->class_type > 4) {
            bad(mod, id, "class", "of the claim must be 1 to 4 (warrior, ranger, mage, rogue)");
            return KY_E_INVALID_ARG;
        }
        if (def->specialization < KY_ABILITY_SPEC_ANY || def->specialization > 1) {
            bad(mod, id, "specialization", "of the claim must be any, 0 or 1");
            return KY_E_INVALID_ARG;
        }
        d.slot = KY_ABILITY_SLOT_FOURTH;
        d.class_type = def->class_type;
        d.specialization = def->specialization;
    } else if (def->slot != KY_ABILITY_SLOT_NONE) {
        bad(mod, id, "slot", "must be 4 (the fourth slot) or 0 (no claim)");
        return KY_E_INVALID_ARG;
    }
    if (def->size >= offsetof(KyubuAbilityDef, restarts) + sizeof(def->restarts) && def->restarts > kAbilityRestartsMax) {
        bad(mod, id, "restarts", "must be 0 to " + std::to_string(kAbilityRestartsMax));
        return KY_E_INVALID_ARG;
    }
    if (out) *out = std::move(d);
    return KY_OK;
}

std::vector<uint8_t> ability_pack(const AbilityData& d) {
    std::vector<uint8_t> b;
    put8(b, kPayloadFormat);
    put8(b, (uint32_t)d.max_rank);
    put8(b, (uint32_t)d.slot);
    put8(b, (uint32_t)d.class_type);
    put8(b, (uint32_t)(d.specialization + 1));   // 0 any, 1 and 2 the specializations
    put32(b, d.duration_ms);
    uint32_t bits;
    static_assert(sizeof bits == sizeof d.mp_cost, "float is 32 bits");
    memcpy(&bits, &d.mp_cost, sizeof bits);
    put32(b, bits);
    for (uint32_t r = 0; r <= d.max_rank; ++r) put32(b, r < d.cooldown_ms.size() ? d.cooldown_ms[r] : 0);
    putstr(b, d.name);
    putstr(b, d.description);
    putstr(b, d.icon);
    putstr(b, d.icon_file);
    return b;
}

bool ability_unpack(const uint8_t* p, size_t n, AbilityData* out) {
    Reader r{p, n};
    AbilityData d;
    if (r.get(1) != kPayloadFormat) return false;
    d.max_rank = r.get(1);
    d.slot = (int)r.get(1);
    d.class_type = (int)r.get(1);
    d.specialization = (int)r.get(1) - 1;
    d.duration_ms = r.get(4);
    uint32_t bits = r.get(4);
    memcpy(&d.mp_cost, &bits, sizeof bits);
    if (!r.ok || d.max_rank < 1 || d.max_rank > kAbilityRankMax) return false;
    for (uint32_t k = 0; k <= d.max_rank; ++k) d.cooldown_ms.push_back(r.get(4));
    d.name = r.str();
    d.description = r.str();
    d.icon = r.str();
    d.icon_file = r.str();
    if (!r.ok || r.i != n) return false;
    if (out) *out = std::move(d);
    return true;
}

namespace {

const char* const kClassNames[] = {"", "warrior", "mage", "ranger", "rogue"};

std::string claim_name(int class_type, int specialization) {
    return std::string(kClassNames[class_type]) + (specialization < 0 ? " any" : " specialization " + std::to_string(specialization));
}

// The entry of another ability that already claims the pair of `d`: its id and owner, or false. The caller holds the
// registry mutex.
bool find_claim(Registry& reg, const std::string& id, const AbilityData& d, std::string* other_id, std::string* other_owner) {
    for (const RegEntry& e : reg.entries) {
        if (e.vanilla || e.id == id) continue;
        AbilityData o;
        if (!ability_unpack(e.payload.data(), e.payload.size(), &o)) continue;
        if (o.slot == KY_ABILITY_SLOT_FOURTH && o.class_type == d.class_type && o.specialization == d.specialization) {
            *other_id = e.id;
            *other_owner = e.owner;
            return true;
        }
    }
    return false;
}

}  // namespace

KyubuResult ability_define(const std::string& mod, KyubuSide side, const std::string& id, const KyubuAbilityDef* def) {
    AbilityData d;
    if (auto r = ability_check(mod, id, def, &d); r != KY_OK) return r;
    const std::vector<uint8_t> payload = ability_pack(d);
    Registries& regs = Registries::get();
    {
        // Held over the claim check and the add: two sides defining at once must not both pass. Only a call that
        // add_defined would accept can conflict (its phase and namespace errors come first).
        std::lock_guard<std::recursive_mutex> lock(regs.mutex());
        if (d.slot == KY_ABILITY_SLOT_FOURTH && !regs.frozen() && id.compare(0, mod.size() + 1, mod + ":") == 0) {
            Registry* reg = regs.get_registry("kyubu:abilities");
            std::string other_id, other_owner;
            if (reg && find_claim(*reg, id, d, &other_id, &other_owner)) {
                Log::get().write(KY_LOG_ERROR, mod, KY_SIDE_NONE,
                                 "ability " + id + ": slot claim (" + claim_name(d.class_type, d.specialization) + ") is held by " +
                                     other_id + " of mod " + other_owner + "; " + id + " of mod " + mod + " is not defined");
                return KY_E_CONFLICT;
            }
        }
        if (auto r = regs.add_defined(mod, "kyubu:abilities", id, payload.data(), (uint32_t)payload.size()); r != KY_OK)
            return r;
    }
    std::lock_guard<std::mutex> g(g_mu);
    AbilityCallbacks& c = g_callbacks[{id, (int)side}];
    c.mod = mod;
    c.on_start = def->on_start;
    c.on_step = def->on_step;
    c.on_end = def->on_end;
    c.user = def->user;
    const bool has_watch = def->size >= offsetof(KyubuAbilityDef, watch_user) + sizeof(def->watch_user);   // appended in 6.2
    c.watch_start = has_watch ? def->watch_start : nullptr;
    c.watch_step = has_watch ? def->watch_step : nullptr;
    c.watch_end = has_watch ? def->watch_end : nullptr;
    c.watch_user = has_watch ? def->watch_user : nullptr;
    const bool has_restarts = def->size >= offsetof(KyubuAbilityDef, restarts) + sizeof(def->restarts);   // appended in 8.1
    c.restarts = has_restarts ? def->restarts : 0;
    return KY_OK;
}

namespace {
AbilitySlotHolder g_slots[5][2];   // [class_type][specialization]; row 0 unused
bool g_slots_built = false;

// The stat rows, number kAbilityPlaceholderNumber up. A built table is never changed: readers load the pointer and use it
// without a lock. An older table stays alive (a rebuild only happens in tests) until ability_callbacks_clear.
struct StatTable {
    std::vector<AbilityStat> rows = std::vector<AbilityStat>(kAbilityNumberMax - kAbilityPlaceholderNumber + 1);
    std::vector<char> defined = std::vector<char>(kAbilityNumberMax - kAbilityPlaceholderNumber + 1, 0);
};
std::atomic<const StatTable*> g_stats{nullptr};
std::vector<std::unique_ptr<StatTable>> g_stat_tables;   // under g_mu
}

void ability_slots_resolve(bool placeholder) {
    Registries& regs = Registries::get();
    std::lock_guard<std::recursive_mutex> lock(regs.mutex());
    AbilitySlotHolder any[5], spec[5][2], none;
    Registry* reg = regs.get_registry("kyubu:abilities");
    AbilitySlotHolder ph;
    auto table = std::make_unique<StatTable>();
    auto put_stat = [&](uint32_t number, const AbilityData& d) {
        if (number < kAbilityPlaceholderNumber || number > kAbilityNumberMax) return;
        const size_t i = number - kAbilityPlaceholderNumber;
        uint32_t icon = 0;   // ability_check has made sure that a non-empty icon names a vanilla ability
        if (!d.icon.empty() && regs.lookup("kyubu:abilities", d.icon, &icon) != KY_OK) icon = 0;
        table->rows[i] = AbilityStat{d.duration_ms, d.mp_cost, d.max_rank, d.cooldown_ms, icon, d.icon_file};
        table->defined[i] = 1;
    };
    if (reg) {
        for (const RegEntry& e : reg->entries) {
            if (e.id == "kyubu:placeholder") {
                ph.id = e.id;
                ph.number = e.number;
                ph.placeholder = true;
                AbilityData pd;   // design D8: no payload, so the values are here
                pd.duration_ms = kPlaceholderDurationMs;
                pd.max_rank = 1;
                pd.cooldown_ms = {kPlaceholderCooldownMs, kPlaceholderCooldownMs};
                put_stat(e.number, pd);
                table->rows[e.number - kAbilityPlaceholderNumber].icon = kPlaceholderIcon;
                continue;
            }
            if (e.vanilla) continue;
            AbilityData d;
            if (!ability_unpack(e.payload.data(), e.payload.size(), &d)) continue;
            put_stat(e.number, d);
            if (d.slot != KY_ABILITY_SLOT_FOURTH) continue;
            if (d.class_type < 1 || d.class_type > 4 || d.specialization < -1 || d.specialization > 1) continue;
            AbilitySlotHolder h;
            h.id = e.id;
            h.number = e.number;
            h.claim = d.specialization;
            (d.specialization < 0 ? any[d.class_type] : spec[d.class_type][d.specialization]) = h;
        }
    }
    std::lock_guard<std::mutex> g(g_mu);
    for (int c = 1; c <= 4; ++c)
        for (int s = 0; s < 2; ++s)
            g_slots[c][s] = !spec[c][s].id.empty() ? spec[c][s] : !any[c].id.empty() ? any[c] : placeholder ? ph : none;
    g_slots_built = true;
    g_stats.store(table.get(), std::memory_order_release);
    g_stat_tables.push_back(std::move(table));
}

const AbilityStat* ability_stat(uint32_t number) {
    if (number < kAbilityPlaceholderNumber || number > kAbilityNumberMax) return nullptr;
    const StatTable* t = g_stats.load(std::memory_order_acquire);
    const size_t i = number - kAbilityPlaceholderNumber;
    return t && t->defined[i] ? &t->rows[i] : nullptr;
}

AbilitySlotHolder ability_slot_holder(int class_type, int specialization) {
    if (class_type < 1 || class_type > 4 || specialization < 0 || specialization > 1) return {};
    std::lock_guard<std::mutex> g(g_mu);
    return g_slots[class_type][specialization];
}

bool ability_slots_ready() {
    std::lock_guard<std::mutex> g(g_mu);
    return g_slots_built;
}

std::vector<RegEntry> ability_entries() {
    Registries& regs = Registries::get();
    std::lock_guard<std::recursive_mutex> lock(regs.mutex());
    Registry* reg = regs.get_registry("kyubu:abilities");
    if (!reg || !regs.frozen()) return {};
    std::vector<RegEntry> all = reg->entries;
    std::sort(all.begin(), all.end(), [](const RegEntry& a, const RegEntry& b) { return a.number < b.number; });
    return all;
}

std::vector<std::string> ability_list_lines() {
    const std::vector<RegEntry> all = ability_entries();
    if (all.empty()) return {"abilities are numbered when the registries freeze: not yet"};
    std::vector<std::string> lines;
    lines.push_back(fmt("%u abilities (cubeworld 0..%u, kyubu:placeholder %u, mods from %u):", (unsigned)all.size(),
                        (unsigned)kAbilityVanillaLast, (unsigned)(kAbilityVanillaLast + 1), (unsigned)(kAbilityVanillaLast + 2)));
    for (const RegEntry& e : all) {
        AbilityData d;
        const bool claims = !e.vanilla && ability_unpack(e.payload.data(), e.payload.size(), &d) && d.slot == KY_ABILITY_SLOT_FOURTH;
        lines.push_back(fmt("  %3u  %-36s %-18s %s", (unsigned)e.number, e.id.c_str(), e.owner.c_str(),
                            claims ? ("slot 4: " + claim_name(d.class_type, d.specialization)).c_str() : "-"));
    }
    return lines;
}

std::vector<std::string> ability_slot_lines() {
    if (!ability_slots_ready()) return {"the slot table is built when the registries freeze: not yet"};
    std::vector<std::string> lines;
    for (int c = 1; c <= 4; ++c)
        for (int s = 0; s < 2; ++s) {
            const AbilitySlotHolder h = ability_slot_holder(c, s);
            std::string what = "nobody";
            if (!h.id.empty()) {
                what = h.id + " (" + std::to_string(h.number) + "), ";
                what += h.placeholder ? "placeholder" : h.claim < 0 ? "claim: any" : "claim: specialization " + std::to_string(h.claim);
            }
            lines.push_back(fmt("  %-8s specialization %d: %s", kClassNames[c], s, what.c_str()));
        }
    return lines;
}

const AbilityCallbacks* ability_callbacks(const std::string& id, KyubuSide side) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_callbacks.find({id, (int)side});
    return it == g_callbacks.end() ? nullptr : &it->second;
}

void ability_callbacks_clear() {
    std::lock_guard<std::mutex> g(g_mu);
    g_callbacks.clear();
    g_stats.store(nullptr, std::memory_order_release);
    g_stat_tables.clear();
    for (auto& row : g_slots)
        for (auto& h : row) h = AbilitySlotHolder();
    g_slots_built = false;
}

}  // namespace kyubu
