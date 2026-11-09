// The step runner and the ends of a mod ability (openspec add-ability-api, design D6): a mid hook at
// cube::World::abilityDispatchSite runs the ability of every creature whose mode is a registered number, and a pass after
// each World step ends the activations.
#pragma once
#include "common.h"

#include "cubeworld/types.h"

namespace kyubu::game {

// Why an activation ended; the values are KY_ABILITY_END_*.
enum class EndReason : int32_t { None = -1, Done = 0, Cancelled = 1, Stunned = 2, Died = 3, Interrupted = 4, Gone = 5 };
const char* end_reason_name(EndReason r);   // "done", "cancelled", "stunned", "died", "interrupted", "gone"; "none" for None

// What the after-step pass knows about an activation (design D6, "Ending").
struct EndFacts {
    bool gone = false;           // the creature was destroyed, or the world was left
    uint32_t mode = 0;           // the creature's current_mode now
    uint32_t number = 0;         // the activation's ability number
    float hp = 1;
    int32_t stun_time = 0;
    bool stun_known = false;     // ability_stun is available: only then a stun is `stunned`, else `interrupted`
    uint32_t elapsed_ms = 0;     // the mode time now when mode == number, else the last one the hook saw
    uint32_t duration_ms = 0;
    bool cancel_marked = false;  // a callback failed, or ctx:cancel() / ability_end was called
    uint32_t report_slack_ms = 0;  // the mode is another game's report: `done` also when the last time is this short of the duration
};
struct EndDecision {
    EndReason reason = EndReason::None;   // None: the activation goes on
    bool write_mode_zero = false;         // the mode is still the number: write mode 0 and mode time 0 before on_end
};
// The order: gone; died (hp <= 0); cancelled; stunned (or interrupted); then the mode: still the number, ends `done` at
// duration_ms (writing mode 0) and otherwise goes on; another mode ends `done` when it is 0 and the last time reached the
// duration (or is within report_slack_ms of it), else `interrupted`.
EndDecision decide_end(const EndFacts& f);

// The mp after the cost is taken: never below 0.
float mp_after_cost(float mp, float cost);

// What one creature at the dispatch site is: the first step of an activation, another step of it, or a new activation
// replacing the one that was running (another number, or the mode time went back). `Hold` is a step of the running activation
// whose mode time went back by at most `step_back_ms` (0: never): a watched creature's time is corrected by each server update
// (openspec add-skills-unleashed-multiplayer B2), so the step goes on at the time it had, with dt 0.
enum class StepKind { Start, Continue, Replace, Hold };
StepKind classify_step(bool has_record, uint32_t record_number, uint32_t record_last_elapsed, uint32_t mode, uint32_t elapsed_ms,
                       uint32_t step_back_ms = 0);

// How far back a watched creature's mode time may step and still be the same activation: more than one server update
// interval (20 to 50 ms), less than any restart of a mode (a Fire Mage release sets the time back to 0 after a charge).
constexpr uint32_t kWatchStepBackMs = 100;

// A restart (KyubuAbilityDef.restarts): the mode's owner wrote its own time back to about 0 (a Fire Mage release). The report
// lands within one or two updates, so the time seen is at most this.
constexpr uint32_t kRestartWindowMs = 250;

// Whether a `Replace` of a reported run (a watched creature, a remote player on Server.exe) is that run's restart instead: the
// same number, the mode time back by more than kWatchStepBackMs (a smaller step is a Hold) to kRestartWindowMs or less, and
// fewer than `allowed` restarts so far. The creature's own run never restarts: its mod sees an end and a start.
bool is_restart(bool reported, uint32_t record_number, uint32_t record_last_elapsed, uint32_t mode, uint32_t elapsed_ms,
                uint32_t restarts_so_far, uint32_t allowed);

// How far short of the duration the last mode time seen may be when the mode comes from another game (a watched creature, or a
// remote player on Server.exe) and still end `done`: the owner's game ends the mode at the duration, and the report of mode 0
// lands after a time that trailed its own. The server's run 2 of watch-berserker-0720 saw 1591 of 1700 ms (109 short).
constexpr uint32_t kReportEndSlackMs = 250;

// A watched activation's end (B3, B4): the creature's mode is the server's, so only the mode leaving the number ends it, and
// nothing is written to the creature. Gone first; then, once the mode has left: cancelled, died, stunned (or interrupted), done
// (mode 0 and the last time within report_slack_ms of the duration: the bystander's time trails the server's), else interrupted.
// write_mode_zero is always false.
EndDecision decide_watch_end(const EndFacts& f);

// The game's own guard in front of every mode block: a client that has joined a server runs only its own player's
// modes, a server or single-player World also runs every monster's. The step runner (and so ability_start) serves only these.
bool ability_runs_here(const cube_World* w, const cube_Creature* c);
// On Server.exe the guard lets every creature through: a remote player's mod mode is run by the server (D2).

// The bystander path (openspec add-skills-unleashed-multiplayer, D5 (a)): on a joined Cube.exe, any creature but the local player
// whose mode is a mod number is watched: its ability's watch callbacks run (presentation only), when it defined any. Never on
// Server.exe or in single-player.
bool ability_watch_here(const cube_World* w, const cube_Creature* c);
// A watched creature's rank: its skills[9] as the server sent it, from 1 to the ability's maximum.
uint32_t ability_watch_rank(const cube_Creature* c, uint32_t number);

// The rank an activation starts with: on Server.exe the creature's skills[9] (what its client sent) bounded by the ability's
// maximum; on Cube.exe the local player's stored rank, and 1 for any other creature.
uint32_t ability_rank_for(const cube_World* w, const cube_Creature* c, uint32_t number);

// Server.exe's check of a remote player's start (openspec add-skills-unleashed-multiplayer, N1 and T3 to T5): the game has none,
// and the mode, mp and ranks are what the client sent. Checked in this order: the rank (0 refuses), the server's own cooldown
// for that player, then the mp. The mp is the higher of the value sent with the start and the one the server held before the
// start (`before_mp`, when `before_known`): the client sends its player every 20 ms from another thread, so the first update
// in the new mode may already have the cost taken off.
enum class ServerRefusal { None, Rank, Cooldown, Mp };
const char* server_refusal_name(ServerRefusal r);   // "rank 0", "cooldown", "mp"; "none"
ServerRefusal server_admit(uint32_t rank, float reported_mp, float before_mp, bool before_known, float cost, uint64_t now_ms,
                           uint64_t ready_at_ms);

// Server.exe, after each World step: remembers the mp of every player (hostile_type 0) that is not in a mod mode, for
// server_admit's `before_mp`. Called by the World detour; does nothing on Cube.exe.
void ability_server_after_step(cube_World* w);

// The hook's body: `w` is the World (EDI) and `c` the creature (EAX) at the site. Does nothing unless the creature's mode
// is a registered mod number and the game's own guard lets this side run it (`joined` 0 and a monster, or the local player).
// Game thread, inside the World step.
void ability_step_creature(cube_World* w, cube_Creature* c);

// The pass after a World step (a safe point): ends what decide_end says, and queues itself for the next step while any
// activation is left.
void ability_end_pass();
// Ends every activation with `r` at once, for a world that is left (`gone`). Calls on_end.
void ability_end_all(EndReason r);
// Marks the creature's activation to end `cancelled` at the next pass. False when it has none, or when it is watched (the
// mode is the server's: a bystander cannot end it).
bool ability_mark_cancel(KyubuCreature h);
// A callback of the running activation failed (a Lua error): the activation ends `cancelled`. Does nothing outside a callback.
void ability_callback_failed();

size_t ability_active_count();

// Installs the mid hook at abilityDispatchSite when ability_slot_available() says so; logs once why not.
void install_ability_step_hook();
void remove_ability_step_hook();

// Server.exe: sets when the server admits the ability `number` again for the creature `h` (server_gate): `ms` from now, or at
// once for 0. A server module that gives a cooldown back (a fizzled charge) calls it through ability_cooldown_set, so the
// player's next start is not refused for the cooldown the admission started. Does nothing off Server.exe.
void ability_server_cooldown_set(KyubuCreature h, uint32_t number, uint32_t ms);

// Tests: -1 asks the game whether ability_stun is available, 0 and 1 force the answer. Forgets every activation.
void set_stun_known_for_test(int known);
// Tests: the server's clock for the cooldown, in ms; -1 is GetTickCount64.
void set_server_clock_for_test(int64_t ms);
void reset_ability_step_for_test();

}  // namespace kyubu::game
