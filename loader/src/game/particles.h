// The particles primitive (openspec add-particle-effects, D1): the game's own particle effects, on the World step's output.
#pragma once

namespace kyubu::game {

// Pushes the effects mods asked for (particle_spawn) onto the output of the World step that has just run, the way the game's own
// sites do. `step_out` is that step's `step_out` argument. A no-op when nothing is queued, the primitive is unavailable or
// `step_out` is null. Called by the cube::World::update detour on Cube.exe, under the World's lock.
void flush_particles(void* step_out);

// Tests only: how many effects wait for the next step; and empties the queue.
unsigned queued_particles_for_test();
void clear_particles_for_test();

}  // namespace kyubu::game
