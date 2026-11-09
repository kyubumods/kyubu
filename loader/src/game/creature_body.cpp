// The creature_body primitive (openspec add-dragon-boss, D2): see creature_body.h.
//
// Per attached creature the loader keeps an Instance (creature_body_core.h) and the last matrices of its pieces. Drawing:
//  - creature_draw_site (and _b, _c when they resolve) writes the hide mask argument (ESP+0x20) to all ones, so the game draws none of
//    the creature's own parts;
//  - draw_creature_end_site, the common exit of draw_creature, advances the instance's clock (once per frame: draws of one frame are
//    under 1 ms apart), poses it and draws each piece with the game's own setColour, setMatrices and Model::draw (probe model-part-draw);
//  - creature_death_burst_site, on a creature with hp <= 0, bursts each piece with createParticles at its last matrix and writes 0 to
//    the game's draw flag so it skips its own burst (probe death-burst-call); the body then stays hidden and undrawn until the creature
//    goes.
#include "creature_body.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "call.h"
#include "creature_body_core.h"
#include "game.h"
#include "handles.h"
#include "hooks.h"
#include "log.h"
#include "models.h"
#include "modmanager.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

using Clock = std::chrono::steady_clock;

struct Attached {
    body::Instance inst;
    std::vector<std::array<float, 16>> mats;   // the last drawn matrix of each piece
    std::vector<body::MarkerHit> pending;      // markers passed, not yet read
    Clock::time_point last_adv{};
    bool advanced = false;
    bool dead = false;                         // burst: hidden, no longer drawn
    int drawn = 0;                             // frames drawn (the log says the first)
};

std::mutex g_mu;
std::unordered_map<uint64_t, Attached> g_att;
std::map<uint32_t, std::shared_ptr<const body::BodyDef>> g_defs;
uint32_t g_next_def = 1;
size_t g_count_hint = 0;   // g_att.size(), read without the lock by the hooks (a stale 0 only delays a draw by a frame)

KyubuId g_hooks[6] = {};

template <class T> T rd(uintptr_t a) { T v{}; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v); return v; }
template <class T> void wr(uintptr_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v); }

constexpr uintptr_t kModelSize = 0x44;   // Model/Sprite: int x, y, z (probe death-burst-call read the size here)
constexpr uintptr_t kDrawFlag = 0x1D10;
constexpr uint32_t kHideAll = 0xFFFFFFFFu;

using SetColourFn = void(__fastcall*)(void* shader, void*, const float* colour);
using SetMatricesFn = void(__fastcall*)(void* shader, void*, const float* w, const float* v, const float* p);
using DrawFn = void(__fastcall*)(void* model, void*);
using BurstFn = void(__fastcall*)(void* controller, void*, void* model, const float* world, const float* unused_vec3, const float* velocity,
                                  float unused_factor, float origin_x, float origin_y, const float* colour, int type);
SetColourFn g_colour = nullptr;
SetMatricesFn g_matrices = nullptr;
DrawFn g_draw = nullptr;
BurstFn g_burst = nullptr;

uint64_t creature_id_at(uintptr_t creature) { return creature ? rd<uint64_t>(creature + offsetof(cube_Creature, id)) : 0; }

// The game draws in a space moved to put the world's middle at 0 in x and y (GameController.render_origin, mapping types/core.toml: minus
// 8396928 blocks each; z unmoved). A matrix handed to the shader is in it, and a float cannot hold the world position itself (8.4 million blocks).
void render_origin(double* ox, double* oy) {
    *ox = *oy = 0;
    const uintptr_t global = verified_symbol("cube::game_controller");
    const uintptr_t gc = global ? rd<uint32_t>(global) : 0;
    if (!gc) return;
    *ox = std::fabs((double)rd<int64_t>(gc + 0x1D8));
    *oy = std::fabs((double)rd<int64_t>(gc + 0x1E0));
}

