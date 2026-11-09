// Binds the ranks file (registry/ability_ranks.h, design D9) to the played character and to the rank API (openspec
// add-ability-api). The character is known from two game functions: load_character (the list's SELECT loads a slot
// into the player) and save_character (the autosave, every 60 s, confirms the slot). Both are mapped on Cube.exe only, so the
// ranks are the local player's and a client's.
#pragma once
#include "common.h"

#include "cubeworld/types.h"

namespace kyubu::game {

// Detours load_character and save_character when ability_ranks is available; logs once why not (game.cpp's check_entries has
// said why). The file's path is <game_dir>\Save\characters.kyubu-abilities.json; it is read at the first use.
void install_ability_rank_hooks();
void remove_ability_rank_hooks();

// What both detours do after the original returns: the character in `slot` is played, and `player` holds its name. A creature
// that is not the player (the character list's own copies) is ignored.
void ability_rank_character(int32_t slot, const cube_Creature* creature);

// The World was left: the file is written when something is unsaved. The character stays selected: the player returns to the
// list and may enter the world again without a new load.
void ability_rank_world_exit();

// The stored rank of the local player in the mod ability `number`, clamped to the ability's maximum: 1 for a number with no
// definition, no character selected, or ability_ranks unavailable. ability_rank_of (ability_bar.cpp) calls it on every bar
// build and step.
uint32_t ability_rank_stored(uint32_t number);

// The rank functions without the game-API guard (the console calls them, the API after its guard). `why` gets the reason of a
// refusal. KY_E_UNAVAILABLE: ability_ranks unavailable (names the blocking entry), KY_E_INVALID_ARG: not a defined mod ability,
// KY_E_REFUSED: no character selected, or the file could not be written (`out` still holds the rank, which lasts until the
// game exits).
KyubuResult ability_rank_read(uint32_t number, uint32_t* out, std::string* why);
KyubuResult ability_rank_write(uint32_t number, uint32_t rank, uint32_t* out, std::string* why);

// `ability rank <id> [rank]`: the console's lines.
std::vector<std::string> ability_rank_lines(const std::string& arg);

// Tests: the file is read from `path` (and nothing is selected); an empty path takes the default back.
void set_rank_path_for_test(const std::string& path);

}  // namespace kyubu::game
