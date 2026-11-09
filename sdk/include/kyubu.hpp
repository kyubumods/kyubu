// kyubu.hpp - header-only C++20 convenience layer over kyubu_api.h.
// Nothing here crosses the ABI: it only wraps the C function table.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kyubu_api.h"

namespace kyubu {

class Mod {
public:
    Mod(const KyubuApi* api, KyubuModHandle self, KyubuSide side) : api_(api), self_(self), side_(side) {}

    const KyubuApi* api() const { return api_; }
    KyubuModHandle handle() const { return self_; }
    KyubuSide side() const { return side_; }

    // ---- logging
    template <typename... A>
    void log(KyubuLogLevel l, const char* f, A... a) const {
        char buf[1024];
        std::snprintf(buf, sizeof buf, f, a...);
        api_->log(self_, l, buf);
    }
    template <typename... A> void info(const char* f, A... a) const { log(KY_LOG_INFO, f, a...); }
    template <typename... A> void warn(const char* f, A... a) const { log(KY_LOG_WARN, f, a...); }
    template <typename... A> void error(const char* f, A... a) const { log(KY_LOG_ERROR, f, a...); }

    // ---- events (the std::function is owned by the Mod object and must outlive the subscription)
    KyubuId on(const char* event, std::function<void(KyubuEvent&)> fn, int32_t priority = 0) {
        auto holder = std::make_unique<std::function<void(KyubuEvent&)>>(std::move(fn));
        KyubuId id = 0;
        auto tramp = [](KyubuEvent* ev, void* user) { (*static_cast<std::function<void(KyubuEvent&)>*>(user))(*ev); };
        if (api_->subscribe(self_, event, KY_SIDE_NONE, priority, tramp, holder.get(), &id) == KY_OK)
            handlers_.push_back(std::move(holder));
        return id;
    }
    template <typename T> static T* data(KyubuEvent& ev) { return static_cast<T*>(ev.data); }
    void cancel(KyubuEvent& ev) const { api_->event_cancel(&ev); }

    // ---- symbols & hooks
    uintptr_t symbol(const char* name) const {
        uintptr_t a = 0;
        return api_->resolve_symbol(name, &a) == KY_OK ? a : 0;
    }
    // Typed function hook: Fn is the exact function pointer type of the target (incl. convention).
    template <typename Fn>
    KyubuId hook(const char* symbol_name, Fn handler, Fn* next, int32_t priority = 0) {
        KyubuId id = 0;
        api_->hook_symbol(self_, symbol_name, reinterpret_cast<void*>(handler), priority, reinterpret_cast<void**>(next), &id);
        return id;
    }
    template <typename Fn>
    KyubuId hook(uintptr_t address, Fn handler, Fn* next, int32_t priority = 0) {
        KyubuId id = 0;
        api_->hook_function(self_, address, reinterpret_cast<void*>(handler), priority, reinterpret_cast<void**>(next), &id);
        return id;
    }
    KyubuId patch(uintptr_t address, std::span<const uint8_t> bytes) {
        KyubuId id = 0;
        api_->patch_bytes(self_, address, bytes.data(), (uint32_t)bytes.size(), &id);
        return id;
    }
    void unhook(KyubuId id) { api_->unhook(self_, id); }

    template <typename T>
    bool read(uintptr_t addr, T& out) const { return api_->mem_read(addr, &out, sizeof(T)) == KY_OK; }
    template <typename T>
    bool write(uintptr_t addr, const T& v) { return api_->mem_write(self_, addr, &v, sizeof(T)) == KY_OK; }

    // ---- registries
    KyubuResult add_model(std::string_view id, std::string_view key) {
        std::string i(id), k(key);
        return api_->registry_add(self_, "kyubu:models", i.c_str(), k.data(), (uint32_t)k.size(), nullptr);
    }
    KyubuResult remap_model(std::string_view vanilla_id, std::string_view key) {
        std::string i(vanilla_id), k(key);
        return api_->registry_remap(self_, "kyubu:models", i.c_str(), k.data(), (uint32_t)k.size());
    }
    uint32_t model_slot(const char* id) const {
        uint32_t n = 0xFFFFFFFFu;
        api_->registry_lookup("kyubu:models", id, &n);
        return n;
    }