// The creature's place in render space (blocks) and its yaw in radians.
void creature_place(const cube_Creature* cr, float pos[3], float* yaw) {
    double ox, oy;
    render_origin(&ox, &oy);
    pos[0] = (float)(((double)cr->data.pos.x - ox) / 65536.0);
    pos[1] = (float)(((double)cr->data.pos.y - oy) / 65536.0);
    pos[2] = (float)((double)cr->data.pos.z / 65536.0);
    *yaw = cr->data.body_yaw * 0.017453292519943295f;
}

void hide_parts(uintptr_t creature, uintptr_t esp) {
    if (g_count_hint == 0 || !creature) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_att.count(creature_id_at(creature))) wr<uint32_t>(esp + 0x20, kHideAll);
}

KyubuResult not_attached(GameCall& g, const char* what) { return g.refuse(KY_E_NOT_FOUND, std::string(what) + ": the creature wears no body"); }

// ---- the API ----

KyubuResult KYUBU_CALL k_body_define(KyubuModHandle self, const KyubuBodyDesc* desc, KyubuBody* out, char* why, uint32_t why_cap) {
    clear_refusal();
    auto say = [&](const std::string& s) {
        if (!why || !why_cap) return;
        const size_t n = std::min<size_t>(s.size(), why_cap - 1);
        std::memcpy(why, s.data(), n);
        why[n] = 0;
    };
    say("");
    if (out) *out = 0;
    KyubuSide side = KY_SIDE_NONE;
    Mod* mod = ModManager::get().from_handle(self, &side);
    if (!mod || (side != KY_SIDE_CLIENT && side != KY_SIDE_SERVER)) return KY_E_INVALID_ARG;
    if (mod->state == ModState::Faulted) return KY_E_FAULTED;
    const char* fn = "body_define";
    std::string unavailable;
    if (!available(Prim::creature_body, &unavailable)) {
        say(unavailable);
        return note_refusal(mod, side, fn, KY_E_UNAVAILABLE, "creature bodies are unavailable on this build: " + unavailable);
    }
    if (!desc || !out || desc->size < offsetof(KyubuBodyDesc, clips) + sizeof(void*) || !desc->name || !desc->rig_text ||
        (desc->piece_count && !desc->pieces) || (desc->clip_count && !desc->clips)) {
        say("desc, out, name or rig_text is NULL, or desc->size is too small");
        return note_refusal(mod, side, fn, KY_E_INVALID_ARG, "desc, out, name or rig_text is NULL, or desc->size is too small");
    }
    std::vector<body::PieceSpec> pieces;
    for (uint32_t i = 0; i < desc->piece_count; ++i) {
        const KyubuBodyPiece& p = desc->pieces[i];
        if (!p.node) return note_refusal(mod, side, fn, KY_E_INVALID_ARG, "a piece has no node name");
        body::PieceSpec ps;
        ps.node = p.node;
        ps.slot = p.model_slot;
        void* model = Models::get().model_at(p.model_slot);
        if (!model) {
            const std::string m = std::string("piece '") + p.node + "': model slot " + std::to_string(p.model_slot) + " is not loaded";
            say(m);
            return note_refusal(mod, side, fn, KY_E_INVALID_ARG, m);
        }
        for (int k = 0; k < 3; ++k) ps.size[k] = rd<int32_t>((uintptr_t)model + kModelSize + 4 * k);
        pieces.push_back(ps);
    }
    std::vector<body::ClipSpec> clips;
    for (uint32_t i = 0; i < desc->clip_count; ++i) {
        if (!desc->clips[i].name || !desc->clips[i].text) return note_refusal(mod, side, fn, KY_E_INVALID_ARG, "a clip has no name or text");
        clips.push_back({desc->clips[i].name, desc->clips[i].text});
    }
    body::DefineResult r = body::define(desc->name, desc->rig_text, clips, pieces);
    if (!r.def) {
        say(r.error);
        return note_refusal(mod, side, fn, KY_E_INVALID_ARG, r.error);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    const uint32_t id = g_next_def++;
    g_defs[id] = r.def;
    *out = id;
    return KY_OK;
}

KyubuResult KYUBU_CALL k_body_name(KyubuModHandle self, KyubuBody b, uint32_t kind, uint32_t index, char* buf, uint32_t cap) {
    clear_refusal();
    KyubuSide side = KY_SIDE_NONE;
    Mod* mod = ModManager::get().from_handle(self, &side);
    if (!mod) return KY_E_INVALID_ARG;
    if (!buf || !cap || kind > 1) return KY_E_INVALID_ARG;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_defs.find(b);
    if (it == g_defs.end()) return KY_E_NOT_FOUND;
    const body::BodyDef& d = *it->second;
    const std::string* name = nullptr;
    if (kind == 0 && index < d.pieces.size()) name = &d.pieces[index].name;
    if (kind == 1 && index < d.clips.size()) name = &d.clips[index].name;
    if (!name) return KY_E_NOT_FOUND;
    if (name->size() + 1 > cap) return KY_E_BUFFER;
    std::memcpy(buf, name->c_str(), name->size() + 1);
    return KY_OK;
}

KyubuResult KYUBU_CALL k_attach(KyubuModHandle self, KyubuCreature c, KyubuBody b, float scale) {
    GameCall g(self, Prim::creature_body, "creature_body_attach");
    if (!g) return g.result();
    cube_Creature* cr = g.creature(c);
    if (!cr) return g.result();
    std::lock_guard<std::mutex> lock(g_mu);
    if (b == 0) {
        g_att.erase(cr->id);
        g_count_hint = g_att.size();
        return KY_OK;
    }
    auto it = g_defs.find(b);
    if (it == g_defs.end()) return g.refuse(KY_E_NOT_FOUND, "no body " + std::to_string(b));
    if (!(scale > 0) || !std::isfinite(scale)) return g.refuse(KY_E_INVALID_ARG, "scale must be above 0");
    Attached a;
    a.inst.def = it->second;
    a.inst.scale = scale;
    a.mats.assign(it->second->pieces.size(), {});
    g_att[cr->id] = std::move(a);
    g_count_hint = g_att.size();
    return KY_OK;
}

KyubuResult KYUBU_CALL k_play(KyubuModHandle self, KyubuCreature c, const char* clip, float start_ms, uint32_t flags, float fade_ms) {
    GameCall g(self, Prim::creature_body, "creature_body_play");
    if (!g) return g.result();
    cube_Creature* cr = g.creature(c);
    if (!cr) return g.result();
    if (!clip) return g.refuse(KY_E_INVALID_ARG, "clip is NULL");
    if (!std::isfinite(start_ms) || !std::isfinite(fade_ms) || fade_ms < 0)
        return g.refuse(KY_E_INVALID_ARG, "start_ms or fade_ms is not a finite number, or fade_ms is below 0");
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_att.find(cr->id);
    if (it == g_att.end()) return not_attached(g, "creature_body_play");
    const int idx = it->second.inst.def->clip_index(clip);
    if (idx < 0) return g.refuse(KY_E_NOT_FOUND, std::string("the body has no clip '") + clip + "'");
    it->second.inst.play(idx, start_ms, (flags & KY_BODY_LOOP) != 0, fade_ms);
    return KY_OK;
}

KyubuResult KYUBU_CALL k_offset(KyubuModHandle self, KyubuCreature c, uint32_t part, uint32_t channel, const float* v, uint32_t group) {
    GameCall g(self, Prim::creature_body, "creature_body_offset");
    if (!g) return g.result();
    cube_Creature* cr = g.creature(c);
    if (!cr) return g.result();
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_att.find(cr->id);
    if (it == g_att.end()) return not_attached(g, "creature_body_offset");
    if (!v) {
        it->second.inst.clear_offsets();
        return KY_OK;
    }
    if (part >= (uint32_t)anim::kPartCount || channel > 1 || group >= (uint32_t)body::kMaxGroups)
        return g.refuse(KY_E_INVALID_ARG, "part must be 0 to 5, channel 0 or 1, group 0 to 7");
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(v[k])) return g.refuse(KY_E_INVALID_ARG, "v has a value that is not finite");
    it->second.inst.set_offset((anim::Part)part, channel == 0 ? anim::Channel::Pos : anim::Channel::Rot, v, (int)group);
    return KY_OK;
}

