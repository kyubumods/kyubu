// GameCall: the guard every game-API function starts with (openspec add-game-api, design D5).
#pragma once
#include "common.h"

#include "cubeworld/types.h"
#include "game.h"
#include "world.h"

namespace kyubu {
struct Mod;
}

namespace kyubu::game {

// Bundles the checks, so that a game-API function is only its own logic:
//
//   GameCall g(self, Prim::example, "creature_example");
//   if (!g) return g.result();
//   cube_Creature* cr = g.creature(c);         // KY_E_GONE, KY_E_SIDE
//   if (!cr) return g.result();
//   if (!g.owns(cr)) return g.result();        // only for changes: KY_E_SIDE
//   cr->data.hp = v;
//
// It checks the mod (a faulted mod gets KY_E_FAULTED), then the function's side rule (its row in kSideRules; a
// one-side function called from the other side gets KY_E_SIDE), then the primitive, the World of the mod's side, the
// game thread and the World's lock (WorldAccess), and holds the lock until it goes out of scope. Each refusal is
// logged once per mod, function and reason. A refused call touches no game memory.
class GameCall {
public:
    GameCall(KyubuModHandle self, Prim prim, const char* fn);
    GameCall(const GameCall&) = delete;
    GameCall& operator=(const GameCall&) = delete;

    explicit operator bool() const { return r_ == KY_OK; }
    KyubuResult result() const { return r_; }
    const std::string& why() const { return why_; }
    KyubuSide side() const { return side_; }
    cube_World* world() const { return access_ ? access_->world() : nullptr; }

    // The creature a handle names on this call's side. Null, with the result set, when there is none. Handles need the
    // `handles` primitive (its destructor detour is what makes a gone creature's handle fail): without it,
    // KY_E_UNAVAILABLE.
    cube_Creature* creature(KyubuCreature h);
    // A handle for a creature, to hand to the mod. 0, with the result set, when there is none (as for creature()).
    KyubuCreature handle(cube_Creature* c);
    // Whether the call's side owns the creature; when it does not, the result is KY_E_SIDE and the log says who does.
    bool owns(const cube_Creature* c);
    // The function's own refusal (an argument, a state the guard doesn't check): sets the result and logs it once,
    // like the guard's. Returns r.
    KyubuResult refuse(KyubuResult r, const std::string& why);
    // Another primitive the function uses besides its own (the guard checks one): false, with KY_E_UNAVAILABLE and
    // the reason, when it is unavailable. creature() and handle() need `handles` this way; a function that builds
    // something before it hands out a handle checks first.
    bool need(Prim p);

private:
    bool fail(KyubuResult r, const std::string& why);

    const char* fn_;
    Mod* mod_ = nullptr;
    KyubuSide side_ = KY_SIDE_NONE;
    KyubuResult r_ = KY_OK;
    std::string why_;
    std::unique_ptr<WorldAccess> access_;
};

// The reason of the latest GameCall refusal on this thread; empty when the latest call passed or was refused without
// a reason (not a mod handle, a faulted mod). The Lua bindings return it, since the C functions return only a code.
const std::string& last_refusal();

// A refusal by a function that has no GameCall (the animation editor's calls take no World): logs it once per mod,
// function and reason, as GameCall does, keeps the reason for last_refusal() and returns `r`. clear_refusal() empties the
// reason at the start of such a call.
KyubuResult note_refusal(Mod* m, KyubuSide side, const char* fn, KyubuResult r, const std::string& why);
void clear_refusal();

}  // namespace kyubu::game