    // ---- channels
    KyubuResult send(const char* channel, uint32_t connection, std::span<const uint8_t> data) {
        return api_->channel_send(self_, channel, connection, data.data(), (uint32_t)data.size());
    }

    // ---- game API (docs/game-api.md; API 1.1). Whether a primitive ("world", "handles", ...) is available on this
    // build; `why` gets the reason when it is not (or when the loader is older than 1.1).
    bool has_game() const { return has_member(offsetof(KyubuApi, game_available)); }
    bool game_available(const char* primitive, std::string* why = nullptr) const {
        char buf[512] = {};
        if (!has_game()) {
            if (why) *why = "the loader is older than API 1.1";
            return false;
        }
        KyubuResult r = api_->game_available(primitive, buf, sizeof buf);
        if (r != KY_OK && why) *why = buf;
        return r == KY_OK;
    }
    // The values of a verified mapped enum ("cube::EntityType") in value order; empty, with the reason in `why`, for
    // an unknown or candidate enum.
    struct EnumValue {
        int64_t value;
        std::string name;
    };
    std::vector<EnumValue> game_enum(const char* name, std::string* why = nullptr) const {
        std::vector<EnumValue> out;
        if (!has_member(offsetof(KyubuApi, game_enum))) {
            if (why) *why = "the loader is older than API 1.1";
            return out;
        }
        char buf[512] = {};
        int64_t value = 0;
        for (uint32_t i = 0;; ++i) {
            KyubuResult r = api_->game_enum(name, i, &value, buf, sizeof buf);
            if (r == KY_E_NOT_FOUND && i > 0) return out;   // past the last value
            if (r != KY_OK) {
                if (why) *why = buf[0] ? std::string(buf) : "game_enum failed (result " + std::to_string(r) + ")";
                return {};
            }
            out.push_back({value, buf});
        }
    }
    // The creature functions return the C member's result, or KY_E_UNAVAILABLE when the loader is older than 1.1.
    // A creature's verified state; sets out.size.
    KyubuResult creature_get(KyubuCreature c, KyubuCreatureInfo& out) const {
        if (!has_member(offsetof(KyubuApi, creature_get))) return KY_E_UNAVAILABLE;
        out.size = sizeof out;
        return api_->creature_get(self_, c, &out);
    }
    // The client side's own player (client side only).
    KyubuResult creature_player(KyubuCreature& out) const {
        out = 0;
        if (!has_member(offsetof(KyubuApi, creature_player))) return KY_E_UNAVAILABLE;
        return api_->creature_player(self_, &out);
    }
    // A handle, on this side, of the creature with a game id (creature_get's `id`).
    KyubuResult creature_find(int64_t id, KyubuCreature& out) const {
        out = 0;
        if (!has_member(offsetof(KyubuApi, creature_find))) return KY_E_UNAVAILABLE;
        return api_->creature_find(self_, id, &out);
    }
    // Moves a creature this side owns to pos (x, y, z in units, 65,536 to a block) and stops it.
    KyubuResult creature_teleport(KyubuCreature c, const int64_t pos[3]) const {
        if (!has_member(offsetof(KyubuApi, creature_teleport))) return KY_E_UNAVAILABLE;
        return api_->creature_teleport(self_, c, pos);
    }
    // Sets a creature's KyubuCreatureFlags (all at once; 0 clears); a creature this side owns, Cube.exe only for now.
    KyubuResult creature_set_flags(KyubuCreature c, uint32_t flags) const {
        if (!has_member(offsetof(KyubuApi, creature_set_flags))) return KY_E_UNAVAILABLE;
        return api_->creature_set_flags(self_, c, flags);
    }
    // Creature bodies (1.4, docs/game-api.md "Creature bodies"): pieces on a rig drawn in place of a creature's own parts.
    KyubuResult body_define(const KyubuBodyDesc& desc, KyubuBody* out, char* why = nullptr, uint32_t why_cap = 0) const {
        if (!has_member(offsetof(KyubuApi, body_define))) return KY_E_UNAVAILABLE;
        KyubuBodyDesc d = desc;
        d.size = sizeof d;
        return api_->body_define(self_, &d, out, why, why_cap);
    }
    KyubuResult body_name(KyubuBody body, uint32_t kind, uint32_t index, char* buf, uint32_t cap) const {
        if (!has_member(offsetof(KyubuApi, body_name))) return KY_E_UNAVAILABLE;
        return api_->body_name(self_, body, kind, index, buf, cap);
    }
    KyubuResult creature_body_attach(KyubuCreature c, KyubuBody body, float scale) const {
        if (!has_member(offsetof(KyubuApi, creature_body_attach))) return KY_E_UNAVAILABLE;
        return api_->creature_body_attach(self_, c, body, scale);
    }
    KyubuResult creature_body_play(KyubuCreature c, const char* clip, float start_ms = 0, uint32_t flags = 0, float fade_ms = 0) const {
        if (!has_member(offsetof(KyubuApi, creature_body_play))) return KY_E_UNAVAILABLE;
        return api_->creature_body_play(self_, c, clip, start_ms, flags, fade_ms);
    }
    KyubuResult creature_body_offset(KyubuCreature c, uint32_t part, uint32_t channel, const float* v, uint32_t group = 0) const {
        if (!has_member(offsetof(KyubuApi, creature_body_offset))) return KY_E_UNAVAILABLE;
        return api_->creature_body_offset(self_, c, part, channel, v, group);
    }
    KyubuResult creature_body_weight(KyubuCreature c, uint32_t group, float weight) const {
        if (!has_member(offsetof(KyubuApi, creature_body_weight))) return KY_E_UNAVAILABLE;
        return api_->creature_body_weight(self_, c, group, weight);
    }
    KyubuResult creature_body_read(KyubuCreature c, KyubuBodyState* state, KyubuBodyMarker* markers, uint32_t cap, uint32_t* n) const {
        if (!has_member(offsetof(KyubuApi, creature_body_read))) return KY_E_UNAVAILABLE;
        if (state) state->size = sizeof *state;
        return api_->creature_body_read(self_, c, state, markers, cap, n);
    }
    KyubuResult creature_body_point(KyubuCreature c, const char* piece, const float* p, const char* clip, float t_ms, float* out) const {
        if (!has_member(offsetof(KyubuApi, creature_body_point))) return KY_E_UNAVAILABLE;
        return api_->creature_body_point(self_, c, piece, p, clip, t_ms, out);
    }
    KyubuResult creature_set_yaw(KyubuCreature c, float yaw) const {
        if (!has_member(offsetof(KyubuApi, creature_set_yaw))) return KY_E_UNAVAILABLE;
        return api_->creature_set_yaw(self_, c, yaw);
    }
    // Puts an item into a creature's bags (client side: the local player's); sets item.size.
    KyubuResult creature_give_item(KyubuCreature c, KyubuItem item) const {
        if (!has_member(offsetof(KyubuApi, creature_give_item))) return KY_E_UNAVAILABLE;
        item.size = sizeof item;
        return api_->creature_give_item(self_, c, &item);
    }
    // One of the game's own particle effects (client side; docs/game-api.md, particle_spawn); sets p.size.
    KyubuResult particle_spawn(KyubuParticle p) const {
        if (!has_member(offsetof(KyubuApi, particle_spawn))) return KY_E_UNAVAILABLE;
        p.size = sizeof p;
        return api_->particle_spawn(self_, &p);
    }
    // A point light of the mod (client side; docs/game-api.md, light_spawn); sets l.size.
    KyubuResult light_spawn(KyubuLight l, uint32_t* id) const {
        if (!has_member(offsetof(KyubuApi, light_spawn))) return KY_E_UNAVAILABLE;
        l.size = sizeof l;
        return api_->light_spawn(self_, &l, id);
    }
    KyubuResult light_set(uint32_t id, KyubuLight l) const {
        if (!has_member(offsetof(KyubuApi, light_set))) return KY_E_UNAVAILABLE;
        l.size = sizeof l;
        return api_->light_set(self_, id, &l);
    }
    KyubuResult light_remove(uint32_t id) const {
        if (!has_member(offsetof(KyubuApi, light_remove))) return KY_E_UNAVAILABLE;
        return api_->light_remove(self_, id);
    }
    // Builds a creature the game's way (server side only); sets spawn.size.
    KyubuResult creature_spawn(KyubuSpawn spawn, KyubuCreature& out) {
        out = 0;
        if (!has_member(offsetof(KyubuApi, creature_spawn))) return KY_E_UNAVAILABLE;
        spawn.size = sizeof spawn;
        return api_->creature_spawn(self_, &spawn, &out);
    }