KyubuResult KYUBU_CALL k_weight(KyubuModHandle self, KyubuCreature c, uint32_t group, float weight) {
    GameCall g(self, Prim::creature_body, "creature_body_weight");
    if (!g) return g.result();
    cube_Creature* cr = g.creature(c);
    if (!cr) return g.result();
    if (group >= (uint32_t)body::kMaxGroups || !(weight >= 0) || !std::isfinite(weight))
        return g.refuse(KY_E_INVALID_ARG, "group must be 0 to 7 and weight 0 or more");
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_att.find(cr->id);
    if (it == g_att.end()) return not_attached(g, "creature_body_weight");
    it->second.inst.set_group_weight((int)group, weight);
    return KY_OK;
}

KyubuResult KYUBU_CALL k_read(KyubuModHandle self, KyubuCreature c, KyubuBodyState* state, KyubuBodyMarker* markers, uint32_t cap, uint32_t* n) {
    GameCall g(self, Prim::creature_body, "creature_body_read");
    if (!g) return g.result();
    cube_Creature* cr = g.creature(c);
    if (!cr) return g.result();
    if (n) *n = 0;
    if (markers && !cap) return g.refuse(KY_E_INVALID_ARG, "markers is given with a cap of 0");
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_att.find(cr->id);
    if (it == g_att.end()) return not_attached(g, "creature_body_read");
    Attached& a = it->second;
    if (state) {
        if (state->size < offsetof(KyubuBodyState, reserved)) return g.refuse(KY_E_INVALID_ARG, "state->size is too small");
        state->clip = a.inst.playing();
        state->time_ms = a.inst.time_ms();
        state->fade_left_ms = a.inst.fade_left_ms();
        state->flags = a.inst.looping() ? KY_BODY_LOOP : 0;
    }
    if (markers) {
        uint32_t k = 0;
        while (k < cap && k < a.pending.size()) {
            markers[k].clip = a.pending[k].clip;
            std::memset(markers[k].name, 0, sizeof markers[k].name);
            std::strncpy(markers[k].name, a.pending[k].name.c_str(), sizeof markers[k].name - 1);
            ++k;
        }
        a.pending.erase(a.pending.begin(), a.pending.begin() + k);
        if (n) *n = k;
    }
    return KY_OK;
}

