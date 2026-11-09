// The server_hits primitive (openspec add-skills-unleashed-multiplayer; primitives.md, Server hits): Server.exe sends each of
// hitCreature's hits on a player to the clients twice, and the loader drops the second copy.
#pragma once
#include "common.h"

#include "cubeworld/types.h"

namespace kyubu::game {

// The mid hook on cube::World::hitListSite: notes the address of the Hit that hitCreature is about to push and then apply.
void server_hits_on_list_site(KyubuCpuContext* ctx);

// The cube::World::applyHit detour (thiscall: the hit, the step output, the changed-zone set; `ret 0xC`).
void __fastcall server_hits_apply_detour(cube_World* self, void* edx, const cube_Hit* hit, void* out, void* zones);

void install_server_hit_hooks();   // boot, Server.exe, when server_hits is available; says why in the log when not
void remove_server_hit_hooks();    // tests

// Tests: the original applyHit the detour calls, and the number of copies dropped so far.
using ApplyHitFn = void(__fastcall*)(cube_World*, void*, const cube_Hit*, void*, void*);
void set_server_hits_next_for_test(ApplyHitFn next);
uint64_t server_hits_dropped();

}  // namespace kyubu::game
