// The stat detours of the fourth ability slot (openspec add-ability-api, design D5): the game's own questions about an
// ability's cooldown, mp cost and duration are answered from the definition for mod numbers (111 up), and by the game for
// every other number.
#pragma once
#include "common.h"

namespace kyubu::game {

// The pure answers, so that the lookup is tested without hooks. Each returns false for a number that is not a defined mod
// ability (vanilla 0..110, an undefined number, or a table that is not built yet): the detour then calls the original with
// the arguments it was given.
//
// `level` is the second argument the game passes. -1 means the creature's own rank: ability_rank_of(number) for the local
// player (`own_is_local`), 1 for any other creature. Any level is clamped to 0..max_rank (the tooltip's "next level" passes
// max_rank + 1 on a full skill).
bool stat_cooldown(uint32_t number, int32_t level, bool own_is_local, int32_t* ms);
bool stat_cost(uint32_t number, float* mp);   // a fraction of the creature's mp, as ability_cost returns it
bool stat_duration(uint32_t number, int32_t* ms);

// Installs the three detours when ability_slot_available() says so. All three or none: a failure removes the ones already
// installed and logs one ERROR line.
void install_ability_stat_hooks();
void remove_ability_stat_hooks();

}  // namespace kyubu::game
