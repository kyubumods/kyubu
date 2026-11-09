// The game API for abilities: reading a creature's ability and its cooldowns, starting and ending a mod ability (openspec
// add-ability-api; docs/abilities.md).
#include <cstddef>
#include <cstring>

#include "abilities.h"
#include "ability_rank_bind.h"
#include "ability_step.h"
#include "call.h"
#include "cubeworld/functions.h"
#include "game.h"
#include "handles.h"
#include "int_map.h"
#include "loader.h"
#include "log.h"
#include "registry.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

// The smallest KyubuAbilityInfo a caller can pass: `size` and `number`.
constexpr uint32_t kInfoMinSize = offsetof(KyubuAbilityInfo, number) + sizeof(uint32_t);

// The milliseconds left in a creature's cooldown map for `number`: 0 for no entry, an entry of 0 or below, or a map that is
// not a tree. The game stores 0 when the cooldown is over and does not erase the node.
uint32_t cooldown_left(const cube_Creature* c, uint32_t number) {
    bool bad = false;
    const uint32_t* v = int_map_find(&c->ability_cooldowns, (int)number, &bad);
    if (!v) return 0;
    const int32_t ms = (int32_t)*v;
    return ms > 0 ? (uint32_t)ms : 0;
}

KyubuResult KYUBU_CALL k_ability_get(KyubuModHandle self, KyubuCreature c, KyubuAbilityInfo* out) {
    GameCall g(self, Prim::ability_state, "ability_get");
    if (!g) return g.result();
    if (!out || out->size < kInfoMinSize)
        return g.refuse(KY_E_INVALID_ARG, out ? fmt("out->size is %u, below %u", out->size, kInfoMinSize) : "out is NULL");
    const cube_Creature* cr = g.creature(c);
    if (!cr) return g.result();
    KyubuAbilityInfo info = {};
    info.size = sizeof info;
    const uint32_t mode = cr->data.current_mode;
    if (mode != 0) {
        info.number = mode;
        info.elapsed_ms = cr->data.mode_start_time > 0 ? (uint32_t)cr->data.mode_start_time : 0;
        std::string id;
        if (Registries::get().name_of("kyubu:abilities", mode, &id) == KY_OK && id.size() < sizeof info.id)
            memcpy(info.id, id.c_str(), id.size() + 1);
    }
    const uint32_t n = std::min<uint32_t>(out->size, sizeof info);
    memcpy((char*)out + sizeof(uint32_t), (const char*)&info + sizeof(uint32_t), n - sizeof(uint32_t));
    return KY_OK;
}

KyubuResult KYUBU_CALL k_ability_cooldown(KyubuModHandle self, KyubuCreature c, uint32_t number, uint32_t* out_ms) {
    if (out_ms) *out_ms = 0;
    GameCall g(self, Prim::ability_state, "ability_cooldown");
    if (!g) return g.result();
    if (!out_ms) return g.refuse(KY_E_INVALID_ARG, "out_ms is NULL");
    if (number > kAbilityNumberMax)
        return g.refuse(KY_E_INVALID_ARG, fmt("number is %u, above %u", number, kAbilityNumberMax));
    const cube_Creature* cr = g.creature(c);
    if (!cr) return g.result();
    *out_ms = cooldown_left(cr, number);
    return KY_OK;
}

KyubuResult KYUBU_CALL k_ability_cooldown_set(KyubuModHandle self, KyubuCreature c, uint32_t number, uint32_t ms) {
    GameCall g(self, Prim::ability_state, "ability_cooldown_set");
    if (!g) return g.result();
    if (number == 0 || number > kAbilityNumberMax)
        return g.refuse(KY_E_INVALID_ARG, fmt("number is %u, not 1 to %u", number, kAbilityNumberMax));
    if (ms > KY_ABILITY_COOLDOWN_MAX)
        return g.refuse(KY_E_INVALID_ARG, fmt("ms is %u, above %u", ms, KY_ABILITY_COOLDOWN_MAX));
    cube_Creature* cr = g.creature(c);
    if (!cr || !g.owns(cr)) return g.result();
    bool bad = false;
    if (uint32_t* v = int_map_find(&cr->ability_cooldowns, (int)number, &bad)) *v = ms;   // no entry: the game makes none either
    ability_server_cooldown_set(c, number, ms);   // Server.exe's admission (server_gate); nothing elsewhere
    return KY_OK;
}

// Whether ability_start and ability_end have something to start: the step runner and the stat detours of the fourth slot.
bool slot_or_refuse(GameCall& g) {
    std::string why;
    if (ability_slot_available(&why)) return true;
    g.refuse(KY_E_UNAVAILABLE, "the fourth ability slot is unavailable: " + why);
    return false;
}

