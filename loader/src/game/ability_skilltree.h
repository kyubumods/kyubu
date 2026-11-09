// Learning the fourth ability in the skill tree (openspec add-ability-api, design D13). The game's skill panel has
// 11 buttons, one per `Creature.skills` entry; button 9 (ABILITY4) is built, laid out and handled by the clicks, and hidden
// twice. Kyubu shows it while a mod holds the player's slot 4, gives it the holder's icon, and reads the holder's
// rank from `skills[9]`.
#pragma once
#include "common.h"

#include "cubeworld/types.h"

namespace kyubu::game {

constexpr uint32_t kSkillAbility4 = 9;        // index of ABILITY4 in Creature.skills, skill_buttons and skill_icons
constexpr uint32_t kSkillButtonCount = 11;

// The slot-4 holder of (class_type, specialization) when the tree can teach it: a mod ability (a defined number from 112 up).
// The placeholder is not learnable (it stays at rank 1 for everybody), and nobody-holds-it has no button. False otherwise.
bool skilltree_learnable(int class_type, int specialization, uint32_t* number);

// The specialization whose abilities the skill panel shows: while it is open, the SkillWidget's working copy (+0x18C: a click on
// a specialization button sets it, Learn copies it to the player, Cancel puts the player's back); otherwise, or when that
// value is not 0 or 1, the player's own. Button 9's owner, icon and rank cap follow it, as the game's buttons 6 to 8 do.
int skilltree_shown_specialization(const cube_GameController* gc);

// The visible flag of skill button `index` (the vector's element, its node at +0x3C, then node[+0x94][node[+0x68]]); null
// when the vector is short or a pointer on the way is null.
int32_t* skill_button_flag(const cube_GameController* gc, uint32_t index);

// What one pass of the hook did.
enum class TreeStep {
    PanelClosed,   // skills_open is 0: nothing to show
    NoPlayer,
    NotLearnable,  // no mod holds the player's slot 4: button 9 stays hidden, as in vanilla
    NoButton,      // the button vector is short or its node is not there
    Shown,         // button 9's flag is 1 (the icon may follow on a later frame: the panel makes its node on first look-up)
};
// One pass of the mid hook at skillButton9HideSite, on the game controller `gc`. Writes only button 9's flag, the value of an
// existing node of skill_icons[9] when it is null or the icon Kyubu put there before (icon_swap), and the panel's working rank of
// button 9 when it is over the ability's max_rank.
TreeStep skilltree_step(cube_GameController* gc);

// Design D13's rank rule. When the tree is active (its hook installed) and `number` is the local player's learnable holder,
// the rank is skills[9] clamped to the ability's maximum rank and this returns true; otherwise false and `rank` is untouched.
bool skilltree_rank_get(uint32_t number, uint32_t* rank);
// The same test for a write: clamps `rank` to 0..max_rank, stores it in skills[9] of the local player and returns true.
bool skilltree_rank_set(uint32_t number, uint32_t rank, uint32_t* stored);

// True once the hook is installed (the primitive ability_skilltree was available).
bool skilltree_active();

// Installs the mid hook at skillButton9HideSite when ability_slot_available() and ability_skilltree are available; logs once
// why not. The fourth slot works without it, and the ranks then come from the ranks file.
void install_ability_skilltree_hook();
void remove_ability_skilltree_hook();

// Tests: makes the tree active for `player` (null takes it back) without a hook, so that the rank rule is tested alone. Also
// forgets which icon Kyubu last wrote into skill_icons[9].
void set_skilltree_player_for_test(cube_Creature* player);

}  // namespace kyubu::game