KyubuResult KYUBU_CALL k_point(KyubuModHandle self, KyubuCreature c, const char* piece, const float* p, const char* clip, float t_ms, float* out) {
    GameCall g(self, Prim::creature_body, "creature_body_point");
    if (!g) return g.result();
    cube_Creature* cr = g.creature(c);
    if (!cr) return g.result();
    if (!piece || !p || !out) return g.refuse(KY_E_INVALID_ARG, "piece, p or out is NULL");
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_att.find(cr->id);
    if (it == g_att.end()) return not_attached(g, "creature_body_point");
    const body::Instance& in = it->second.inst;
    const int pi = in.def->piece_index(piece);
    if (pi < 0) return g.refuse(KY_E_NOT_FOUND, std::string("the body has no piece '") + piece + "'");
    kanimator::Solved s;
    if (clip) {
        const int ci = in.def->clip_index(clip);
        if (ci < 0) return g.refuse(KY_E_NOT_FOUND, std::string("the body has no clip '") + clip + "'");
        s = kanimator::solve_pose(in.def->rig, in.pose_of(ci, t_ms, false));
    } else {
        s = in.solve();
    }
    float pos[3], yaw;
    creature_place(cr, pos, &yaw);
    body::piece_point(*in.def, s, pi, p, pos, yaw, in.scale, out);
    return KY_OK;
}

}  // namespace

