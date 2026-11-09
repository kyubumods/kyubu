// The lights primitive: point lights of mods (openspec add-light-api; docs/game-api.md, light_spawn).
//
// cube::GameController::render fills a std::vector of cube::Light (0x30 bytes) every frame: one for each lit lamp, projectile,
// spell flash and lit world object. It then sorts the vector by the squared distance to the camera, hands each light to the terrain
// regions its radius reaches (16 each) and gives the first 16 to the models (the vertex shader uses 10). light_sort_site is after
// the last push and before the sort: the lights of mods are pushed there with the game's own push_back and a sort key of 0, so they
// come first and light the ground as well. Pushed after the hand-out (light_collect_site) a light lights the models only (probe
// light-place).
#include "lights.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

#include "call.h"
#include "cubeworld/functions.h"
#include "cubeworld/types.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "modmanager.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

constexpr const char* kSortSite = "light_sort_site";
constexpr uint32_t kVectorAt = 0xC8;   // the light vector (first, last, end) from ESP at light_sort_site
constexpr uint32_t kMinSize = offsetof(KyubuLight, fade_ms) + sizeof(uint32_t);

struct Light {
    uint32_t id;
    Mod* mod;             // the owner (mods live until shutdown; a reload keeps the Mod): its lights go when it is no longer loaded
    KyubuLight def;
    uint64_t from_ms;     // when its life started (light_spawn, or a light_set with a life)
};

std::mutex g_mu;
std::vector<Light> g_lights;
uint32_t g_next_id = 1;
uint64_t g_test_clock = 0;
KyubuId g_hook = 0;

uint64_t now_ms() {
    if (g_test_clock) return g_test_clock;
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The checks light_spawn and light_set share; "" when `l` is good.
std::string check(const KyubuLight* l) {
    if (!l) return "l is NULL";
    if (l->size < kMinSize) return fmt("l->size is %u, below %u", l->size, kMinSize);
    if (l->flags) return fmt("flags is %u, not 0", l->flags);
    if (!std::isfinite(l->radius) || !(l->radius > 0.0f && l->radius <= KY_LIGHT_RADIUS_MAX))
        return fmt("radius is %g, outside above 0 to %g", l->radius, KY_LIGHT_RADIUS_MAX);
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(l->color[i]) || !(l->color[i] >= 0.0f && l->color[i] <= KY_LIGHT_COLOR_MAX))
            return fmt("color[%d] is outside 0 to %g", i, KY_LIGHT_COLOR_MAX);
    if (l->fade_ms > l->life_ms) return fmt("fade_ms is %u, above life_ms %u", l->fade_ms, l->life_ms);
    return "";
}

Mod* owner_of(KyubuModHandle self) { return ModManager::get().from_handle(self); }

Light* find(uint32_t id, const Mod* mod) {
    for (auto& l : g_lights)
        if (l.id == id && l.mod == mod) return &l;
    return nullptr;
}

KyubuResult KYUBU_CALL k_light_spawn(KyubuModHandle self, const KyubuLight* l, uint32_t* out_id) {
    GameCall g(self, Prim::lights, "light_spawn");   // client-only (kSideRules)
    if (!g) return g.result();
    if (!out_id) return g.refuse(KY_E_INVALID_ARG, "out_id is NULL");
    const std::string why = check(l);
    if (!why.empty()) return g.refuse(KY_E_INVALID_ARG, why);
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_lights.size() >= KY_LIGHT_MAX) return g.refuse(KY_E_REFUSED, fmt("%u lights are alive already", KY_LIGHT_MAX));
    Light n{g_next_id++, owner_of(self), *l, now_ms()};
    n.def.size = sizeof(KyubuLight);
    if (g_next_id == 0) g_next_id = 1;
    g_lights.push_back(n);
    *out_id = n.id;
    return KY_OK;
}

