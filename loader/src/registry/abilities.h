// Ability definitions: the checks of a KyubuAbilityDef, the packed payload of a kyubu:abilities entry, and the callbacks
// that stay with the defining mod (they are not part of the payload, so both sides and every machine agree on it).
#pragma once
#include "common.h"
#include "registry.h"

#include <map>
#include <mutex>

namespace kyubu {

constexpr uint32_t kAbilityDurationMax = 60000;
constexpr uint32_t kAbilityCooldownMax = 3600000;
constexpr uint32_t kAbilityRankMax = 255;   // one byte in the payload; a cooldown per rank is kept up to here (was 20)
constexpr uint32_t kAbilityRestartsMax = 15;   // KyubuAbilityDef.restarts
constexpr uint32_t kAbilityVanillaLast = 110;   // cube::AbilityId 0..110; 111 is kyubu:placeholder

// The data part of a definition, as the payload holds it.
struct AbilityData {
    std::string name;          // text ids (empty: none)
    std::string description;
    std::string icon;          // id of a vanilla ability in kyubu:abilities (empty: none)
    std::string icon_file;     // key of a PNG in data3.db served by the mod's assets/icons/ (empty: none)
    uint32_t duration_ms = 500;
    float mp_cost = 0;
    uint32_t max_rank = 1;
    int slot = 0;              // 0: no claim, 4: claims slot 4
    int class_type = 0;        // cube::ClassType 1..4 while slot is 4
    int specialization = -1;   // -1 any, 0, 1 while slot is 4
    std::vector<uint32_t> cooldown_ms;   // max_rank + 1 values, one per rank 0..max_rank
};

// Checks `def` the way ability_define does. KY_E_INVALID_ARG names the bad field in an error line of the log (source:
// `mod`); a text id that is not in kyubu:text only gets a warning (the text may be added later in the same phase).
KyubuResult ability_check(const std::string& mod, const std::string& id, const KyubuAbilityDef* def, AbilityData* out);

// The payload: fixed field order, little endian, no pointers.
std::vector<uint8_t> ability_pack(const AbilityData& d);
bool ability_unpack(const uint8_t* p, size_t n, AbilityData* out);

// The name of cube::ClassType / cube::Specialization values, for scripts: "warrior" gives 1, "any" gives -1; -2 when the
// name is unknown. A specialization name belongs to one class ("guardian" is the warrior's 1).
int ability_class_value(const std::string& name);
int ability_specialization_value(int class_type, const std::string& name);

// What a definition keeps outside the payload, per side: the same ability is defined once per side in single-player.
struct AbilityCallbacks {
    std::string mod;
    KyubuAbilityFn on_start = nullptr, on_step = nullptr, on_end = nullptr;
    void* user = nullptr;
    // The watch callbacks (a bystander's view of another creature's run); all null for an ability that is not watched.
    KyubuAbilityFn watch_start = nullptr, watch_step = nullptr, watch_end = nullptr;
    void* watch_user = nullptr;
    uint32_t restarts = 0;   // KyubuAbilityDef.restarts: how often a reported run of it may set its time back to about 0
};

// ability_define without the lifecycle checks (api.cpp makes them): checks, packs, adds to kyubu:abilities, and keeps the
// callbacks of `side`. The same data again is KY_OK (the callbacks of that side are replaced); other data is KY_E_DUPLICATE.
// A slot claim of a pair (class, specialization) that another ability already holds is KY_E_CONFLICT, adds nothing, and the
// log names both mods; an `any` claim and a specialization claim of one class do not conflict.
KyubuResult ability_define(const std::string& mod, KyubuSide side, const std::string& id, const KyubuAbilityDef* def);

// The slot-4 resolution table (design D3), 4 classes x 2 specializations. Built once after the freeze from the claims in
// kyubu:abilities: the specialization's claim, else the class's `any`, else kyubu:placeholder when `placeholder` is true,
// else nobody. Building it again replaces the table.
void ability_slots_resolve(bool placeholder);

struct AbilitySlotHolder {
    std::string id;            // empty: nobody holds the slot
    uint32_t number = 0;
    bool placeholder = false;  // the holder is kyubu:placeholder
    int claim = -2;            // why the holder has the slot: -1 the class's `any` claim, 0 or 1 the specialization's claim, -2 no claim
};
// The holder for a class (1..4) and a specialization (0 or 1); an empty id for other values or before the table is built.
AbilitySlotHolder ability_slot_holder(int class_type, int specialization);
// True once ability_slots_resolve has run (after the freeze): tells "not built yet" from "nobody holds it".
bool ability_slots_ready();

// What the game asks the stat detours (design D5), by ability number. kyubu:placeholder (111) has no payload: its values are
// design D8's. The table is built by ability_slots_resolve and is read without a lock, because modeDuration runs about 150
// times a second: nothing here may take the registry mutex.
constexpr uint32_t kAbilityPlaceholderNumber = 111;
constexpr uint32_t kAbilityNumberMax = 255;   // current_mode is one byte
constexpr uint32_t kPlaceholderDurationMs = 500;
constexpr uint32_t kPlaceholderCooldownMs = 2000;
constexpr uint32_t kPlaceholderIcon = 54;   // cubeworld:smash: its node holds an icon in the icon map (replays of 2.6 and 4.5)
constexpr double kPlaceholderNoticeSeconds = 3.0;
struct AbilityStat {
    uint32_t duration_ms = 0;
    float mp_cost = 0;
    uint32_t max_rank = 1;
    std::vector<uint32_t> cooldown_ms;   // max_rank + 1 values
    uint32_t icon = 0;                   // the vanilla ability (1..110) whose icon the HUD shows; 0: none
    std::string icon_file;               // the mod's own icon (a data3.db key), tried before `icon`; empty: none
};
// The stat row of a number from 111 up, or nullptr (vanilla 0..110, a number nobody defined, or the table is not built).
const AbilityStat* ability_stat(uint32_t number);

// "<Class> ability 4: not made yet" (design D8); the class name is from class_type 1..4, else "Class <n>".
std::string placeholder_text(int class_type);
// Where the placeholder's text shows besides the log (the client's overlay). Null: only the log.
using NoticeFn = void (*)(const std::string& text, double seconds);
void ability_set_notice(NoticeFn fn);
// The placeholder's on_start: the text goes to the log and to the notice function.
void ability_placeholder_started(int class_type);

// Every entry of kyubu:abilities, sorted by number (the registry keeps them in the order they were added). Empty before the freeze.
std::vector<RegEntry> ability_entries();

// The lines of the console commands `ability list` (every ability: number, id, owning mod, claim) and `ability slots` (the
// holder of slot 4 for each class and specialization, and why).
std::vector<std::string> ability_list_lines();
std::vector<std::string> ability_slot_lines();

// The callbacks of an ability on a side; nullptr when that side defined none.
const AbilityCallbacks* ability_callbacks(const std::string& id, KyubuSide side);
void ability_callbacks_clear();   // tests: also empties the slot table

}  // namespace kyubu
