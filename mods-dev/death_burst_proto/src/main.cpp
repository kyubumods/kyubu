// death_burst_proto: probe death-burst-call (task 2.2 of add-dragon-boss). Question: can a mod burst a model of its own with the
// game's createParticles at creature_death_burst_site, and skip the game's own burst by clearing [creature+0x1D10]?
//  - draw_creature_end_site: remembers the model loader (frame EBP+0x14); after `delay` calls kills one creature
//    (hp 0 < old hp < 1000, so not the boosted player) by writing its hp (creature+0x16C) to 0.
//  - creature_death_burst_site (ESI the creature, EDI the GameController): createParticles(quiver, creature+0x1644 (the head
//    matrix), ..., colour red, type 0), then [ESI+0x1D10] = 0 when `skip` is set.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "kyubu.hpp"

namespace {

kyubu::Mod* g_mod = nullptr;

using GetFn = void*(__fastcall*)(void* loader, void* edx, int32_t slot);
// thiscall, nine stack args, callee pops 0x24: a fastcall with ECX = this and a dummy EDX takes the same stack
using BurstFn = void(__fastcall*)(void* controller, void* edx, void* model, const float* world, const float* unused_vec3,
                                  const float* velocity, float unused_factor, float origin_x, float origin_y, const float* colour,
                                  int type);

GetFn g_get = nullptr;
BurstFn g_burst = nullptr;
void* g_loader = nullptr;
int g_calls = 0, g_bursts = 0;
int g_delay = 0;
bool g_skip = true;
bool g_killed = false;
int32_t g_slot = 0x347;
float g_colour[4] = {1.0f, 0.1f, 0.1f, 1.0f};
const float g_zero3[3] = {0, 0, 0};

template <class T> T rd(uintptr_t a) { T v{}; std::memcpy(&v, reinterpret_cast<void*>(a), sizeof v); return v; }
template <class T> void wr(uintptr_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v); }



bool g_candidate = false;
uintptr_t g_target = 0;
std::chrono::steady_clock::time_point g_candidate_at;
double g_wait_s = 3.0;

void end_site(KyubuCpuContext* ctx, void*) {
    ++g_calls;
    g_loader = reinterpret_cast<void*>(rd<uint32_t>(ctx->ebp + 0x14));
    const uintptr_t creature = rd<uint32_t>(ctx->edi);
    if (!creature) return;
    if (g_killed) {
        static int after = 0;
        if (creature == g_target && ++after <= 6)
            g_mod->info("death_burst_proto: after the write, creature %08x hp %.1f flag %u", (unsigned)creature, rd<float>(creature + 0x16C), (unsigned)rd<uint8_t>(creature + 0x1D10));
        return;
    }
    const float hp = rd<float>(creature + 0x16C);
    // the target is the local player, boosted by the script to 20000 hp: the game deletes a dead slime before it renders (a hp
    // write on the client does not start the dying mode), but it keeps a dead player and still bursts it
    if (hp < 10000.0f) return;
    const auto now = std::chrono::steady_clock::now();
    if (!g_candidate) {
        g_candidate = true;
        g_candidate_at = now;
        g_mod->info("death_burst_proto: player %08x hp %.1f, kill in %.1f s", (unsigned)creature, hp, g_wait_s);
        return;
    }
    if (std::chrono::duration<double>(now - g_candidate_at).count() < g_wait_s) return;
    wr<float>(creature + 0x16C, 0.0f);
    g_killed = true;
    g_target = creature;
    g_mod->info("death_burst_proto: killed creature %08x (hp was %.1f) at call %d", (unsigned)creature, hp, g_calls);
}

void death_site(KyubuCpuContext* ctx, void*) {
    const uintptr_t creature = ctx->esi;
    void* controller = reinterpret_cast<void*>(ctx->edi);
    const uint8_t flag = rd<uint8_t>(creature + 0x1D10);
    static int logged = 0;
    if (++logged <= 3) g_mod->info("death_burst_proto: death site, creature %08x flag %u loader %p", (unsigned)creature, (unsigned)flag, g_loader);
    if (!g_loader || !flag) return;
    void* model = g_get(g_loader, nullptr, g_slot);
    if (!model) return;
    float m[16];
    std::memcpy(m, reinterpret_cast<void*>(creature + 0x1644), sizeof m);
    for (float f : m) if (!std::isfinite(f)) return;
    g_burst(controller, nullptr, model, m, g_zero3, reinterpret_cast<const float*>(creature + 0x34), 1.0f, 0.0f, 0.0f, g_colour, 0);
    ++g_bursts;
    g_mod->info("death_burst_proto: burst %d of model %p at %.2f %.2f %.2f", g_bursts, model, m[12], m[13], m[14]);
    if (g_skip) wr<uint8_t>(creature + 0x1D10, 0);
}

}  // namespace

struct DeathBurstProto {
    explicit DeathBurstProto(kyubu::Mod& m) : mod(m) {
        if (m.side() != KY_SIDE_CLIENT) return;
        g_mod = &m;
        g_delay = int(m.config_float("delay", 0));
        g_skip = m.config_float("skip", 1) != 0;
        g_slot = int32_t(m.config_float("slot", 0x347));
        const uintptr_t end = m.symbol("draw_creature_end_site"), death = m.symbol("creature_death_burst_site");
        g_get = reinterpret_cast<GetFn>(m.symbol("ModelLoader::get"));
        g_burst = reinterpret_cast<BurstFn>(m.symbol("cube::GameController::createParticles"));
        if (!end || !death || !g_get || !g_burst) {
            m.warn("death_burst_proto: a symbol is missing (end %08x death %08x get %p burst %p)", (unsigned)end, (unsigned)death, (void*)g_get, (void*)g_burst);
            return;
        }
        KyubuId id = 0;
        if (m.api()->hook_mid(m.handle(), end, &end_site, nullptr, &id) != KY_OK) m.warn("death_burst_proto: hook_mid (end) failed");
        if (m.api()->hook_mid(m.handle(), death, &death_site, nullptr, &id) != KY_OK) m.warn("death_burst_proto: hook_mid (death) failed");
        else m.info("death_burst_proto: hooked %08x and %08x (delay %d, skip %d)", (unsigned)end, (unsigned)death, g_delay, (int)g_skip);
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("death_burst_proto", KY_SIDE_CLIENT, DeathBurstProto)