// ---- the hooks ----

void body_on_draw_site(KyubuCpuContext* ctx) { hide_parts(ctx->esi, ctx->esp); }
void body_on_draw_site_b(KyubuCpuContext* ctx) { hide_parts(ctx->ecx, ctx->esp); }

void body_on_draw_end(KyubuCpuContext* ctx) {
    if (g_count_hint == 0 || !g_colour || !g_matrices || !g_draw) return;
    const uintptr_t record = ctx->edi, frame = ctx->ebp;
    const uintptr_t creature = rd<uint32_t>(record);
    if (!creature) return;
    void* shader = reinterpret_cast<void*>(rd<uint32_t>(frame + 0x8));
    const float* view = reinterpret_cast<const float*>(rd<uint32_t>(frame + 0xC));
    const float* proj = reinterpret_cast<const float*>(rd<uint32_t>(frame + 0x10));
    const float* colour = reinterpret_cast<const float*>(rd<uint32_t>(frame + 0x1C));
    if (!shader || !view || !proj || !colour) return;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_att.find(creature_id_at(creature));
    if (it == g_att.end() || it->second.dead) return;
    Attached& a = it->second;
    const Clock::time_point now = Clock::now();
    if (a.advanced) {
        const float dt = std::chrono::duration<float, std::milli>(now - a.last_adv).count();
        if (dt >= 1.0f) {   // the passes of one frame are far closer than that
            a.inst.advance(std::min(dt, 100.0f), &a.pending);
            a.last_adv = now;
        }
    } else {
        a.advanced = true;
        a.last_adv = now;
    }
    if (a.inst.playing() < 0) return;   // nothing played yet: draw nothing (the parts are hidden)
    float pos[3], yaw;
    creature_place(reinterpret_cast<const cube_Creature*>(creature), pos, &yaw);
    const kanimator::Solved s = a.inst.solve();
    const body::BodyDef& d = *a.inst.def;
    g_colour(shader, nullptr, colour);
    for (size_t i = 0; i < d.pieces.size(); ++i) {
        body::piece_matrix(d, s, (int)i, pos, yaw, a.inst.scale, a.mats[i].data());
        void* model = Models::get().model_at(d.pieces[i].slot);
        if (!model) continue;
        g_matrices(shader, nullptr, a.mats[i].data(), view, proj);
        g_draw(model, nullptr);
    }
    if (++a.drawn == 1 || a.drawn == 300)
        KINFO("creature body: draw %d of creature %llu at render pos %.3f %.3f %.3f yaw %.3f; piece 0 (%s) matrix t %.3f %.3f %.3f", a.drawn,
              (unsigned long long)creature_id_at(creature), pos[0], pos[1], pos[2], yaw, d.pieces.empty() ? "-" : d.pieces[0].name.c_str(),
              a.mats[0][12], a.mats[0][13], a.mats[0][14]);
}

void body_on_death_site(KyubuCpuContext* ctx) {
    if (g_count_hint == 0 || !g_burst) return;
    const uintptr_t creature = ctx->esi;
    void* controller = reinterpret_cast<void*>(ctx->edi);
    if (!creature || !rd<uint8_t>(creature + kDrawFlag)) return;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_att.find(creature_id_at(creature));
    if (it == g_att.end() || it->second.dead) return;
    Attached& a = it->second;
    static const float zero3[3] = {0, 0, 0};
    static const float white[4] = {1, 1, 1, 1};
    const float* velocity = reinterpret_cast<const float*>(creature + offsetof(cube_Creature, data) + offsetof(cube_CreatureData, velocity));
    const body::BodyDef& d = *a.inst.def;
    for (size_t i = 0; i < d.pieces.size(); ++i) {
        void* model = Models::get().model_at(d.pieces[i].slot);
        bool finite = true;
        for (float f : a.mats[i]) finite = finite && std::isfinite(f);
        if (!model || !finite || a.mats[i][15] != 1.0f) continue;   // a piece that was never drawn has no matrix
        g_burst(controller, nullptr, model, a.mats[i].data(), zero3, velocity, 1.0f, 0.0f, 0.0f, white, 0);
    }
    wr<uint8_t>(creature + kDrawFlag, 0);   // the game skips its own burst of the hidden parts
    a.dead = true;
    KINFO("creature body: burst %u pieces of creature %llu", (unsigned)d.pieces.size(), (unsigned long long)creature_id_at(creature));
}

