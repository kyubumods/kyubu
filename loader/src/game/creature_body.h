// The creature_body primitive: bodies of model pieces on a rig, drawn in place of a creature's own parts (openspec add-dragon-boss, D2;
// docs/game-api.md, "Creature bodies"). The pure part (definition, clips, pose) is creature_body_core.h.
#pragma once
#include <cstdint>

#include "kyubu_api.h"

namespace kyubu::game {

// Puts the mid hooks on the draw sites, the end of draw_creature and the death burst when the `creature_body` primitive is available.
void install_creature_body_hooks();
void remove_creature_body_hooks();
// The game destroys a creature / the world was left: its body ends.
void creature_body_drop(uint64_t creature_id);
void creature_body_world_exit();
// Whether a creature wears a body (tests).
bool creature_has_body(uint64_t creature_id);

// The hook bodies, public for the unit tests.
void body_on_draw_site(KyubuCpuContext* ctx);         // creature_draw_site: ESI the creature; hides its parts
void body_on_draw_site_b(KyubuCpuContext* ctx);       // creature_draw_site_b / _c: ECX the creature
void body_on_draw_end(KyubuCpuContext* ctx);          // draw_creature_end_site: EDI the draw record, EBP the frame; draws the pieces
void body_on_death_site(KyubuCpuContext* ctx);        // creature_death_burst_site: ESI the creature, EDI the GameController

}  // namespace kyubu::game
