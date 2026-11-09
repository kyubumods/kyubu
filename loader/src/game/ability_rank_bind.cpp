// The ranks file bound to the played character (design D9). One AbilityRanks for the process: the file is read at the
// first use, a rank change writes it at once, and the selection follows load_character and save_character.
#include "ability_rank_bind.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "abilities.h"
#include "ability_ranks.h"
#include "ability_skilltree.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "loader.h"
#include "registry.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

constexpr const char* kLoad = "load_character";
constexpr const char* kSave = "save_character";
constexpr const char* kController = "cube::game_controller";
constexpr const char* kFile = "characters.kyubu-abilities.json";

// void load_character(GameController* this, int slot, Creature* creature) and void save_character(GameController*, int slot,
// Creature* player): thiscall, `ret 8`. __fastcall with an unused EDX has the thiscall ABI.
using CharacterFn = void(__fastcall*)(cube_GameController*, void*, int32_t, cube_Creature*);

CharacterFn g_load_next = nullptr;
CharacterFn g_save_next = nullptr;
KyubuId g_hooks[2] = {0, 0};
uintptr_t g_controller_global = 0;

std::mutex g_mu;            // guards the lazy load and the path
std::string g_path_for_test;
std::atomic<bool> g_loaded{false};

AbilityRanks& store() {
    static AbilityRanks* s = new AbilityRanks();   // lives to the end of the process: the detours may run during exit
    return *s;
}

// The store with its file read: once, at the first use.
AbilityRanks& loaded_store() {
    AbilityRanks& s = store();
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_loaded) {
        s.load(g_path_for_test.empty() ? join_path(join_path(L().game_dir, "Save"), kFile) : g_path_for_test);
        g_loaded = true;
    }
    return s;
}

bool ranks_available(std::string* why) { return available(Prim::ability_ranks, why); }

std::string player_name(const cube_Creature* c) {
    const char* n = c->data.name;
    return std::string(n, strnlen(n, sizeof c->data.name));
}

bool is_player(const cube_Creature* c) {
    const cube_GameController* gc = g_controller_global ? *(cube_GameController* const*)g_controller_global : nullptr;
    return gc && c == gc->player;
}

void __fastcall load_detour(cube_GameController* self, void* edx, int32_t slot, cube_Creature* creature) {
    g_load_next(self, edx, slot, creature);
    if (is_player(creature)) ability_rank_character(slot, creature);
}

void __fastcall save_detour(cube_GameController* self, void* edx, int32_t slot, cube_Creature* player) {
    g_save_next(self, edx, slot, player);
    if (is_player(player)) ability_rank_character(slot, player);
}

// The id and stat of a mod ability's number, or the refusal.
const AbilityStat* mod_ability(uint32_t number, std::string* id, std::string* why) {
    const AbilityStat* stat = ability_stat(number);
    if (!stat) {
        if (why) *why = fmt("%u is not a defined mod ability (vanilla abilities have no rank)", number);
        return nullptr;
    }
    if (id) {
        id->clear();
        Registries::get().name_of("kyubu:abilities", number, id);
    }
    return stat;
}

}  // namespace

void install_ability_rank_hooks() {
    if (g_hooks[0] || g_hooks[1]) return;
    std::string why;
    if (!ranks_available(&why)) return;   // game.cpp's check_entries has logged why
    const uintptr_t controller = verified_symbol(kController, &why);
    const uintptr_t load = controller ? verified_symbol(kLoad, &why) : 0;
    const uintptr_t save = load ? verified_symbol(kSave, &why) : 0;
    std::string err = why;
    g_controller_global = controller;
    if (!controller || !load || !save ||
        Hooks::get().hook_function("kyubu", load, (void*)&load_detour, kLoaderPriority, (void**)&g_load_next, &g_hooks[0], &err) != KY_OK ||
        Hooks::get().hook_function("kyubu", save, (void*)&save_detour, kLoaderPriority, (void**)&g_save_next, &g_hooks[1], &err) != KY_OK) {
        remove_ability_rank_hooks();
        KERROR("abilities: the ranks are off: cannot detour %s and %s: %s", kLoad, kSave, err.c_str());
        return;
    }
    KINFO("abilities: the rank detours are installed (%s, %s)", kLoad, kSave);
}

void remove_ability_rank_hooks() {
    for (KyubuId& h : g_hooks) {
        if (h) Hooks::get().remove("kyubu", h);
        h = 0;
    }
    g_load_next = nullptr;
    g_save_next = nullptr;
    g_controller_global = 0;
}

