// model_draw_proto: probe model-part-draw (task 2.1 of add-dragon-boss). At draw_creature_end_site (the common exit of
// draw_creature) it draws a model of the game's own, with the calls the game uses for a creature part:
//   setColour(shader, colour), setMatrices(shader, world, view, projection), Model::draw(model).
// The world matrix is the last part's matrix (draw record + 0x4EC) copied and moved with the game's translate maths.
// The shader, view, projection, model loader and colour are draw_creature's own arguments, read from its frame (EBP).
#include <cmath>
#include <cstdint>
#include <cstring>

#include "kyubu.hpp"

namespace {

kyubu::Mod* g_mod = nullptr;

using GetFn = void*(__fastcall*)(void* loader, void* edx, int32_t slot);                       // ModelLoader::get
using SetColourFn = void(__fastcall*)(void* shader, void* edx, const float* colour);           // CubeShader::setColour
using SetMatricesFn = void(__fastcall*)(void* shader, void* edx, const float* w, const float* v, const float* p);
using DrawFn = void(__fastcall*)(void* model, void* edx);                                      // Model::draw

GetFn g_get = nullptr;
SetColourFn g_colour = nullptr;
SetMatricesFn g_matrices = nullptr;
DrawFn g_draw = nullptr;
int g_calls = 0, g_drawn = 0;
float g_dx = 0, g_dy = 0, g_dz = 0;
int32_t g_slot = 0x347;

template <class T> T rd(uintptr_t a) { T v{}; std::memcpy(&v, reinterpret_cast<void*>(a), sizeof v); return v; }

void end_site(KyubuCpuContext* ctx, void*) {
    ++g_calls;
    const uintptr_t record = ctx->edi, frame = ctx->ebp;
    void* shader = reinterpret_cast<void*>(rd<uint32_t>(frame + 0x8));
    const float* view = reinterpret_cast<const float*>(rd<uint32_t>(frame + 0xC));
    const float* proj = reinterpret_cast<const float*>(rd<uint32_t>(frame + 0x10));
    void* loader = reinterpret_cast<void*>(rd<uint32_t>(frame + 0x14));
    const float* colour = reinterpret_cast<const float*>(rd<uint32_t>(frame + 0x1C));
    if (!shader || !view || !proj || !loader || !colour) return;
    float m[16];
    std::memcpy(m, reinterpret_cast<void*>(record + 0x4EC), sizeof m);
    for (float f : m) if (!std::isfinite(f)) return;
    void* model = g_get(loader, nullptr, g_slot);
    if (g_calls <= 5 || (g_calls % 600) == 0)
        g_mod->info("model_draw_proto: call %d record %08x creature %08x model %08x m15 %.3f t %.2f %.2f %.2f", g_calls, (unsigned)record,
                    (unsigned)rd<uint32_t>(record), (unsigned)(uintptr_t)model, m[15], m[12], m[13], m[14]);
    if (!model || m[15] != 1.0f) return;
    // the matrix is in world blocks (the player's translation read -0.25, 0.01, 146.01): move it in the world, z up
    m[12] += g_dx;
    m[13] += g_dy;
    m[14] += g_dz;
    g_colour(shader, nullptr, colour);
    g_matrices(shader, nullptr, m, view, proj);
    g_draw(model, nullptr);
    ++g_drawn;
}

}  // namespace

struct ModelDrawProto {
    explicit ModelDrawProto(kyubu::Mod& m) : mod(m) {
        if (m.side() != KY_SIDE_CLIENT) return;
        g_mod = &m;
        g_dx = float(m.config_float("dx", 2.0));   // blocks, world x
        g_dy = float(m.config_float("dy", 0.0));
        g_dz = float(m.config_float("dz", 3.0));   // blocks, world z (up)
        g_slot = int32_t(m.config_float("slot", 0x347));
        const uintptr_t site = m.symbol("draw_creature_end_site");
        g_get = reinterpret_cast<GetFn>(m.symbol("ModelLoader::get"));
        g_colour = reinterpret_cast<SetColourFn>(m.symbol("cube::CubeShader::setColour"));
        g_matrices = reinterpret_cast<SetMatricesFn>(m.symbol("cube::CubeShader::setMatrices"));
        g_draw = reinterpret_cast<DrawFn>(m.symbol("Model::draw"));
        if (!site || !g_get || !g_colour || !g_matrices || !g_draw) {
            m.warn("model_draw_proto: a symbol is missing (site %08x get %p colour %p matrices %p draw %p)", (unsigned)site, (void*)g_get, (void*)g_colour,
                   (void*)g_matrices, (void*)g_draw);
            return;
        }
        KyubuId id = 0;
        if (m.api()->hook_mid(m.handle(), site, &end_site, nullptr, &id) != KY_OK) m.warn("model_draw_proto: hook_mid failed");
        else m.info("model_draw_proto: hooked draw_creature_end_site at %08x", (unsigned)site);
        m.on("frame.begin", [](KyubuEvent&) {
            static int n = 0;
            if (++n % 300 == 0 && g_mod) g_mod->info("model_draw_proto: %d hook calls, %d drawn", g_calls, g_drawn);
        });
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("model_draw_proto", KY_SIDE_CLIENT, ModelDrawProto)
