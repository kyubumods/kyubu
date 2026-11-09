#include "kyubu.hpp"
static void(__cdecl* g_next)() = nullptr;
static int g_frames = 0;
static void __cdecl frame_hook() {
    if (++g_frames == 60) *(volatile int*)0 = 1;   // raw hooks are not guarded: this crashes the game
    g_next();
}
struct Crash {
    explicit Crash(kyubu::Mod& m) { m.hook("Game::frame", &frame_hook, &g_next); }
};
KYUBU_MOD("fx_crash", KY_SIDE_CLIENT, Crash)
