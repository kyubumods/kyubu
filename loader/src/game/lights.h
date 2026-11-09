// The lights primitive (openspec add-light-api): point lights of mods in the frame's light vector.
#pragma once
#include <cstdint>
#include <string>

namespace kyubu::game {

// Puts the mid hook at light_sort_site when the `lights` primitive is available; says why in the log when it is not. boot() calls it.
void install_light_hook();
void remove_light_hook();

// Pushes the live lights into the frame's light vector (`vector` is the std::vector the render fills, `now_ms` the clock) with the
// game's push_back, after dropping the ones whose life is over or whose mod is no longer loaded. The mid hook calls it; tests call it
// with a vector of their own. A no-op while no light is alive or the push is not a verified symbol.
void push_lights(void* vector, uint64_t now_ms);

// Ends every light (the world was left), or the lights of one mod (its Lua side is reloaded).
void lights_world_exit();
void lights_drop_mod(const std::string& mod_id);

// Tests only: how many lights are alive; the clock light_spawn and light_set read (0: the real one).
unsigned light_count_for_test();
void set_light_clock_for_test(uint64_t now_ms);

}  // namespace kyubu::game
