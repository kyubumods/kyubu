// The entity_state primitive: reading a creature's verified state (openspec add-entity-spawner, design D4).
#include <cmath>
#include <cstddef>

#include "call.h"
#include "creature_flags.h"
#include "game.h"
#include "handles.h"
#include "log.h"

namespace kyubu::game {

namespace {

// The smallest KyubuCreatureInfo a caller can pass: `size` and `entity_type`.
constexpr uint32_t kInfoMinSize = offsetof(KyubuCreatureInfo, entity_type) + sizeof(uint32_t);

KyubuResult KYUBU_CALL k_creature_get(KyubuModHandle self, KyubuCreature c, KyubuCreatureInfo* out) {
    GameCall g(self, Prim::entity_state, "creature_get");
    if (!g) return g.result();
    if (!out || out->size < kInfoMinSize)
        return g.refuse(KY_E_INVALID_ARG, out ? fmt("out->size is %u, below %u", out->size, kInfoMinSize) : "out is NULL");
    const cube_Creature* cr = g.creature(c);
    if (!cr) return g.result();
    const cube_CreatureData& d = cr->data;
    KyubuCreatureInfo info = {};
    info.entity_type = d.entity_type;
    info.id = (int64_t)cr->id;
    info.pos[0] = d.pos.x;
    info.pos[1] = d.pos.y;
    info.pos[2] = d.pos.z;
    info.body_yaw = d.body_yaw;
    info.hp = d.hp;
    info.level = d.level;
    info.hostile_type = d.hostile_type;
    info.flags = creature_flags_of(cr->id);
    // what fits in the caller's struct, after its own `size`
    const uint32_t n = std::min<uint32_t>(out->size, sizeof info);
    memcpy((char*)out + sizeof(uint32_t), (const char*)&info + sizeof(uint32_t), n - sizeof(uint32_t));
    return KY_OK;
}

KyubuResult KYUBU_CALL k_creature_player(KyubuModHandle self, KyubuCreature* out) {
    if (out) *out = 0;
    GameCall g(self, Prim::entity_state, "creature_player");   // client-only (kSideRules)
    if (!g) return g.result();
    if (!out) return g.refuse(KY_E_INVALID_ARG, "out is NULL");
    cube_Creature* player = g.world()->local_player;
    if (!player) return g.refuse(KY_E_PHASE, "no world is loaded (the World has no local player yet)");
    const KyubuCreature h = g.handle(player);
    if (!h) return g.result();
    *out = h;
    return KY_OK;
}

// The creature with a game id, as a handle on the calling side: how the server side reaches the player, which only the
// client side has a handle of.
KyubuResult KYUBU_CALL k_creature_find(KyubuModHandle self, int64_t id, KyubuCreature* out) {
    if (out) *out = 0;
    GameCall g(self, Prim::entity_state, "creature_find");
    if (!g) return g.result();
    if (!out) return g.refuse(KY_E_INVALID_ARG, "out is NULL");
    if (!g.need(Prim::handles)) return g.result();
    cube_Creature* c = creature_by_id(g.world(), (uint64_t)id);
    if (!c) return g.refuse(KY_E_NOT_FOUND, fmt("no creature with id %lld in this side's World", (long long)id));
    const KyubuCreature h = g.handle(c);
    if (!h) return g.result();
    *out = h;
    return KY_OK;
}

// The most a coordinate may be, in units: 2^47 (about 2.1e9 blocks), far beyond the world (about 1.7e7 blocks) and exact in a Lua number.
constexpr int64_t kTeleportLimit = int64_t(1) << 47;

KyubuResult KYUBU_CALL k_creature_teleport(KyubuModHandle self, KyubuCreature c, const int64_t* pos) {
    GameCall g(self, Prim::entity_state, "creature_teleport");
    if (!g) return g.result();
    if (!pos) return g.refuse(KY_E_INVALID_ARG, "pos is NULL");
    for (int i = 0; i < 3; ++i)
        if (pos[i] > kTeleportLimit || pos[i] < -kTeleportLimit)
            return g.refuse(KY_E_INVALID_ARG, fmt("pos[%d] is %lld, beyond 2^47 units", i, (long long)pos[i]));
    cube_Creature* cr = g.creature(c);
    if (!cr || !g.owns(cr)) return g.result();
    cr->data.pos.x = pos[0];
    cr->data.pos.y = pos[1];
    cr->data.pos.z = pos[2];
    cr->data.velocity.x = 0;
    cr->data.velocity.y = 0;
    cr->data.velocity.z = 0;
    return KY_OK;
}

// Sets a creature's body yaw (degrees, counter-clockwise from north). Only the yaw: the position is creature_teleport's.
KyubuResult KYUBU_CALL k_creature_set_yaw(KyubuModHandle self, KyubuCreature c, float yaw) {
    GameCall g(self, Prim::entity_state, "creature_set_yaw");
    if (!g) return g.result();
    if (!std::isfinite(yaw)) return g.refuse(KY_E_INVALID_ARG, "yaw is not a finite number");
    cube_Creature* cr = g.creature(c);
    if (!cr || !g.owns(cr)) return g.result();
    cr->data.body_yaw = yaw;
    return KY_OK;
}

// ---- creature_state_get / creature_state_set (add-skills-unleashed-multiplayer 10.3, D12) ----

// The 10.3 struct ended at `aim`; `box` (10.7) is filled only when the caller's `size` reaches it.
constexpr uint32_t kStateMinSize = offsetof(KyubuCreatureState, box);

void put3(float* out, const cube_FloatVector3& v) { out[0] = v.x; out[1] = v.y; out[2] = v.z; }
void get3(cube_FloatVector3& v, const float* in) { v.x = in[0]; v.y = in[1]; v.z = in[2]; }
bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

KyubuResult KYUBU_CALL k_creature_state_get(KyubuModHandle self, KyubuCreature c, KyubuCreatureState* out) {
    GameCall g(self, Prim::creature_state, "creature_state_get");
    if (!g) return g.result();
    if (!out || out->size < kStateMinSize)
        return g.refuse(KY_E_INVALID_ARG, out ? fmt("out->size is %u, below %u", out->size, kStateMinSize) : "out is NULL");
    const cube_Creature* cr = g.creature(c);
    if (!cr) return g.result();
    const cube_CreatureData& d = cr->data;
    out->fields = KY_STATE_ALL;
    out->pos[0] = d.pos.x;
    out->pos[1] = d.pos.y;
    out->pos[2] = d.pos.z;
    put3(out->velocity, d.velocity);
    put3(out->knock, d.extra_vel);
    out->mode = d.current_mode;
    out->mode_ms = (int32_t)d.mode_start_time;
    out->stun_ms = d.stun_time;
    out->slowed_ms = (int32_t)d.slowed_time;
    out->charge = d.charged_mp;
    put3(out->aim, d.ray_hit);
    if (out->size >= sizeof(KyubuCreatureState)) put3(out->box, d.appearance.scale);
    return KY_OK;
}

KyubuResult KYUBU_CALL k_creature_state_set(KyubuModHandle self, KyubuCreature c, const KyubuCreatureState* in) {
    GameCall g(self, Prim::creature_state, "creature_state_set");
    if (!g) return g.result();
    if (!in || in->size < kStateMinSize)
        return g.refuse(KY_E_INVALID_ARG, in ? fmt("in->size is %u, below %u", in->size, kStateMinSize) : "in is NULL");
    const uint32_t f = in->fields;
    if (f & ~(uint32_t)KY_STATE_ALL)
        return g.refuse(KY_E_INVALID_ARG, fmt("fields 0x%x has bits that are not defined (0x%x are)", f, (unsigned)KY_STATE_ALL));
    if (f & KY_STATE_POS)
        for (int i = 0; i < 3; ++i)
            if (in->pos[i] > kTeleportLimit || in->pos[i] < -kTeleportLimit)
                return g.refuse(KY_E_INVALID_ARG, fmt("pos[%d] is %lld, beyond 2^47 units", i, (long long)in->pos[i]));
    if (((f & KY_STATE_VELOCITY) && !finite3(in->velocity)) || ((f & KY_STATE_KNOCK) && !finite3(in->knock)) ||
        ((f & KY_STATE_AIM) && !finite3(in->aim)) || ((f & KY_STATE_CHARGE) && !std::isfinite(in->charge)))
        return g.refuse(KY_E_INVALID_ARG, "a float field is not a finite number");
    if ((f & KY_STATE_MODE) && in->mode > 255) return g.refuse(KY_E_INVALID_ARG, fmt("mode %u is above 255", in->mode));
    cube_Creature* cr = g.creature(c);
    if (!cr || !g.owns(cr)) return g.result();
    cube_CreatureData& d = cr->data;
    if (f & KY_STATE_POS) {
        d.pos.x = in->pos[0];
        d.pos.y = in->pos[1];
        d.pos.z = in->pos[2];
    }
    if (f & KY_STATE_VELOCITY) get3(d.velocity, in->velocity);
    if (f & KY_STATE_KNOCK) get3(d.extra_vel, in->knock);
    if (f & KY_STATE_MODE) d.current_mode = (uint8_t)in->mode;
    if (f & KY_STATE_MODE_MS) d.mode_start_time = (uint32_t)in->mode_ms;
    if (f & KY_STATE_STUN) d.stun_time = in->stun_ms;
    if (f & KY_STATE_SLOWED) d.slowed_time = (uint32_t)in->slowed_ms;
    if (f & KY_STATE_CHARGE) d.charged_mp = in->charge;
    if (f & KY_STATE_AIM) get3(d.ray_hit, in->aim);
    return KY_OK;
}

}  // namespace

void fill_entity_state(KyubuApi& a) {
    a.creature_get = k_creature_get;
    a.creature_player = k_creature_player;
    a.creature_find = k_creature_find;
    a.creature_teleport = k_creature_teleport;
    a.creature_set_yaw = k_creature_set_yaw;
    a.creature_state_get = k_creature_state_get;
    a.creature_state_set = k_creature_state_set;
}

}  // namespace kyubu::game
