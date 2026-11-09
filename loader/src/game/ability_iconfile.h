// A mod's own ability icon (openspec add-ability-api, design D14). The game builds its icons once, in the
// GameController's constructor: for each PNG name it calls loadIcon(this, key, database), which reads the key from data3.db
// through Database::getBuffer (so through Kyubu's asset layer) and returns the icon object; the HUD's icon map holds the
// 4-byte pointers. A mod ability's `icon_file` is a data3.db key served by the mod's assets/icons/. Kyubu calls loadIcon
// once for it, with its own data3.db Database, and the result goes where a vanilla icon's pointer went (icon_put).
#pragma once
#include "common.h"

#include "cubeworld/types.h"

namespace kyubu::game {

// The icon object (the game's 4-byte pointer) of the mod ability `number`'s `icon_file`, loaded on the first call and kept.
// 0 when the ability has no icon_file, the primitive ability_iconfile is unavailable, nothing serves the key, or loadIcon
// returned null: the caller falls back to the vanilla `icon`. Each failure is logged once per ability. Game thread only: loadIcon
// makes a texture, and the game never frees it (the icon map's destructor frees nodes, not values), so the object
// is made once and shared by the bar and the skill tree.
uint32_t ability_file_icon(cube_GameController* gc, uint32_t number);

// Tests: replaces the two things the real call needs. `exists` says whether the asset layer serves the key; `load` makes the
// object (a plain pointer value). Null takes the real ones back. The cache and the once-only logs start again.
struct IconFileOps {
    bool (*exists)(const std::string& key);
    uint32_t (*load)(cube_GameController* gc, const std::string& key);
};
void set_icon_file_ops_for_test(const IconFileOps* ops);

}  // namespace kyubu::game