    // ---- abilities (docs/abilities.md; API 1.1). Each returns the C member's result, or KY_E_UNAVAILABLE when the
    // loader is older than the member. Ids come back in a KY_ABILITY_ID_MAX buffer, which holds any id.
    // Register phase only; sets def.size.
    KyubuResult ability_define(const char* id, KyubuAbilityDef def) {
        if (!has_member(offsetof(KyubuApi, ability_define))) return KY_E_UNAVAILABLE;
        def.size = sizeof def;
        return api_->ability_define(self_, id, &def);
    }
    KyubuResult ability_number(const char* id, uint32_t& out) const {
        out = 0;
        if (!has_member(offsetof(KyubuApi, ability_number))) return KY_E_UNAVAILABLE;
        return api_->ability_number(id, &out);
    }
    KyubuResult ability_name(uint32_t number, std::string& out) const {
        out.clear();
        if (!has_member(offsetof(KyubuApi, ability_name))) return KY_E_UNAVAILABLE;
        char buf[KY_ABILITY_ID_MAX] = {};
        KyubuResult r = api_->ability_name(number, buf, sizeof buf);
        if (r == KY_OK) out = buf;
        return r;
    }
    // The ability at `index` in number order; `count` (may be NULL) gets how many there are.
    KyubuResult ability_list(uint32_t index, std::string& id, uint32_t& number, uint32_t* count = nullptr) const {
        id.clear();
        number = 0;
        if (!has_member(offsetof(KyubuApi, ability_list))) return KY_E_UNAVAILABLE;
        char buf[KY_ABILITY_ID_MAX] = {};
        KyubuResult r = api_->ability_list(index, buf, sizeof buf, &number, count);
        if (r == KY_OK) id = buf;
        return r;
    }
    // The ability that holds the fourth slot of a class (cube::ClassType 1 to 4) and specialization (0 or 1).
    KyubuResult ability_slot_owner(int32_t class_type, int32_t specialization, std::string& id, uint32_t& number) const {
        id.clear();
        number = 0;
        if (!has_member(offsetof(KyubuApi, ability_slot_owner))) return KY_E_UNAVAILABLE;
        char buf[KY_ABILITY_ID_MAX] = {};
        KyubuResult r = api_->ability_slot_owner(class_type, specialization, buf, sizeof buf, &number);
        if (r == KY_OK) id = buf;
        return r;
    }
    // The ability a creature is in; sets out.size.
    KyubuResult ability_get(KyubuCreature c, KyubuAbilityInfo& out) const {
        if (!has_member(offsetof(KyubuApi, ability_get))) return KY_E_UNAVAILABLE;
        out.size = sizeof out;
        return api_->ability_get(self_, c, &out);
    }
    // The milliseconds of cooldown the creature has left for the ability `number`.
    KyubuResult ability_cooldown(KyubuCreature c, uint32_t number, uint32_t& out_ms) const {
        out_ms = 0;
        if (!has_member(offsetof(KyubuApi, ability_cooldown))) return KY_E_UNAVAILABLE;
        return api_->ability_cooldown(self_, c, number, &out_ms);
    }
    // Sets the cooldown left (0 gives it back); on Server.exe also when the server admits the ability again.
    KyubuResult ability_cooldown_set(KyubuCreature c, uint32_t number, uint32_t ms) const {
        if (!has_member(offsetof(KyubuApi, ability_cooldown_set))) return KY_E_UNAVAILABLE;
        return api_->ability_cooldown_set(self_, c, number, ms);
    }
    // The block at block x, y, z of this side's World; KY_E_NOT_FOUND for a place in no loaded zone ("unknown").
    KyubuResult world_block(int32_t x, int32_t y, int32_t z, KyubuBlock* out) const {
        if (!has_member(offsetof(KyubuApi, world_block))) return KY_E_UNAVAILABLE;
        return api_->world_block(self_, x, y, z, out);
    }
    KyubuResult creature_hit(KyubuCreature attacker, KyubuCreature target, const KyubuHit* hit, int* landed = nullptr) const {
        if (!has_member(offsetof(KyubuApi, creature_hit))) return KY_E_UNAVAILABLE;
        return api_->creature_hit(self_, attacker, target, hit, landed);
    }
    // A creature's movement and combat state; set writes the parts `in.fields` names (a creature this side owns).
    KyubuResult creature_state_get(KyubuCreature c, KyubuCreatureState* out) const {
        if (!has_member(offsetof(KyubuApi, creature_state_get))) return KY_E_UNAVAILABLE;
        return api_->creature_state_get(self_, c, out);
    }
    KyubuResult creature_state_set(KyubuCreature c, const KyubuCreatureState* in) const {
        if (!has_member(offsetof(KyubuApi, creature_state_set))) return KY_E_UNAVAILABLE;
        return api_->creature_state_set(self_, c, in);
    }
    // Server.exe: the projectile packets joined clients sent since the last take, and a projectile pushed into the World's list.
    KyubuResult shot_take(KyubuShot* out, uint32_t max, uint32_t* count) const {
        if (!has_member(offsetof(KyubuApi, shot_take))) return KY_E_UNAVAILABLE;
        return api_->shot_take(self_, out, max, count);
    }
    KyubuResult projectile_push(const uint8_t* data, uint32_t len) const {
        if (!has_member(offsetof(KyubuApi, projectile_push))) return KY_E_UNAVAILABLE;
        return api_->projectile_push(self_, data, len);
    }
    // Starts / ends a **mod** ability on a creature this side owns.
    KyubuResult ability_start(KyubuCreature c, uint32_t number) const {
        if (!has_member(offsetof(KyubuApi, ability_start))) return KY_E_UNAVAILABLE;
        return api_->ability_start(self_, c, number);
    }
    KyubuResult ability_end(KyubuCreature c) const {
        if (!has_member(offsetof(KyubuApi, ability_end))) return KY_E_UNAVAILABLE;
        return api_->ability_end(self_, c);
    }
    // The local player's rank in a mod ability (client side only), and setting it (clamped to the ability's max_rank).
    KyubuResult ability_rank_get(uint32_t number, uint32_t& out_rank) const {
        out_rank = 0;
        if (!has_member(offsetof(KyubuApi, ability_rank_get))) return KY_E_UNAVAILABLE;
        return api_->ability_rank_get(self_, number, &out_rank);
    }
    KyubuResult ability_rank_set(uint32_t number, uint32_t rank, uint32_t& out_rank) {
        out_rank = 0;
        if (!has_member(offsetof(KyubuApi, ability_rank_set))) return KY_E_UNAVAILABLE;
        return api_->ability_rank_set(self_, number, rank, &out_rank);
    }

