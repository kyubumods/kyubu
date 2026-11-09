// The stat detours (design D5). Vanilla numbers (0..110) and numbers nobody defined call the original, so vanilla's values
// stay as they are. The detours run on every HUD frame and about 150 times a second for modeDuration: one range compare and
// an array look-up, no lock, no log.
#include "ability_stats.h"

#include <algorithm>

#include "abilities.h"
#include "ability_bar.h"
#include "ability_step.h"
#include "cubeworld/types.h"
#include "game.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

constexpr const char* kCooldown = "ability_cooldown";
constexpr const char* kCost = "ability_cost";
constexpr const char* kDuration = "cube::Creature::modeDuration";
constexpr const char* kController = "cube::game_controller";

// int ability_cooldown(Creature* this, int id, int level), `ret 8`, ms in EAX; float ability_cost(...) the same, result in
// st0; int modeDuration(Creature* this), plain `ret`. __fastcall with an unused EDX has the thiscall ABI.
using CooldownFn = int32_t(__fastcall*)(cube_Creature*, void*, int32_t, int32_t);
using CostFn = float(__fastcall*)(cube_Creature*, void*, int32_t, int32_t);
using DurationFn = int32_t(__fastcall*)(cube_Creature*, void*);

CooldownFn g_cooldown_next = nullptr;
CostFn g_cost_next = nullptr;
DurationFn g_duration_next = nullptr;
KyubuId g_hooks[3] = {0, 0, 0};
uintptr_t g_controller_global = 0;   // the address of the game controller pointer, found once at install

bool is_local(const cube_Creature* self) {
    const cube_GameController* gc = g_controller_global ? *(cube_GameController* const*)g_controller_global : nullptr;
    return gc && self == gc->player;
}

int32_t __fastcall cooldown_detour(cube_Creature* self, void* edx, int32_t id, int32_t level) {
    int32_t ms;
    if (L().host.host == KY_HOST_SERVER && level < 0 && (uint32_t)id >= kAbilityPlaceholderNumber)
        level = (int32_t)ability_rank_for(nullptr, self, (uint32_t)id);   // the creature's own rank
    if ((uint32_t)id >= kAbilityPlaceholderNumber && stat_cooldown((uint32_t)id, level, is_local(self), &ms)) return ms;
    return g_cooldown_next(self, edx, id, level);
}

float __fastcall cost_detour(cube_Creature* self, void* edx, int32_t id, int32_t level) {
    float mp;
    if ((uint32_t)id >= kAbilityPlaceholderNumber && stat_cost((uint32_t)id, &mp)) return mp;
    return g_cost_next(self, edx, id, level);
}

int32_t __fastcall duration_detour(cube_Creature* self, void* edx) {
    int32_t ms;
    const uint32_t mode = self->data.current_mode;
    if (mode >= kAbilityPlaceholderNumber && stat_duration(mode, &ms)) return ms;
    return g_duration_next(self, edx);
}

}  // namespace

bool stat_cooldown(uint32_t number, int32_t level, bool own_is_local, int32_t* ms) {
    const AbilityStat* s = ability_stat(number);
    if (!s) return false;
    int64_t rank = level;
    if (level < 0) rank = own_is_local ? ability_rank_of(number) : 1;
    rank = std::clamp<int64_t>(rank, 0, s->max_rank);
    *ms = (int32_t)s->cooldown_ms[(size_t)rank];
    return true;
}

bool stat_cost(uint32_t number, float* mp) {
    const AbilityStat* s = ability_stat(number);
    if (!s) return false;
    *mp = s->mp_cost;
    return true;
}

bool stat_duration(uint32_t number, int32_t* ms) {
    const AbilityStat* s = ability_stat(number);
    if (!s) return false;
    *ms = (int32_t)s->duration_ms;
    return true;
}

void install_ability_stat_hooks() {
    if (g_hooks[0] || g_hooks[1] || g_hooks[2]) return;
    std::string why;
    if (!ability_slot_available(&why)) return;   // game.cpp's check_entries has logged why
    struct Target {
        const char* name;
        void* detour;
        void** next;
    } targets[3] = {{kCooldown, (void*)&cooldown_detour, (void**)&g_cooldown_next},
                    {kCost, (void*)&cost_detour, (void**)&g_cost_next},
                    {kDuration, (void*)&duration_detour, (void**)&g_duration_next}};
    uintptr_t addr[3] = {};
    const bool server = L().host.host == KY_HOST_SERVER;   // Server.exe has no game controller: no creature is "local"
    const uintptr_t controller = server ? 0 : verified_symbol(kController, &why);
    bool ok = server || controller != 0;
    std::string err = why;
    for (int i = 0; ok && i < 3; ++i) {
        addr[i] = verified_symbol(targets[i].name, &err);
        if (!addr[i]) {
            err = std::string(targets[i].name) + ": " + err;
            ok = false;
        }
    }
    g_controller_global = controller;
    for (int i = 0; ok && i < 3; ++i) {
        if (Hooks::get().hook_function("kyubu", addr[i], targets[i].detour, kLoaderPriority, targets[i].next, &g_hooks[i], &err) !=
            KY_OK) {
            err = std::string(targets[i].name) + ": " + err;
            ok = false;
        }
    }
    if (!ok) {
        remove_ability_stat_hooks();
        KERROR("abilities: the fourth slot is off: cannot detour the stat functions: %s", err.c_str());
        return;
    }
    KINFO("abilities: the stat detours are installed (%s, %s, %s)", kCooldown, kCost, kDuration);
}

void remove_ability_stat_hooks() {
    for (KyubuId& h : g_hooks) {
        if (h) Hooks::get().remove("kyubu", h);
        h = 0;
    }
    g_cooldown_next = nullptr;
    g_cost_next = nullptr;
    g_duration_next = nullptr;
    g_controller_global = 0;
}

}  // namespace kyubu::game