KyubuResult KYUBU_CALL k_ability_start(KyubuModHandle self, KyubuCreature c, uint32_t number) {
    const bool server = L().host.host == KY_HOST_SERVER;   // Server.exe has no input path: no modeInterruptible or canUseAbility
    GameCall g(self, server ? Prim::ability_state : Prim::ability_use, "ability_start");
    if (!g) return g.result();
    const AbilityStat* stat = ability_stat(number);
    if (!stat)
        return g.refuse(KY_E_INVALID_ARG, fmt("%u is not a defined mod ability (a vanilla ability starts from the game's own input)", number));
    if (!g.need(Prim::ability_state) || !slot_or_refuse(g)) return g.result();
    cube_Creature* cr = g.creature(c);
    if (!cr || !g.owns(cr)) return g.result();
    std::string name;
    Registries::get().name_of("kyubu:abilities", number, &name);
    // The checks of a press of key 4, in the order that names the reason; the game's own functions come last, so that what
    // they know and Kyubu does not (a timer in the creature) refuses too.
    if (!ability_runs_here(g.world(), cr))
        return g.refuse(KY_E_REFUSED, "the step runner serves only the local player, and monsters in single-player: " + name + " would never run on this creature");
    if (const uint32_t left = cooldown_left(cr, number))
        return g.refuse(KY_E_REFUSED, fmt("%s is on cooldown: %u ms left", name.c_str(), left));
    if (stat->mp_cost > cr->data.mp)
        return g.refuse(KY_E_REFUSED, fmt("not enough mp for %s: it costs %.3f and the creature has %.3f", name.c_str(), stat->mp_cost, cr->data.mp));
    if (server) {   // the server's own busy test: a creature in a mode finishes it first (the end pass or ability_end frees it)
        if (cr->data.current_mode != 0)
            return g.refuse(KY_E_REFUSED, fmt("the creature is busy: it is in mode %u", (unsigned)cr->data.current_mode));
        cr->data.current_mode = (uint8_t)number;
        cr->data.mode_start_time = 0;
        return KY_OK;
    }
    const auto interruptible = (cw_cube_Creature_modeInterruptible_fn)verified_symbol(CW_SYM_cube_Creature_modeInterruptible);
    const auto can_use = (cw_cube_Creature_canUseAbility_fn)verified_symbol(CW_SYM_cube_Creature_canUseAbility);
    if (!interruptible || !can_use) return g.refuse(KY_E_UNAVAILABLE, "the game's modeInterruptible or canUseAbility is not mapped for this build");
    if (!interruptible(cr, nullptr))
        return g.refuse(KY_E_REFUSED, fmt("the creature is busy: its mode %u cannot be interrupted yet", (unsigned)cr->data.current_mode));
    if (!can_use(cr, nullptr, (int32_t)number))
        return g.refuse(KY_E_REFUSED, "the game's canUseAbility refuses " + name + " (a timer on the creature, such as a stun)");
    // What the game's input does (cube::GameController::abilityStartSite): the id becomes the mode and the mode time is cleared.
    // The mp is the step runner's, at the first step, and the cooldown the game's own (cooldownStartSite, through the detour).
    cr->data.current_mode = (uint8_t)number;
    cr->data.mode_start_time = 0;
    return KY_OK;
}

KyubuResult KYUBU_CALL k_ability_end(KyubuModHandle self, KyubuCreature c) {
    GameCall g(self, Prim::ability_state, "ability_end");
    if (!g) return g.result();
    if (!slot_or_refuse(g)) return g.result();
    cube_Creature* cr = g.creature(c);
    if (!cr || !g.owns(cr)) return g.result();
    // The step runner keys its records by the owner's handle, which is the caller's own after the ownership check.
    if (ability_mark_cancel(c)) return KY_OK;
    // A start the step runner has not seen yet (no World step ran since): nothing ran, so nothing has to end; take the mode back.
    const uint32_t mode = cr->data.current_mode;
    if (mode >= kAbilityPlaceholderNumber && ability_stat(mode)) {
        cr->data.current_mode = 0;
        cr->data.mode_start_time = 0;
        return KY_OK;
    }
    return g.refuse(KY_E_NOT_FOUND, "the creature is in no mod ability");
}

KyubuResult KYUBU_CALL k_ability_rank_get(KyubuModHandle self, uint32_t number, uint32_t* out_rank) {
    if (out_rank) *out_rank = 0;
    GameCall g(self, Prim::ability_ranks, "ability_rank_get");
    if (!g) return g.result();
    if (!out_rank) return g.refuse(KY_E_INVALID_ARG, "out_rank is NULL");
    std::string why;
    const KyubuResult r = ability_rank_read(number, out_rank, &why);
    return r == KY_OK ? r : g.refuse(r, why);
}

KyubuResult KYUBU_CALL k_ability_rank_set(KyubuModHandle self, uint32_t number, uint32_t rank, uint32_t* out_rank) {
    if (out_rank) *out_rank = 0;
    GameCall g(self, Prim::ability_ranks, "ability_rank_set");
    if (!g) return g.result();
    uint32_t stored = 0;
    std::string why;
    const KyubuResult r = ability_rank_write(number, rank, &stored, &why);
    if (out_rank) *out_rank = stored;
    return r == KY_OK ? r : g.refuse(r, why);
}

}  // namespace

void fill_ability_api(KyubuApi& a) {
    a.ability_get = k_ability_get;
    a.ability_cooldown = k_ability_cooldown;
    a.ability_cooldown_set = k_ability_cooldown_set;
    a.ability_start = k_ability_start;
    a.ability_end = k_ability_end;
    a.ability_rank_get = k_ability_rank_get;
    a.ability_rank_set = k_ability_rank_set;
}

}  // namespace kyubu::game