KyubuResult KYUBU_CALL k_light_set(KyubuModHandle self, uint32_t id, const KyubuLight* l) {
    GameCall g(self, Prim::lights, "light_set");
    if (!g) return g.result();
    const std::string why = check(l);
    if (!why.empty()) return g.refuse(KY_E_INVALID_ARG, why);
    std::lock_guard<std::mutex> lock(g_mu);
    Light* x = find(id, owner_of(self));
    if (!x) return g.refuse(KY_E_NOT_FOUND, fmt("no light %u of this mod", id));
    x->def = *l;
    x->def.size = sizeof(KyubuLight);
    if (l->life_ms) x->from_ms = now_ms();
    return KY_OK;
}

KyubuResult KYUBU_CALL k_light_remove(KyubuModHandle self, uint32_t id) {
    GameCall g(self, Prim::lights, "light_remove");
    if (!g) return g.result();
    std::lock_guard<std::mutex> lock(g_mu);
    const Mod* mod = owner_of(self);
    auto it = std::find_if(g_lights.begin(), g_lights.end(), [&](const Light& l) { return l.id == id && l.mod == mod; });
    if (it == g_lights.end()) return g.refuse(KY_E_NOT_FOUND, fmt("no light %u of this mod", id));
    g_lights.erase(it);
    return KY_OK;
}

bool mod_loaded(const Mod* m) { return m && m->state == ModState::Loaded; }

void on_sort_site(KyubuCpuContext* ctx) { push_lights((void*)(uintptr_t)(ctx->esp + kVectorAt), now_ms()); }

}  // namespace

void fill_lights(KyubuApi& a) {
    a.light_spawn = k_light_spawn;
    a.light_set = k_light_set;
    a.light_remove = k_light_remove;
}

void push_lights(void* vector, uint64_t now) {
    if (!vector) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_lights.empty()) return;
    const cw_light_vector_push_fn push = (cw_light_vector_push_fn)verified_symbol(CW_SYM_light_vector_push);
    if (!push) return;
    g_lights.erase(std::remove_if(g_lights.begin(), g_lights.end(),
                                  [&](const Light& l) {
                                      return (l.def.life_ms && now - l.from_ms >= l.def.life_ms) || !mod_loaded(l.mod);
                                  }),
                   g_lights.end());
    for (const Light& l : g_lights) {
        float k = 1.0f;
        if (l.def.life_ms && l.def.fade_ms) {
            const uint64_t left = l.def.life_ms - (now - l.from_ms);
            if (left < l.def.fade_ms) k = (float)left / (float)l.def.fade_ms;
        }
        cube_Light r;
        memset(&r, 0, sizeof r);
        r.pos.x = l.def.pos[0];
        r.pos.y = l.def.pos[1];
        r.pos.z = l.def.pos[2];
        r.radius = l.def.radius;
        for (int i = 0; i < 3; ++i) r.color[i] = l.def.color[i] * k;
        r.distance_sq = 0.0f;   // the sort key: first
        push(vector, nullptr, &r);
    }
}

void install_light_hook() {
    if (g_hook) return;
    std::string why;
    if (!available(Prim::lights, &why)) {
        KINFO("lights: light_spawn is off: %s", why.c_str());
        return;
    }
    const uintptr_t site = verified_symbol(kSortSite, &why);
    if (!site || Hooks::get().hook_mid("kyubu", site, KY_SIDE_CLIENT, on_sort_site, &g_hook, &why) != KY_OK) {
        g_hook = 0;
        KERROR("lights: light_spawn is off: cannot hook %s: %s", kSortSite, why.c_str());
        return;
    }
    KINFO("lights: installed at %s", kSortSite);
}

void remove_light_hook() {
    if (g_hook) Hooks::get().remove("kyubu", g_hook);
    g_hook = 0;
}

void lights_world_exit() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_lights.empty()) KINFO("lights: the world was left; %u light(s) ended", (unsigned)g_lights.size());
    g_lights.clear();
}

void lights_drop_mod(const std::string& mod_id) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_lights.erase(std::remove_if(g_lights.begin(), g_lights.end(), [&](const Light& l) { return l.mod && l.mod->id == mod_id; }),
                   g_lights.end());
}

unsigned light_count_for_test() {
    std::lock_guard<std::mutex> lock(g_mu);
    return (unsigned)g_lights.size();
}

void set_light_clock_for_test(uint64_t ms) { g_test_clock = ms; }

}  // namespace kyubu::game
