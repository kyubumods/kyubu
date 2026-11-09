// The game API's base layer (openspec add-game-api): the entry list checked against the running build,
// availability per primitive, and the public queries game_available and game_enum. What the API uses is listed in
// entries.inc.
#pragma once
#include "common.h"

namespace kyubu::game {

enum class Prim : int {
#define GAME_PRIMITIVE(id, section) id,
#include "entries.inc"
    count
};

const char* prim_name(Prim p);

// Checks every entry of every primitive against the running build (L().host) and logs each unavailable primitive
// once, with the entry that blocks it. boot() runs it at startup.
void check_entries();
void boot();

// Why a `type` entry (a struct or an enum of cubeworld/layout.h) can't be used on this loader; empty when it can.
// A candidate struct or enum counts as not verified.
std::string type_problem(const std::string& name);

// Why a `field` entry ("cube::Struct.field" of cubeworld/layout.h) can't be used; empty when it can. Only the field's own
// status counts: a verified field of a candidate struct is usable.
std::string field_problem(const std::string& name);

// The name of `value` in a verified enum of the loader's layouts; null when the enum is unknown or candidate, or has
// no such value. These are the values game_enum lists, so creature_spawn accepts exactly those.
const char* enum_value_name(const char* enum_name, int64_t value);

// Tests only: limits a primitive to the builds in `host_mask` (bits of 1 << KyubuHost), as a GAME_BUILDS line does;
// 0 takes the limit back. The next check_entries() uses it.
void limit_builds_for_test(Prim p, unsigned host_mask);

// Tests only: makes `available(p)` answer `state` whatever check_entries() found: 1 available, 0 unavailable (with a reason), -1 the
// check's own answer again. For a primitive whose entries a unit test cannot stand up (the pose provider's detour and site).
void force_primitive_for_test(Prim p, int state);

// Whether a primitive is available on this build, and why not.
bool available(Prim p, std::string* why = nullptr);

// The fourth ability slot (openspec add-ability-api, D11; the spec's "All or nothing"): true when ability_state,
// ability_step, ability_bar, ability_stats and ability_icons are all available. Otherwise `why` names the first
// blocking primitive and its entry; on Server.exe it says "not applicable" (that build has no skill bar, which is not
// an error). ability_stun and ability_ranks are not part of the set: check them with available().
bool ability_slot_available(std::string* why = nullptr);

// Sets the game-API members of the KyubuApi table (api.cpp's make_table).
void fill_table(KyubuApi& a);
// The members of one primitive (fill_table calls them): entity_state.cpp, spawn.cpp, items.cpp, ability_api.cpp.
void fill_entity_state(KyubuApi& a);
void fill_spawn(KyubuApi& a);
void fill_items(KyubuApi& a);
void fill_particles(KyubuApi& a);      // particles.cpp: particle_spawn
void fill_creature_flags(KyubuApi& a);  // creature_flags.cpp: creature_set_flags
void fill_creature_body(KyubuApi& a);   // creature_body.cpp: body_define ... creature_body_point
void fill_terrain(KyubuApi& a);       // terrain.cpp: world_block
void fill_world_step(KyubuApi& a);    // world_step.cpp: creature_hit
void fill_lights(KyubuApi& a);         // lights.cpp: light_spawn, light_set, light_remove
void fill_ui(KyubuApi& a);             // ui.cpp: ui_node_find, ui_node_get, ui_node_child, render_get
void fill_animation_api(KyubuApi& a);   // animation_api.cpp: animation_define_from_text, animation_preview, animation_preview_clear
void fill_ability_api(KyubuApi& a);   // ability_api.cpp: ability_get, ability_cooldown, ability_cooldown_set, ability_start, ability_end, ability_rank_get, ability_rank_set

}  // namespace kyubu::game
