// Creature handles and ownership (openspec add-game-api, design D2 and D3).
#pragma once
#include "common.h"

#include "cubeworld/types.h"
#include "game.h"

namespace kyubu::game {

// A KyubuCreature packs generation (bits 32-63, never 0), side (bits 24-31) and slot (bits 0-23). The loader keeps a
// slot table per side: slot -> (creature, generation), plus a pointer -> slot index, so the same creature on the same
// side keeps one handle while it lives. The destructor detour frees the slot and bumps its generation before the game
// destroys the creature, and leaving a world frees them all.

// The handle of `c` on `side`, made when there is none yet. 0 when c is null or the side is not a side.
KyubuCreature handle_for(KyubuSide side, cube_Creature* c);

// The creature a handle names, when it names a live one on `side`. Otherwise null with KY_E_INVALID_ARG (0 or not a
// handle), KY_E_SIDE (issued for the other side) or KY_E_GONE (the creature was destroyed, or the world left) in `r`
// and the reason in `why`.
cube_Creature* resolve_handle(KyubuSide side, KyubuCreature h, KyubuResult* r = nullptr, std::string* why = nullptr);

// The creature with a game id in `w`'s creature map, or null (creature_find).
cube_Creature* creature_by_id(cube_World* w, uint64_t id);

// Every creature of `w`'s creature map, as (game id, creature). Game thread, under the World's lock.
std::vector<std::pair<uint64_t, cube_Creature*>> world_creatures(cube_World* w);

// Which side owns a creature (D2): on a client its local player belongs to the client side and any other creature
// to the server side (a player of another client is one, too); Server.exe owns everything (it can't tell a player
// from a monster yet).
KyubuSide owner_of(const cube_World* w, const cube_Creature* c);

// The game is about to destroy `c`: its handles become invalid.
void on_creature_destroyed(const cube_Creature* c);
// The world was left: every handle becomes invalid.
void invalidate_handles();

// Installs the cube::Creature::~Creature detour when the handles primitive is available (boot), and removes it (tests).
void install_handle_hooks();
void remove_handle_hooks();

struct HandleStats {
    uint32_t live[3] = {0, 0, 0};   // by side (KY_SIDE_CLIENT, KY_SIDE_SERVER)
    uint64_t invalidated = 0;       // handles invalidated since the last report
};
HandleStats handle_stats(bool reset_invalidated = false);

// `game hold <id>` (dev mode): a client-side handle for the creature with that game id, kept in a list so that a
// later `game` shows whether it is still live. Returns a line for the console.
std::string hold_creature(uint64_t id, bool all = false);   // all: every creature of the World

// The handle-table lines of the `game` report.
std::vector<std::string> handle_report();

}  // namespace kyubu::game
