// The fourth ability slot on the skill bar (openspec add-ability-api, design D4): the push at
// cube::GameController::abilityBarBuiltSite.
#pragma once
#include "common.h"

#include "cubeworld/types.h"

namespace kyubu::game {

// What bar_fill did.
enum class BarFill {
    Pushed,        // the owner's number is on the bar now
    NotVanillaBar, // the bar does not hold vanilla's five entries: left unchanged
    NoHolder,      // nobody holds slot 4 for the pair (or the pair is not a class and specialization)
    NotLearned,    // the character's rank in the holder's ability is 0
};

constexpr size_t kVanillaBarSize = 5;   // M1, M2 and the three class abilities, after the learned gate

// Appends the slot-4 holder's number for (class_type, specialization) to `bar`, with `push` (the game's own
// IntVector::push_back, so that the vector's allocation stays the game's). Only a bar of exactly kVanillaBarSize entries
// is touched; the rank (abilities.h: ability_slot_holder, then the character's rank in it) must be at least 1. A bar of
// another size is logged once.
using BarPushFn = void (*)(std_vector* bar, const int* value);
BarFill bar_fill(std_vector* bar, int class_type, int specialization, BarPushFn push);

// What icon_fill did (design D7).
enum class IconFill {
    Written,       // the node of `number` held null and now holds the value of the icon ability's node
    AlreadySet,    // the node of `number` holds a value: left unchanged
    NoNode,        // the map has no node for `number` yet: the HUD's own look-up makes it on this frame, so the next frame fills it
    NoSource,      // the map has no node, or only a null value, for `icon_number`: the slot stays empty
    NotATree,      // the map's tree is not one (a walk went over its bound): left unchanged
};

// Gives the HUD's icon map (cube::GameController.ability_icons: std::map<int, pointer>, key at node+0x10, the 4-byte value at
// node+0x14) an icon for the ability `number`: the value of the vanilla ability `icon_number`. It never inserts a node: it
// writes only into one that exists, with a null value. The game's destructor frees the nodes and not the values,
// so the copied pointer needs no write-back at exit.
IconFill icon_fill(const std_map* icons, uint32_t number, uint32_t icon_number);

// Writes `value` (an icon object) into the node of `key` in the icon map `map`, with icon_fill's rules (never inserts; writes only a
// null value). NoSource when `value` is 0. The mod's own icon (ability_iconfile.h) goes in this way.
IconFill icon_put(const std_map* map, uint32_t key, uint32_t value);

// The same copy between two maps: the value of `src_key` in `src_map` goes into the node of `dst_key` in `dst_map`, with
// icon_fill's rules (never inserts; writes only a null value). The skill tree copies ability_icons[icon] into skill_icons[9].
IconFill icon_copy(const std_map* dst_map, uint32_t dst_key, const std_map* src_map, uint32_t src_key);

// Gives the node of `dst_key` in `dst` the icon of the mod ability `number`: its own icon_file object when it has one (loaded
// once, ability_iconfile.h), else the vanilla ability's `icon`, read from gc->ability_icons. The bar passes (ability_icons,
// number), the skill tree (skill_icons, 9). NoSource when neither gives a pointer.
IconFill ability_icon_fill(cube_GameController* gc, const std_map* dst, uint32_t dst_key, uint32_t number);

// The icon object the mod ability `number` shows: its own icon_file object, else the value of its vanilla `icon` in
// gc->ability_icons; 0 when neither gives one.
uint32_t ability_icon_value(cube_GameController* gc, uint32_t number);

// Puts `value` into the node of `key` in `map` when the node holds null or `prev` (the value Kyubu wrote there before), so a
// key that shows different abilities in turn (skill button 9 after a change of specialization) follows them. A node holding
// anything else is the game's: AlreadySet, untouched, as is a node that holds `value` already. Never inserts.
IconFill icon_swap(const std_map* map, uint32_t key, uint32_t value, uint32_t prev);

// The character's rank in an ability: for the player's learnable slot-4 holder it is skills[9] (design D13,
// ability_skilltree.h); for any other number the ranks file (design D9: ability_rank_stored, ability_rank_bind.h). While
// ability_ranks is not mapped, the file's ranks all read 1.
uint32_t ability_rank_of(uint32_t number);

// Installs the mid hook at abilityBarBuiltSite when ability_slot_available() says so; logs once why not.
void install_ability_bar_hook();
void remove_ability_bar_hook();

// Tests: `rank` replaces ability_rank_of (null takes it back), and the once-only log of a foreign bar starts again.
void set_rank_source_for_test(uint32_t (*rank)(uint32_t number));
void reset_bar_log_for_test();

}  // namespace kyubu::game
