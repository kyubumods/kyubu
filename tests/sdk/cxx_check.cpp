// Compiles kyubu.hpp (C++20) and a portable thiscall hook declaration.
#include "kyubu.hpp"

using Fn = bool(__fastcall*)(void*, void*, const void*, void**, uint32_t*);
static Fn next_fn = nullptr;
static bool __fastcall hook(void* self, void* edx, const void* k, void** o, uint32_t* n) { return next_fn(self, edx, k, o, n); }

struct Check {
    explicit Check(kyubu::Mod& m) {
        m.hook("Database::get", &hook, &next_fn);
        m.info("%d", 1);
        std::string why;
        if (!m.game_available("handles", &why)) m.warn("%s", why.c_str());
        for (const kyubu::Mod::EnumValue& v : m.game_enum("cube::EntityType", &why)) m.info("%lld %s", (long long)v.value, v.name.c_str());
        KyubuCreature player = 0;
        KyubuCreatureInfo info{};
        if (m.creature_player(player) == KY_OK && m.creature_get(player, info) == KY_OK) m.info("%f", (double)info.hp);
        KyubuSpawn spawn{};
        spawn.entity_type = 37;
        spawn.hostile_type = 1;
        spawn.level = 5;
        KyubuCreature slime = 0;
        if (m.creature_spawn(spawn, slime) != KY_OK) m.warn("no slime");
        static const uint32_t cooldowns[2] = {0, 1000};
        KyubuAbilityDef def{};
        def.duration_ms = 500;
        def.max_rank = 1;
        def.cooldown_ms = cooldowns;
        def.slot = KY_ABILITY_SLOT_FOURTH;
        def.class_type = 1;
        def.specialization = KY_ABILITY_SPEC_ANY;
        m.ability_define("cxx_check:burst", def);
        uint32_t number = 0, ms = 0, rank = 0, count = 0;
        std::string id;
        if (m.ability_number("cxx_check:burst", number) == KY_OK && m.ability_name(number, id) == KY_OK) m.info("%s", id.c_str());
        if (m.ability_list(0, id, number, &count) == KY_OK && m.ability_slot_owner(1, 0, id, number) == KY_OK) m.info("%u", count);
        KyubuAbilityInfo ai{};
        if (m.ability_get(player, ai) == KY_OK && m.ability_cooldown(player, ai.number, ms) == KY_OK) m.info("%u", ms);
        if (m.ability_start(player, number) == KY_OK) m.ability_end(player);
        if (m.ability_rank_get(number, rank) == KY_OK) m.ability_rank_set(number, rank + 1, rank);
    }
};
KYUBU_MOD("cxx_check", KY_SIDE_CLIENT, Check)
