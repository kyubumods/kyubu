// The step runner and the ends of a mod ability (design D6). An activation is one creature running one mod ability, kept
// from its first step until the pass after a World step ends it. The hook and the pass run on the game thread, inside the
// World's lock; the one exception is a world that is left (ability_end_all), which can come from another thread, so the
// table has a recursive mutex.
#include "ability_step.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "abilities.h"
#include "ability_bar.h"
#include "ability_skilltree.h"
#include "ability_stats.h"
#include "animations.h"
#include "game.h"
#include "handles.h"
#include "hooks.h"
#include "kyubu_api.h"
#include "loader.h"
#include "log.h"
#include "modmanager.h"
#include "registry.h"
#include "symbols.h"
#include "world.h"

namespace kyubu::game {

namespace {

constexpr const char* kSite = "cube::World::abilityDispatchSite";
constexpr double kSlowCallbackMs = 2.0;       // design "Risks": a slower callback slows the World step
constexpr uint64_t kSlowWarnEveryMs = 5000;   // one warning per ability in this time, not one per step

struct Activation {
    KyubuCreature creature = 0;
    KyubuSide side = KY_SIDE_NONE;
    uint32_t number = 0;
    uint32_t rank = 1;
    std::string id;
    uint32_t last_elapsed = 0;
    uint32_t steps = 0;
    bool cancel = false;
    bool watch = false;   // a bystander's copy of another creature's run: the watch callbacks, presentation only
    bool reported = false;   // the mode is another game's report (watched, or a remote player on Server.exe): kReportEndSlackMs
    // Markers: the mode time the hook read at the last step (-1 before the first), and where this step's span starts.
    double step_elapsed = -1;
    double marker_from = -1;
    uint64_t game_id = 0;     // the creature's game id: its own clip binding (ability_animation_bind_for) gives the markers
    uint32_t restarts = 0;    // the restarts of this run seen so far (KyubuAbilityDef.restarts)
};

enum class Which { Start, Step, End };

std::recursive_mutex g_mu;
std::map<KyubuCreature, Activation> g_active;
bool g_pass_queued = false;
Activation* g_current = nullptr;   // the activation whose callback runs now
KyubuId g_hook = 0;
int g_stun_override = -1;
std::map<uint32_t, uint64_t> g_slow_warned;   // ability number -> tick of its last warning

// Server.exe's admission of a remote player's start (server_admit). A refused run is remembered until the client's own run of it
// must be over, so the updates that follow with the same mode are refused without a second check or a second log line.
struct Refused {
    uint32_t number = 0;
    uint32_t last_elapsed = 0;
    uint64_t until = 0;
};
std::map<KyubuCreature, std::map<uint32_t, uint64_t>> g_ready_at;   // player -> ability number -> server time it is ready again
std::map<KyubuCreature, Refused> g_refused;
std::map<const cube_Creature*, float> g_before_mp;   // a player's mp after the last step it was in no mod mode
int64_t g_clock_override = -1;

bool stun_known() {
    if (g_stun_override >= 0) return g_stun_override == 1;
    return available(Prim::ability_stun);
}

double now_ms() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

// Runs the callback of `which` for `a`. A mod that is not loaded any more, or a native fault, fails the call; a Lua error
// reaches ability_callback_failed. Either marks the activation cancelled (except in on_end, where nothing is left to cancel).
void run_callback(Activation& a, Which which, EndReason reason, uint32_t elapsed, uint32_t dt) {
    const AbilityCallbacks* cb = ability_callbacks(a.id, a.side);
    if (!cb) return;
    const KyubuAbilityFn fn = a.watch ? (which == Which::Start ? cb->watch_start : which == Which::Step ? cb->watch_step : cb->watch_end)
                                      : (which == Which::Start ? cb->on_start : which == Which::Step ? cb->on_step : cb->on_end);
    void* const user = a.watch ? cb->watch_user : cb->user;
    if (!fn) return;
    KyubuAbilityCtx ctx = {};
    ctx.size = sizeof ctx;
    ctx.creature = a.creature;
    ctx.number = a.number;
    ctx.id = a.id.c_str();
    ctx.elapsed_ms = elapsed;
    ctx.dt_ms = dt;
    ctx.steps = a.steps;
    ctx.rank = a.rank;
    ctx.side = (int32_t)a.side;
    ctx.reason = (int32_t)reason;
    ctx.watch = a.watch ? 1u : 0u;
    ctx.restarts = a.restarts;
    // The markers of the bound clip crossed since the previous step, from the mode time on this side (design D6). on_end gets those
    // between the last step and the end time, so a marker at the clip's end is told once; on_start gets none.
    std::vector<std::string> marker_names;
    std::vector<const char*> marker_ptrs;
    if (which != Which::Start) {
        const AbilityStat* stat = ability_stat(a.number);
        marker_names = Animations::get().ability_markers(a.id, a.game_id, which == Which::Step ? a.marker_from : a.step_elapsed, (double)elapsed,
                                                         stat ? (float)stat->duration_ms : 0);
        for (const std::string& n : marker_names) marker_ptrs.push_back(n.c_str());
        ctx.markers = marker_ptrs.empty() ? nullptr : marker_ptrs.data();
        ctx.marker_count = (uint32_t)marker_ptrs.size();
    }
    const char* name = which == Which::Start ? "on_start" : which == Which::Step ? "on_step" : "on_end";
    const std::string what = a.id + (a.watch ? " watch " : " ") + name;
    Mod* mod = ModManager::get().find(cb->mod);   // null for the loader's own and for the tests' stand-ins
    Activation* prev = g_current;
    g_current = &a;
    const double t0 = now_ms();
    const bool ok = ModManager::get().invoke(mod, a.side, what.c_str(), [&] { fn(user, &ctx); });
    const double took = now_ms() - t0;
    g_current = prev;
    if (!ok && which != Which::End) a.cancel = true;
    if (!ok && which == Which::End)
        KWARN("abilities: %s did not run for the end `%s` (the mod is faulted or disabled)", what.c_str(), end_reason_name(reason));
    if (took > kSlowCallbackMs) {
        const uint64_t now = GetTickCount64();
        uint64_t& last = g_slow_warned[a.number];
        if (last == 0 || now - last >= kSlowWarnEveryMs) {
            last = now;
            KWARN("abilities: %s took %.1f ms (more than %.0f ms): it runs inside the World step", what.c_str(), took, kSlowCallbackMs);
        }
    }
}

// The end: on_end once with the reason, and one line in the log. The caller drops the record.
void finish(Activation& a, EndReason reason) {
    KINFO("abilities: %s%s ends `%s` on creature %llu after %u step(s), %u ms", a.id.c_str(), a.watch ? " (watched)" : "",
          end_reason_name(reason), (unsigned long long)a.creature, a.steps, a.last_elapsed);
    run_callback(a, Which::End, reason, a.last_elapsed, 0);
}

uint64_t server_now() { return g_clock_override >= 0 ? (uint64_t)g_clock_override : GetTickCount64(); }

void take_mode_back(cube_Creature* c) {
    c->data.current_mode = 0;
    c->data.mode_start_time = 0;
}

// A remote player's start on Server.exe: true when it may run. A refusal takes the mode back in the server's copy, so the
// server runs nothing for it; the client's own player is not corrected (the client applies no update to its own player).
bool server_gate(cube_World* w, cube_Creature* c, KyubuCreature h, uint32_t mode, uint32_t elapsed, const AbilityStat* stat) {
    const uint64_t now = server_now();
    auto rf = g_refused.find(h);
    if (rf != g_refused.end()) {
        if (rf->second.number == mode && elapsed >= rf->second.last_elapsed && now < rf->second.until) {
            rf->second.last_elapsed = elapsed;   // the same refused run, one update later
            take_mode_back(c);
            return false;
        }
        g_refused.erase(rf);
    }
    const uint32_t rank = ability_rank_for(w, c, mode);
    std::map<uint32_t, uint64_t>& ready = g_ready_at[h];
    for (auto it = ready.begin(); it != ready.end();) it = it->second <= now ? ready.erase(it) : std::next(it);
    const auto ra = ready.find(mode);
    const uint64_t ready_at = ra == ready.end() ? 0 : ra->second;
    const auto bm = g_before_mp.find(c);
    const bool before_known = bm != g_before_mp.end();
    const float before = before_known ? bm->second : 0.0f;
    const float sent = c->data.mp;
    const ServerRefusal why = server_admit(rank, sent, before, before_known, stat->mp_cost, now, ready_at);
    if (why != ServerRefusal::None) {
        std::string id;
        Registries::get().name_of("kyubu:abilities", mode, &id);
        KINFO("abilities: Server.exe refuses %s (number %u) on creature %llu: %s (rank %u; mp %.3f sent, %.3f before%s; cost %.3f; "
              "cooldown %llu ms left)",
              id.c_str(), mode, (unsigned long long)h, server_refusal_name(why), rank, sent, before, before_known ? "" : " (unknown)",
              stat->mp_cost, (unsigned long long)(ready_at > now ? ready_at - now : 0));
        g_refused[h] = Refused{mode, elapsed, now + stat->duration_ms + 1000};
        if (ready.empty()) g_ready_at.erase(h);
        take_mode_back(c);
        return false;
    }
    int32_t cd = 0;
    if (stat_cooldown(mode, (int32_t)rank, false, &cd) && cd > 0) ready[mode] = now + (uint64_t)cd;
    if (ready.empty()) g_ready_at.erase(h);
    if (before_known && before > sent) c->data.mp = before;   // the step runner takes the cost once, from the higher value
    return true;
}

}  // namespace

void ability_server_cooldown_set(KyubuCreature h, uint32_t number, uint32_t ms) {
    if (L().host.host != KY_HOST_SERVER) return;
    std::lock_guard<std::recursive_mutex> g(g_mu);
    if (ms > 0) {
        g_ready_at[h][number] = server_now() + ms;
        return;
    }
    const auto it = g_ready_at.find(h);
    if (it == g_ready_at.end()) return;
    it->second.erase(number);
    if (it->second.empty()) g_ready_at.erase(it);
}

namespace {

void queue_pass() {
    if (g_pass_queued) return;
    g_pass_queued = true;
    after_step([] { ability_end_pass(); });
}

void on_dispatch(KyubuCpuContext* c) {
    auto* w = (cube_World*)(uintptr_t)c->edi;
    auto* creature = (cube_Creature*)(uintptr_t)c->eax;
    if (w && creature) ability_step_creature(w, creature);
}

}  // namespace

const char* server_refusal_name(ServerRefusal r) {
    switch (r) {
        case ServerRefusal::Rank: return "rank 0";
        case ServerRefusal::Cooldown: return "cooldown";
        case ServerRefusal::Mp: return "mp";
        default: return "none";
    }
}

ServerRefusal server_admit(uint32_t rank, float reported_mp, float before_mp, bool before_known, float cost, uint64_t now_ms,
                           uint64_t ready_at_ms) {
    if (rank == 0) return ServerRefusal::Rank;
    if (now_ms < ready_at_ms) return ServerRefusal::Cooldown;
    if (cost > 0) {
        const float have = before_known ? std::max(reported_mp, before_mp) : reported_mp;
        if (have < cost - 1e-4f) return ServerRefusal::Mp;   // a little slack: the client's own check passed at the same value
    }
    return ServerRefusal::None;
}

void ability_server_after_step(cube_World* w) {
    if (L().host.host != KY_HOST_SERVER || !w) return;
    std::lock_guard<std::recursive_mutex> g(g_mu);
    std::map<const cube_Creature*, float> next;
    for (const auto& [id, c] : world_creatures(w)) {
        (void)id;
        if (!c || c->data.hostile_type != 0) continue;
        const uint32_t mode = c->data.current_mode;
        if (mode >= kAbilityPlaceholderNumber && ability_stat(mode)) {   // in a mod mode: keep the value from before it
            const auto it = g_before_mp.find(c);
            if (it != g_before_mp.end()) next.emplace(c, it->second);
            continue;
        }
        next.emplace(c, c->data.mp);
    }
    g_before_mp.swap(next);
}

bool ability_runs_here(const cube_World* w, const cube_Creature* c) {
    if (L().host.host == KY_HOST_SERVER) return true;   // Server.exe: every creature with a mod mode, a remote player's too (its mode is the client's report)
    return (!w->joined && c->data.hostile_type != 0) || c == w->local_player;
}

bool ability_watch_here(const cube_World* w, const cube_Creature* c) {
    return L().host.host != KY_HOST_SERVER && w->joined && c != w->local_player;
}

uint32_t ability_watch_rank(const cube_Creature* c, uint32_t number) {
    const AbilityStat* s = ability_stat(number);
    const uint32_t max = s ? std::max<uint32_t>(s->max_rank, 1) : 1;
    return std::clamp<uint32_t>(c->data.skills[kSkillAbility4], 1, max);
}

const char* end_reason_name(EndReason r) {
    switch (r) {
        case EndReason::Done: return "done";
        case EndReason::Cancelled: return "cancelled";
        case EndReason::Stunned: return "stunned";
        case EndReason::Died: return "died";
        case EndReason::Interrupted: return "interrupted";
        case EndReason::Gone: return "gone";
        default: return "none";
    }
}

EndDecision decide_end(const EndFacts& f) {
    if (f.gone) return {EndReason::Gone, false};
    const bool still = f.mode == f.number;
    if (f.hp <= 0) return {EndReason::Died, still};
    if (f.cancel_marked) return {EndReason::Cancelled, still};
    if (f.stun_time > 0) return {f.stun_known ? EndReason::Stunned : EndReason::Interrupted, still};
    if (still) {
        if (f.elapsed_ms >= f.duration_ms) return {EndReason::Done, true};
        return {EndReason::None, false};
    }
    if (f.mode == 0 && (uint64_t)f.elapsed_ms + f.report_slack_ms >= f.duration_ms) return {EndReason::Done, false};
    return {EndReason::Interrupted, false};
}

EndDecision decide_watch_end(const EndFacts& f) {
    if (f.gone) return {EndReason::Gone, false};
    if (f.mode == f.number) return {EndReason::None, false};
    if (f.cancel_marked) return {EndReason::Cancelled, false};
    if (f.hp <= 0) return {EndReason::Died, false};
    if (f.stun_time > 0) return {f.stun_known ? EndReason::Stunned : EndReason::Interrupted, false};
    if (f.mode == 0 && (uint64_t)f.elapsed_ms + f.report_slack_ms >= f.duration_ms) return {EndReason::Done, false};
    return {EndReason::Interrupted, false};
}

float mp_after_cost(float mp, float cost) { return std::max(0.0f, mp - cost); }

StepKind classify_step(bool has_record, uint32_t record_number, uint32_t record_last_elapsed, uint32_t mode, uint32_t elapsed_ms,
                       uint32_t step_back_ms) {
    if (!has_record) return StepKind::Start;
    if (record_number != mode) return StepKind::Replace;
    if (elapsed_ms < record_last_elapsed) return record_last_elapsed - elapsed_ms <= step_back_ms ? StepKind::Hold : StepKind::Replace;
    return StepKind::Continue;
}

bool is_restart(bool reported, uint32_t record_number, uint32_t record_last_elapsed, uint32_t mode, uint32_t elapsed_ms,
                uint32_t restarts_so_far, uint32_t allowed) {
    if (!reported || record_number != mode || restarts_so_far >= allowed) return false;
    if (elapsed_ms >= record_last_elapsed || record_last_elapsed - elapsed_ms <= kWatchStepBackMs) return false;
    return elapsed_ms <= kRestartWindowMs;
}

uint32_t ability_rank_for(const cube_World* w, const cube_Creature* c, uint32_t number) {
    if (L().host.host == KY_HOST_SERVER) {   // the rank is the creature's own skills[9], as the client sent it, bounded by the ability
        const AbilityStat* s = ability_stat(number);
        return std::min<uint32_t>(c->data.skills[kSkillAbility4], s ? s->max_rank : 1);
    }
    return c == w->local_player ? ability_rank_of(number) : 1;   // the ranks are the local player's, as in ability_stats
}

void ability_step_creature(cube_World* w, cube_Creature* c) {
    const uint32_t mode = c->data.current_mode;
    if (mode < kAbilityPlaceholderNumber) return;   // vanilla, or no mode: the common case, nothing else to do
    const AbilityStat* stat = ability_stat(mode);
    if (!stat) return;
    const bool watch = !ability_runs_here(w, c);
    if (watch && !ability_watch_here(w, c)) return;
    std::lock_guard<std::recursive_mutex> g(g_mu);
    const KyubuSide side = watch ? KY_SIDE_CLIENT : owner_of(w, c);
    const KyubuCreature h = handle_for(side, c);
    if (!h) return;
    uint32_t elapsed = c->data.mode_start_time;
    // The mode is another game's report: a watched creature, or a remote player on Server.exe. Each update can set its time back a
    // little (B2); on the server a back step taken for a new start was refused (cooldown) and cut the run for everyone.
    const bool reported = watch || (L().host.host == KY_HOST_SERVER && c->data.hostile_type == 0);
    auto it = g_active.find(h);
    StepKind kind = classify_step(it != g_active.end(), it != g_active.end() ? it->second.number : 0,
                                  it != g_active.end() ? it->second.last_elapsed : 0, mode, elapsed, reported ? kWatchStepBackMs : 0);
    if (kind == StepKind::Hold) {   // an update set the reported time back a little: the run goes on at the time it had
        elapsed = it->second.last_elapsed;
        kind = StepKind::Continue;
    }
    bool restart = false;
    if (kind == StepKind::Replace) {   // the owner set its own time back to about 0 (a release after a charge): the same run goes on
        const AbilityCallbacks* cb = ability_callbacks(it->second.id, side);
        if (cb && is_restart(reported, it->second.number, it->second.last_elapsed, mode, elapsed, it->second.restarts, cb->restarts)) {
            Activation& r = it->second;
            ++r.restarts;
            KINFO("abilities: %s%s restarts (%u of %u) on creature %llu at %u ms, after %u ms", r.id.c_str(), r.watch ? " (watched)" : "",
                  r.restarts, cb->restarts, (unsigned long long)r.creature, elapsed, r.last_elapsed);
            restart = true;
            kind = StepKind::Continue;
        }
    }
    if (kind == StepKind::Replace) {
        finish(it->second, EndReason::Interrupted);
        g_active.erase(it);
        it = g_active.end();
        kind = StepKind::Start;
    }
    uint32_t dt = elapsed;
    if (kind == StepKind::Start) {
        if (L().host.host == KY_HOST_SERVER && c->data.hostile_type == 0 && !server_gate(w, c, h, mode, elapsed, stat)) return;
        std::string id;
        if (Registries::get().name_of("kyubu:abilities", mode, &id) != KY_OK) return;
        if (watch) {   // only an ability with watch callbacks is watched: the others cost a bystander nothing
            const AbilityCallbacks* cb = ability_callbacks(id, KY_SIDE_CLIENT);
            if (!cb || !(cb->watch_start || cb->watch_step || cb->watch_end)) return;
        }
        Activation a;
        a.creature = h;
        a.side = side;
        a.number = mode;
        a.rank = watch ? ability_watch_rank(c, mode) : ability_rank_for(w, c, mode);
        a.id = id;
        a.watch = watch;
        a.reported = reported;
        a.game_id = (uint64_t)c->id;
        it = g_active.emplace(h, std::move(a)).first;
        if (watch) {
            // nothing is taken or told: the mp and the mode are the server's
        } else if (stat->mp_cost > 0) c->data.mp = mp_after_cost(c->data.mp, stat->mp_cost);   // the game takes none for a mod ability
        if (mode == kAbilityPlaceholderNumber && c == w->local_player) ability_placeholder_started(c->data.class_type);
    } else {
        dt = restart ? 0 : elapsed - it->second.last_elapsed;
    }
    Activation& a = it->second;
    ++a.steps;
    a.marker_from = restart ? -1 : a.step_elapsed;   // -1 on the first step and at a restart: a marker at t = 0 is told once
    a.step_elapsed = elapsed;
    a.last_elapsed = elapsed;
    if (kind == StepKind::Start) run_callback(a, Which::Start, EndReason::None, elapsed, dt);
    if (!a.cancel) run_callback(a, Which::Step, EndReason::None, elapsed, dt);
    queue_pass();
}

void ability_end_pass() {
    std::lock_guard<std::recursive_mutex> g(g_mu);
    g_pass_queued = false;
    const bool stun = stun_known();
    std::vector<KyubuCreature> keys;
    keys.reserve(g_active.size());
    for (auto& kv : g_active) keys.push_back(kv.first);
    for (KyubuCreature h : keys) {
        auto it = g_active.find(h);
        if (it == g_active.end()) continue;   // an on_end ended the others
        Activation& a = it->second;
        cube_Creature* c = resolve_handle(a.side, h);
        const AbilityStat* stat = ability_stat(a.number);
        EndFacts f;
        f.number = a.number;
        f.gone = c == nullptr;
        f.stun_known = stun;
        f.cancel_marked = a.cancel;
        f.duration_ms = stat ? stat->duration_ms : 0;
        f.elapsed_ms = a.last_elapsed;
        f.report_slack_ms = a.reported ? kReportEndSlackMs : 0;
        if (c) {
            f.mode = c->data.current_mode;
            f.hp = c->data.hp;
            f.stun_time = c->data.stun_time;
            // The mode time moves on inside the step, after the hook read it: the pass sees a later time than the hook did.
            // a.last_elapsed stays the hook's, so that dt_ms is the difference of two hook readings.
            if (f.mode == a.number) f.elapsed_ms = c->data.mode_start_time;
        }
        const EndDecision d = a.watch ? decide_watch_end(f) : decide_end(f);
        if (d.reason == EndReason::None) continue;
        if (c && f.mode == a.number) a.last_elapsed = f.elapsed_ms;   // on_end reports the time the ability ended at
        if (d.write_mode_zero && c) {
            c->data.current_mode = 0;
            c->data.mode_start_time = 0;
        }
        finish(a, d.reason);
        g_active.erase(h);
    }
    if (!g_active.empty()) queue_pass();
}

void ability_end_all(EndReason r) {
    std::lock_guard<std::recursive_mutex> g(g_mu);
    std::map<KyubuCreature, Activation> all;
    all.swap(g_active);
    g_pass_queued = false;
    g_ready_at.clear();   // the world is left: the server's cooldowns and refusals go with it
    g_refused.clear();
    g_before_mp.clear();
    for (auto& kv : all) finish(kv.second, r);
}

bool ability_mark_cancel(KyubuCreature h) {
    std::lock_guard<std::recursive_mutex> g(g_mu);
    auto it = g_active.find(h);
    if (it == g_active.end() || it->second.watch) return false;
    it->second.cancel = true;
    return true;
}

void ability_callback_failed() {
    std::lock_guard<std::recursive_mutex> g(g_mu);
    if (g_current) g_current->cancel = true;
}

size_t ability_active_count() {
    std::lock_guard<std::recursive_mutex> g(g_mu);
    return g_active.size();
}

void install_ability_step_hook() {
    if (g_hook) return;
    std::string why;
    if (!ability_slot_available(&why)) return;   // game.cpp's check_entries has logged why
    const uintptr_t site = verified_symbol(kSite, &why);
    std::string err = why;
    if (!site || Hooks::get().hook_mid("kyubu", site, L().host.host == KY_HOST_SERVER ? KY_SIDE_SERVER : KY_SIDE_CLIENT, on_dispatch, &g_hook, &err) != KY_OK) {
        g_hook = 0;
        KERROR("abilities: the fourth slot is off: cannot hook %s: %s", kSite, err.c_str());
        return;
    }
    KINFO("abilities: the step runner is installed at %s", kSite);
}

void remove_ability_step_hook() {
    if (g_hook) Hooks::get().remove("kyubu", g_hook);
    g_hook = 0;
}

void set_stun_known_for_test(int known) { g_stun_override = known; }
void set_server_clock_for_test(int64_t ms) { g_clock_override = ms; }

void reset_ability_step_for_test() {
    std::lock_guard<std::recursive_mutex> g(g_mu);
    g_active.clear();
    g_pass_queued = false;
    g_current = nullptr;
    g_slow_warned.clear();
    g_stun_override = -1;
    g_ready_at.clear();
    g_refused.clear();
    g_before_mp.clear();
    g_clock_override = -1;
}

}  // namespace kyubu::game