    // ---- the animation editor's calls (client side, full trust, dev mode; docs/game-api.md). `err` gets the errors of a bad clip. ----
    KyubuResult animation_define_from_text(const char* name, const char* text, char* err = nullptr, uint32_t err_cap = 0) const {
        if (!has_member(offsetof(KyubuApi, animation_define_from_text))) return KY_E_UNAVAILABLE;
        return api_->animation_define_from_text(self_, name, text, err, err_cap);
    }
    KyubuResult animation_preview(const char* clip_id, double t_ms) const {
        if (!has_member(offsetof(KyubuApi, animation_preview))) return KY_E_UNAVAILABLE;
        return api_->animation_preview(self_, clip_id, t_ms);
    }
    KyubuResult animation_preview_clear() const {
        if (!has_member(offsetof(KyubuApi, animation_preview_clear))) return KY_E_UNAVAILABLE;
        return api_->animation_preview_clear(self_);
    }

    // ---- the animation binding (any trust level, either side; docs/game-api.md) ----
    KyubuResult ability_animation_bind(const char* ability_id, const char* clip_id) const {
        if (!has_member(offsetof(KyubuApi, ability_animation_bind))) return KY_E_UNAVAILABLE;
        return api_->ability_animation_bind(self_, ability_id, clip_id);
    }
    // One creature's clip (its game id); clip_id NULL drops it.
    KyubuResult ability_animation_bind_for(const char* ability_id, uint64_t creature_id, const char* clip_id) const {
        if (!has_member(offsetof(KyubuApi, ability_animation_bind_for))) return KY_E_UNAVAILABLE;
        return api_->ability_animation_bind_for(self_, ability_id, creature_id, clip_id);
    }

