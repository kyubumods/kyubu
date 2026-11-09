// The creature_flags primitive (openspec add-dragon-boss, D3): creature_set_flags and the hit-site hooks that obey the flags.
#pragma once
#include <cstdint>

#include "cubeworld/types.h"
#include "kyubu_api.h"

namespace kyubu::game {

// Puts the mid hooks on applyHit's five effect sites and on hitListSite / hitDamageSite when the `creature_flags` primitive is
// available; says why in the log when it is not. boot() calls it.
void install_creature_flag_hooks();
void remove_creature_flag_hooks();

// The flags of a creature (KyubuCreatureFlags bits); 0 for none. Any thread.
uint32_t creature_flags_of(uint64_t creature_id);
// Sets or clears (0) the flags of a creature by its game id.
void creature_flags_set(uint64_t creature_id, uint32_t flags);
// The game is destroying the creature, or the world was left: its flags (all flags) end.
void creature_flags_drop(uint64_t creature_id);
void creature_flags_world_exit();

// The hook bodies, public for the unit tests. Each reads the register or stack the site documents (mapping/cube-.../combat.toml).
void flags_on_push_site(KyubuCpuContext* ctx);           // hitPushSite: XMM5-7 := the target's extra_vel when unstoppable
void flags_on_velocity_site(KyubuCpuContext* ctx);       // hitVelocityResetSite: saves the velocity when unstoppable
void flags_on_velocity_end_site(KyubuCpuContext* ctx);   // hitVelocityResetEndSite: restores it
void flags_on_slow_site(KyubuCpuContext* ctx);           // hitSlowSite: ECX := the target's own timer
void flags_on_stun_site(KyubuCpuContext* ctx);           // hitStunSite: ECX := the target's stun, held at or below 0
void flags_on_list_site(KyubuCpuContext* ctx);           // hitListSite: the Hit at ESP+0xC4, damage 0 when invulnerable
void flags_on_damage_site(KyubuCpuContext* ctx);         // hitDamageSite: ESI the target, EBX the Hit
void root_on_ai_site(KyubuCpuContext* ctx);             // aiTickSite: [EBP-0x2B14] the creature; a rooted one does not think or act
void root_on_move_site(KyubuCpuContext* ctx);           // moveStepSite: [EBP-0x2B14] the creature, EBP-0x1C its step; a rooted one only falls

// The two KY_CREATURE_ROOTED hooks (primitive creature_root, both builds); install_creature_flag_hooks() calls the first.
void install_creature_root_hooks();
void remove_creature_root_hooks();

}  // namespace kyubu::game
