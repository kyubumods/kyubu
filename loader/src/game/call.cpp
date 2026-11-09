// GameCall (design D5).
#include "call.h"

#include <mutex>
#include <set>

#include "api.h"
#include "handles.h"
#include "log.h"
#include "modmanager.h"

namespace kyubu::game {

namespace {

const char* side_name(KyubuSide s) { return s == KY_SIDE_CLIENT ? "client" : "server"; }

// Logs a refusal once per mod, function and reason (like side_refused in api.cpp).
void refused(Mod* m, KyubuSide side, const char* fn, KyubuResult r, const std::string& why) {
    static std::mutex mu;
    static std::set<std::string> seen;
    const std::string id = m ? m->id : std::string("?");
    const std::string key = id + "/" + fn + "/" + std::to_string((int)r) + "/" + why;
    {
        std::lock_guard<std::mutex> g(mu);
        if (!seen.insert(key).second) return;
    }
    Log::get().write(KY_LOG_WARN, id, side, std::string(fn) + " refused: " + why);
}

thread_local std::string t_last_refusal;

}  // namespace

const std::string& last_refusal() { return t_last_refusal; }

KyubuResult note_refusal(Mod* m, KyubuSide side, const char* fn, KyubuResult r, const std::string& why) {
    t_last_refusal = why;
    refused(m, side, fn, r, why);
    return r;
}

void clear_refusal() { t_last_refusal.clear(); }

GameCall::GameCall(KyubuModHandle self, Prim prim, const char* fn) : fn_(fn) {
    t_last_refusal.clear();
    mod_ = ModManager::get().from_handle(self, &side_);
    if (!mod_ || (side_ != KY_SIDE_CLIENT && side_ != KY_SIDE_SERVER)) {
        r_ = KY_E_INVALID_ARG;   // not a mod handle: nothing to log against
        return;
    }
    if (mod_->state == ModState::Faulted) {
        r_ = KY_E_FAULTED;
        return;
    }
    // The function's side rule (kSideRules). A name with no row passes: api_side_allowed would refuse it, and the
    // test-only names in tests/unit/test_game.cpp have none. Real game functions always have a row (test_api.cpp
    // checks the count, test_game_api_checklist.py the names). Checked before WorldAccess, so a refused call takes
    // no lock and touches no game memory.
    if (api_side_of(fn) != KY_SIDE_NONE && !api_side_allowed(fn, side_)) {
        fail(KY_E_SIDE, std::string(fn) + " is " + side_name(api_side_of(fn)) + "-only and was called from the " +
                            side_name(side_) + " side");
        return;
    }
    access_ = std::make_unique<WorldAccess>(prim, side_);
    if (!*access_) {
        fail(access_->result(), access_->why());
        access_.reset();
    }
}

bool GameCall::fail(KyubuResult r, const std::string& why) {
    r_ = r;
    why_ = why;
    t_last_refusal = why;
    refused(mod_, side_, fn_, r, why);
    return false;
}

KyubuResult GameCall::refuse(KyubuResult r, const std::string& why) {
    fail(r, why);
    return r;
}

// Without the destructor detour a handle would outlive its creature, so no handle is made or resolved without
// `handles`.
bool GameCall::need(Prim p) {
    if (r_ != KY_OK) return false;
    std::string w;
    return available(p, &w) || fail(KY_E_UNAVAILABLE, std::string(prim_name(p)) + " is unavailable: " + w);
}

cube_Creature* GameCall::creature(KyubuCreature h) {
    if (r_ != KY_OK || !need(Prim::handles)) return nullptr;
    KyubuResult r = KY_OK;
    std::string why;
    cube_Creature* c = resolve_handle(side_, h, &r, &why);
    if (!c) fail(r, why);
    return c;
}

KyubuCreature GameCall::handle(cube_Creature* c) {
    if (r_ != KY_OK || !need(Prim::handles)) return 0;
    const KyubuCreature h = handle_for(side_, c);
    if (!h) fail(KY_E_INVALID_ARG, "no creature");
    return h;
}

bool GameCall::owns(const cube_Creature* c) {
    if (r_ != KY_OK) return false;
    const KyubuSide owner = owner_of(world(), c);
    if (owner == side_) return true;
    return fail(KY_E_SIDE, std::string("the ") + side_name(owner) + " side owns this creature" +
                               (owner == KY_SIDE_CLIENT ? " (the local player)" : "") + "; it cannot be changed from the " +
                               side_name(side_) + " side");
}

}  // namespace kyubu::game
