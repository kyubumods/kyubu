// The player_shots primitive (openspec add-skills-unleashed-multiplayer 10.9, design D16): the projectile packets joined clients
// send, copied on Server.exe's receive thread for a mod to take, and projectile_push into the World's own list.
#pragma once
#include "common.h"

namespace kyubu::game {

// The mid hook on Net::receiveProjectileSite (the connection's thread): copies the packet and its connection id, once a mod wants them.
void shots_on_receive_site(KyubuCpuContext* ctx);

void install_shot_hooks();   // boot, Server.exe, when player_shots is available; says why in the log when not
void remove_shot_hooks();    // tests

void shots_enqueue_for_test(uint32_t connection, const void* data);   // as the hook does, without the wanted gate
void reset_shots_for_test();

void fill_shots(KyubuApi& a);   // shot_take, projectile_push

}  // namespace kyubu::game