    // ---- the UI scene tree and the render device (client side, main thread; docs/game-api.md) ----
    KyubuResult ui_node_find(const char* name, uint32_t nth, KyubuUiNode* out) const {
        if (!has_member(offsetof(KyubuApi, ui_node_find))) return KY_E_UNAVAILABLE;
        return api_->ui_node_find(self_, name, nth, out);
    }
    KyubuResult ui_node_get(uintptr_t node, KyubuUiNode* out) const {
        if (!has_member(offsetof(KyubuApi, ui_node_get))) return KY_E_UNAVAILABLE;
        return api_->ui_node_get(self_, node, out);
    }
    KyubuResult ui_node_child(uintptr_t node, uint32_t index, KyubuUiNode* out) const {
        if (!has_member(offsetof(KyubuApi, ui_node_child))) return KY_E_UNAVAILABLE;
        return api_->ui_node_child(self_, node, index, out);
    }
    KyubuResult render_get(KyubuRenderData* out) const {
        if (!has_member(offsetof(KyubuApi, render_get))) return KY_E_UNAVAILABLE;
        return api_->render_get(self_, out);
    }

    // ---- mod management (docs/mods.md, "Managing mods"; API 1.2). Needs full trust and manage_mods = true in
    // mod.toml: a denied call returns KY_E_DENIED (mods_count and mods_queue_count return 0). Each returns the C
    // member's result, or KY_E_UNAVAILABLE when the loader is older than 1.2.
    bool has_mod_management() const { return has_member(offsetof(KyubuApi, game_restart)); }
    uint32_t mods_count() const {
        return has_member(offsetof(KyubuApi, mods_count)) ? api_->mods_count(self_) : 0;
    }
    // The `index`th package of mods\; sets out.size.
    KyubuResult mods_get(uint32_t index, KyubuModInfo& out) const {
        if (!has_member(offsetof(KyubuApi, mods_get))) return KY_E_UNAVAILABLE;
        out.size = sizeof out;
        return api_->mods_get(self_, index, &out);
    }
    // SHA-256 of the package of `id` (the rule of docs/mods.md).
    KyubuResult mods_package_hash(const char* id, uint8_t (&out)[32]) const {
        if (!has_member(offsetof(KyubuApi, mods_package_hash))) return KY_E_UNAVAILABLE;
        return api_->mods_package_hash(self_, id, out);
    }
    uint32_t mods_option_count(const char* id) const {
        return has_member(offsetof(KyubuApi, mods_option_count)) ? api_->mods_option_count(self_, id) : 0;
    }
    KyubuResult mods_option_get(const char* id, uint32_t index, KyubuOptionInfo& out) const {
        if (!has_member(offsetof(KyubuApi, mods_option_get))) return KY_E_UNAVAILABLE;
        return api_->mods_option_get(self_, id, index, &out);
    }
    // Checks a value given as text without saving it; mods_option_set saves it (now, no restart).
    KyubuResult mods_option_check(const char* id, const char* key, const char* value) const {
        if (!has_member(offsetof(KyubuApi, mods_option_check))) return KY_E_UNAVAILABLE;
        return api_->mods_option_check(self_, id, key, value);
    }
    KyubuResult mods_option_set(const char* id, const char* key, const char* value) {
        if (!has_member(offsetof(KyubuApi, mods_option_set))) return KY_E_UNAVAILABLE;
        return api_->mods_option_set(self_, id, key, value);
    }
    // Rereads mods\ so that packages added or removed by hand show in mods_get as `new` or `missing`.
    KyubuResult mods_rescan() {
        if (!has_member(offsetof(KyubuApi, mods_rescan))) return KY_E_UNAVAILABLE;
        return api_->mods_rescan(self_);
    }
    // The queue that the loader applies at the next start (kyubu\pending.toml).
    KyubuResult mods_queue(const KyubuModOp& op) {
        if (!has_member(offsetof(KyubuApi, mods_queue))) return KY_E_UNAVAILABLE;
        return api_->mods_queue(self_, &op);
    }
    KyubuResult mods_queue_clear() {
        if (!has_member(offsetof(KyubuApi, mods_queue_clear))) return KY_E_UNAVAILABLE;
        return api_->mods_queue_clear(self_);
    }
    uint32_t mods_queue_count() const {
        return has_member(offsetof(KyubuApi, mods_queue_count)) ? api_->mods_queue_count(self_) : 0;
    }
    KyubuResult mods_queue_get(uint32_t index, KyubuModOp& out) const {
        if (!has_member(offsetof(KyubuApi, mods_queue_get))) return KY_E_UNAVAILABLE;
        return api_->mods_queue_get(self_, index, &out);
    }
    // Starts the game again and ends this process the normal way (client, start menu only).
    KyubuResult game_restart() {
        if (!has_member(offsetof(KyubuApi, game_restart))) return KY_E_UNAVAILABLE;
        return api_->game_restart(self_);
    }