void ability_rank_character(int32_t slot, const cube_Creature* creature) {
    if (!creature || slot < 0) return;
    loaded_store().select(slot, player_name(creature));
}

void ability_rank_world_exit() {
    if (!g_loaded) return;
    AbilityRanks& s = store();
    if (s.dirty()) s.save();
}

uint32_t ability_rank_stored(uint32_t number) {
    if (!g_loaded || !ranks_available(nullptr)) return 1;   // nothing was read, so nobody is selected
    const AbilityStat* stat = ability_stat(number);
    if (!stat) return 1;
    std::string id;
    if (Registries::get().name_of("kyubu:abilities", number, &id) != KY_OK) return 1;
    return store().get(id, stat->max_rank);
}

KyubuResult ability_rank_read(uint32_t number, uint32_t* out, std::string* why) {
    std::string w;
    uint32_t tree = 0;
    if (skilltree_rank_get(number, &tree)) {   // the player's slot-4 holder: its rank is skills[9] (design D13)
        if (out) *out = tree;
        return KY_OK;
    }
    if (!ranks_available(&w)) {
        if (why) *why = "the ability ranks are unavailable on this build: " + w;
        return KY_E_UNAVAILABLE;
    }
    std::string id;
    const AbilityStat* stat = mod_ability(number, &id, &w);
    if (!stat) {
        if (why) *why = w;
        return KY_E_INVALID_ARG;
    }
    AbilityRanks& s = loaded_store();
    if (!s.selected()) {
        if (why) *why = "no character is selected yet: every rank reads 1 until one is played";
        if (out) *out = std::min<uint32_t>(1, stat->max_rank);
        return KY_E_REFUSED;
    }
    if (out) *out = s.get(id, stat->max_rank);
    return KY_OK;
}

KyubuResult ability_rank_write(uint32_t number, uint32_t rank, uint32_t* out, std::string* why) {
    std::string w;
    uint32_t tree = 0;
    if (skilltree_rank_set(number, rank, &tree)) {   // skills[9] is the game's own save field: no file write
        if (out) *out = tree;
        return KY_OK;
    }
    if (!ranks_available(&w)) {
        if (why) *why = "the ability ranks are unavailable on this build: " + w;
        return KY_E_UNAVAILABLE;
    }
    std::string id;
    const AbilityStat* stat = mod_ability(number, &id, &w);
    if (!stat) {
        if (why) *why = w;
        return KY_E_INVALID_ARG;
    }
    AbilityRanks& s = loaded_store();
    if (!s.selected()) {
        if (why) *why = "no character is selected yet: the rank was not set";
        return KY_E_REFUSED;
    }
    const int stored = s.set(id, rank, stat->max_rank);
    if (out) *out = s.get(id, stat->max_rank);
    if (stored < 0) {
        if (why) *why = "the ranks file could not be written (" + s.path() + "): the rank holds until the game exits";
        return KY_E_REFUSED;
    }
    return KY_OK;
}

std::vector<std::string> ability_rank_lines(const std::string& arg) {
    const std::string usage = "usage: ability rank <id> [rank]";
    size_t a = arg.find_first_not_of(' ');
    if (a == std::string::npos) return {usage};
    const size_t b = arg.find(' ', a);
    const std::string id = arg.substr(a, b == std::string::npos ? std::string::npos : b - a);
    std::string rest = b == std::string::npos ? "" : arg.substr(b);
    const size_t c = rest.find_first_not_of(' ');
    rest = c == std::string::npos ? "" : rest.substr(c, rest.find_last_not_of(' ') - c + 1);
    uint32_t number = 0;
    if (Registries::get().lookup("kyubu:abilities", id, &number) != KY_OK) return {"no ability \"" + id + "\" (ability list shows them)"};
    uint32_t rank = 0;
    std::string why;
    KyubuResult r;
    if (rest.empty()) {
        r = ability_rank_read(number, &rank, &why);
    } else {
        char* end = nullptr;
        const unsigned long v = strtoul(rest.c_str(), &end, 10);
        if (rest[0] == '-' || *end || end == rest.c_str() || v > 0xFFFF) return {"the rank must be a whole number from 0 up, got \"" + rest + "\""};
        r = ability_rank_write(number, (uint32_t)v, &rank, &why);
    }
    if (r != KY_OK) return {why};
    return {fmt("%s: rank %u of %u", id.c_str(), rank, ability_stat(number)->max_rank)};
}

void set_rank_path_for_test(const std::string& path) {
    std::lock_guard<std::mutex> g(g_mu);
    store().deselect();
    g_path_for_test = path;
    g_loaded = false;
}

}  // namespace kyubu::game
