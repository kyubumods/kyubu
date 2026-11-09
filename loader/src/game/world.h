// The World of each side, the game thread, the World's lock and the safe points around each World step
// (openspec add-game-api, design D2 and D4).
#pragma once
#include "common.h"

#include "cubeworld/types.h"
#include "game.h"

namespace kyubu::game {

// The World a side uses now (D2): single-player Cube.exe gives both sides the game controller's World, a joined
// client only its client side, Server.exe only its server side, the World cube::World::update steps. Null, with
// KY_E_PHASE (no world is loaded, or it has not stepped yet) or KY_E_SIDE (this host has no such side) in `r` and
// the reason in `why`, when there is none.
cube_World* side_world(KyubuSide side, KyubuResult* r = nullptr, std::string* why = nullptr);

// The thread that steps the World (0 until the first step), and whether the caller is on it.
DWORD game_thread();
bool on_game_thread();
// Whether a World step (the original cube::World::update) is running now.
bool in_step();

// Safe points (D4): fn runs on the game thread before the next World step, or after it returns. Anything that
// changes the creature map uses after_step, because the step iterates the map and mod code can run inside it.
void before_step(std::function<void()> fn);
void after_step(std::function<void()> fn);

// The world session, from the loader's lifecycle.world_enter and world_exit (game_hooks.cpp).
void on_world_enter();
void on_world_exit();

// Installs the cube::World::update detour when the world primitive is available (boot), and removes it (tests).
void install_world_hooks();
void remove_world_hooks();

// The checks on the World every game-API call makes (GameCall builds on it): the primitive is available, the side
// has a World, the caller is on the game thread; then it holds the World's lock until it goes out of scope. A
// refused call touches no game memory.
class WorldAccess {
public:
    WorldAccess(Prim p, KyubuSide side);
    ~WorldAccess();
    WorldAccess(const WorldAccess&) = delete;
    WorldAccess& operator=(const WorldAccess&) = delete;

    explicit operator bool() const { return r_ == KY_OK; }
    KyubuResult result() const { return r_; }
    const std::string& why() const { return why_; }
    cube_World* world() const { return world_; }

private:
    KyubuResult r_ = KY_OK;
    std::string why_;
    cube_World* world_ = nullptr;
};

// The `game` console command's report (design D7): each side's World, the game thread and the lock, and each
// primitive's availability. Server.exe logs it when it captures its World.
std::vector<std::string> report();

}  // namespace kyubu::game