    // ---- options (docs/config.md; API 1.1). The getters return `fallback` when the option is missing, has
    // another type, or the loader is older than 1.1.
    bool has_config() const { return api_->size >= offsetof(KyubuApi, config_set) + sizeof(void*); }
    bool config_bool(const char* key, bool fallback = false) const {
        int v = 0;
        return has_config() && api_->config_get_bool(self_, key, &v) == KY_OK ? v != 0 : fallback;
    }
    int64_t config_int(const char* key, int64_t fallback = 0) const {
        int64_t v = 0;
        return has_config() && api_->config_get_int(self_, key, &v) == KY_OK ? v : fallback;
    }
    double config_float(const char* key, double fallback = 0) const {
        double v = 0;
        return has_config() && api_->config_get_float(self_, key, &v) == KY_OK ? v : fallback;
    }
    std::string config_string(const char* key, std::string fallback = {}) const {
        char buf[1024];
        return has_config() && api_->config_get_string(self_, key, buf, sizeof buf) == KY_OK ? std::string(buf) : fallback;
    }
    // Validated like the player's file, then saved to kyubu/config/<id>.toml.
    KyubuResult config_set(const char* key, const char* value) {
        return has_config() ? api_->config_set(self_, key, value) : KY_E_UNAVAILABLE;
    }
    KyubuResult config_set(const char* key, bool value) { return config_set(key, value ? "true" : "false"); }
    KyubuResult config_set(const char* key, int64_t value) { return config_set(key, std::to_string(value).c_str()); }
    KyubuResult config_set(const char* key, int value) { return config_set(key, (int64_t)value); }
    KyubuResult config_set(const char* key, double value) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.17g", value);
        return config_set(key, buf);
    }

private:
    // Whether the loader's table reaches the member at `offset` (members after 1.0's end are appended).
    bool has_member(size_t offset) const { return api_->size >= offset + sizeof(void*); }