void creature_body_drop(uint64_t id) {
    if (g_count_hint == 0) return;
    std::lock_guard<std::mutex> lock(g_mu);
    g_att.erase(id);
    g_count_hint = g_att.size();
}

void creature_body_world_exit() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_att.clear();
    g_count_hint = 0;
}

bool creature_has_body(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_att.count(id) != 0;
}

void install_creature_body_hooks() {
    if (g_hooks[0]) return;
    std::string why;
    if (!available(Prim::creature_body, &why)) {
        KINFO("creature body: bodies are off: %s", why.c_str());
        return;
    }
    auto& st = SymbolTable::get();
    g_colour = reinterpret_cast<SetColourFn>(verified_symbol("cube::CubeShader::setColour"));
    g_matrices = reinterpret_cast<SetMatricesFn>(verified_symbol("cube::CubeShader::setMatrices"));
    g_draw = reinterpret_cast<DrawFn>(verified_symbol("Model::draw"));
    g_burst = reinterpret_cast<BurstFn>(verified_symbol("cube::GameController::createParticles"));
    if (!g_colour || !g_matrices || !g_draw || !g_burst) {
        KERROR("creature body: bodies are off: a draw function is missing (setColour %p setMatrices %p draw %p createParticles %p)", (void*)g_colour,
               (void*)g_matrices, (void*)g_draw, (void*)g_burst);
        return;
    }
    struct Site {
        const char* name;
        void (*fn)(KyubuCpuContext*);
        bool required;
    };
    const Site sites[] = {{"creature_draw_site", body_on_draw_site, true},
                          {"draw_creature_end_site", body_on_draw_end, true},
                          {"creature_death_burst_site", body_on_death_site, true},
                          {"creature_draw_site_b", body_on_draw_site_b, false},   // candidates (read from the code): hooked when they resolve
                          {"creature_draw_site_c", body_on_draw_site_b, false}};
    int n = 0;
    for (const Site& s : sites) {
        const uintptr_t a = s.required ? verified_symbol(s.name, &why) : st.addr(s.name);
        if (!a) {
            if (s.required) {
                KERROR("creature body: bodies are off: no site %s: %s", s.name, why.c_str());
                remove_creature_body_hooks();
                return;
            }
            continue;
        }
        if (Hooks::get().hook_mid("kyubu", a, KY_SIDE_CLIENT, s.fn, &g_hooks[n], &why) != KY_OK) {
            KERROR("creature body: cannot hook %s: %s", s.name, why.c_str());
            if (s.required) {
                remove_creature_body_hooks();
                return;
            }
            continue;
        }
        ++n;
    }
    KINFO("creature body: installed at %d sites", n);
}

void remove_creature_body_hooks() {
    for (KyubuId& id : g_hooks) {
        if (id) Hooks::get().remove("kyubu", id);
        id = 0;
    }
}

void fill_creature_body(KyubuApi& a) {
    a.body_define = k_body_define;
    a.body_name = k_body_name;
    a.creature_body_attach = k_attach;
    a.creature_body_play = k_play;
    a.creature_body_offset = k_offset;
    a.creature_body_weight = k_weight;
    a.creature_body_read = k_read;
    a.creature_body_point = k_point;
}

}  // namespace kyubu::game