    const KyubuApi* api_;
    KyubuModHandle self_;
    KyubuSide side_;
    std::vector<std::unique_ptr<std::function<void(KyubuEvent&)>>> handlers_;
};

}  // namespace kyubu

// Declares the three entry points and forwards init/shutdown to a user class:
//   struct MyMod { MyMod(kyubu::Mod& m); ~MyMod(); };
//   KYUBU_MOD("my_mod", KY_SIDE_CLIENT | KY_SIDE_SERVER, MyMod)
#define KYUBU_MOD(ID, SIDES, TYPE)                                                                         \
    static kyubu::Mod* g_kyubu_mods[3];                                                                    \
    static TYPE* g_kyubu_inst[3];                                                                          \
    extern "C" KYUBU_EXPORT int KYUBU_CALL kyubu_mod_query(KyubuModQuery* q) {                           \
        q->api_version = KYUBU_API_VERSION;                                                                \
        std::strncpy(q->id, ID, sizeof q->id - 1);                                                         \
        q->sides = (SIDES);                                                                                \
        return 0;                                                                                          \
    }                                                                                                      \
    extern "C" KYUBU_EXPORT int KYUBU_CALL kyubu_mod_init(const KyubuApi* api, KyubuModHandle h, KyubuSide s) { \
        g_kyubu_mods[s] = new kyubu::Mod(api, h, s);                                                       \
        g_kyubu_inst[s] = new TYPE(*g_kyubu_mods[s]);                                                      \
        return 0;                                                                                          \
    }                                                                                                      \
    extern "C" KYUBU_EXPORT void KYUBU_CALL kyubu_mod_shutdown(KyubuSide s) {                            \
        delete g_kyubu_inst[s];                                                                            \
        g_kyubu_inst[s] = nullptr;                                                                         \
    }
