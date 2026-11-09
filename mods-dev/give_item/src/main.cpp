// give_item: puts an item into the local player's bags (Cube.exe, single-player or joined to a server: the bags are the client's own). The console command give_item:give builds an item from its
// arguments (or asks the game's own generator for a random one) and gives it with the game API's creature_give_item. It then reads the bags
// back with raw access, only to say where the item landed (the dev check of the API call).
//
//   give_item:give <type> <sub_type> [level] [rarity] [material] [modifier]
//   give_item:give random <level> [rarity] [category]
//   give_item:give coins <amount> [copper|silver|gold]
//
// The level is the stored (vanilla) level of the item, not a mod level: the game shows the item's power, which grows with it
// (stored 21 is power 51, stored 1981 is power 100). For the stackable types the level word is the quantity (the game stores level 1).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <cubeworld/cubeworld.h>

#include "kyubu.hpp"

namespace {

kyubu::Mod* g_mod = nullptr;

constexpr uint32_t kInventoryOffset = 0x11DC;   // Creature+0x11DC: the bags (mapping: cube::Creature.inventory)
constexpr uint32_t kSlotSize = 0x11C;           // a count word, then the 0x118-byte ItemData
constexpr uint32_t kCoins = 0x128;              // the coin counter of the bags
constexpr uint32_t kWorldOffset = 0x2E4;        // GameController+0x2DC: the World, the `this` of the item generators

uintptr_t controller() {
    const uintptr_t global = g_mod->symbol("cube::game_controller");
    uint32_t gc = 0;
    if (!global || g_mod->api()->mem_read(global, &gc, sizeof gc) != KY_OK) return 0;
    return gc;
}

uintptr_t player_creature() {
    const uintptr_t gc = controller();
    uint32_t player = 0;
    if (!gc || g_mod->api()->mem_read(gc + offsetof(cube_GameController, player), &player, sizeof player) != KY_OK) return 0;
    return player;
}

void say(KyubuPrintFn print, void* ctx, const char* text) {
    print(ctx, text);
    g_mod->info("%s", text);   // the log keeps it too: replays read the log
}

// Where an item with these type, sub_type and material is in the bags now: the last such slot. false when none.
bool find_slot(uintptr_t inventory, const cube_ItemData& want, int* tab_out, int* slot_out, int32_t* count_out) {
    uint32_t begin = 0, end = 0;
    if (g_mod->api()->mem_read(inventory, &begin, 4) != KY_OK || g_mod->api()->mem_read(inventory + 4, &end, 4) != KY_OK || !begin || end < begin) return false;
    bool found = false;
    for (uint32_t t = 0; t < (end - begin) / 12 && t < 8; ++t) {
        uint32_t first = 0, last = 0;
        if (g_mod->api()->mem_read(begin + t * 12, &first, 4) != KY_OK || g_mod->api()->mem_read(begin + t * 12 + 4, &last, 4) != KY_OK || !first || last < first) continue;
        for (uint32_t s = 0; s < (last - first) / kSlotSize && s < 512; ++s) {
            uint8_t raw[kSlotSize];
            if (g_mod->api()->mem_read(first + s * kSlotSize, raw, sizeof raw) != KY_OK) break;
            const cube_ItemData* item = reinterpret_cast<const cube_ItemData*>(raw + 4);
            // a stackable type stores level 1 and takes its quantity from the level word, so its level is not compared
            const bool stackable = want.type == 0 || want.type == 0x0B || want.type == 0x0C || want.type == 0x0D || want.type == 0x14 || want.type == 0x15 || want.type == 0x17 ||
                                   want.type == 0x18 || want.type == 0x19;
            if (item->type == want.type && item->sub_type == want.sub_type && item->material == want.material && item->modifier == want.modifier && item->rarity == want.rarity &&
                (stackable || item->level == want.level)) {
                found = true;
                *tab_out = static_cast<int>(t);
                *slot_out = static_cast<int>(s);
                std::memcpy(count_out, raw, 4);
            }
        }
    }
    return found;
}

bool parse_int(const char*& s, long* out) {
    while (*s == ' ') ++s;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 0);
    if (end == s) return false;
    s = end;
    *out = v;
    return true;
}

void give(const cube_ItemData& item, KyubuPrintFn print, void* ctx) {
    KyubuCreature player = 0;
    if (g_mod->creature_player(player) != KY_OK) {
        say(print, ctx, "give_item: no player yet (enter a world first)");
        return;
    }
    const uintptr_t inventory = player_creature() + kInventoryOffset;
    uint32_t coins_before = 0;
    g_mod->api()->mem_read(inventory + kCoins, &coins_before, 4);
    KyubuItem spec = {};
    spec.type = item.type;
    spec.sub_type = item.sub_type;
    spec.rarity = item.rarity;
    spec.material = item.material;
    spec.modifier = item.modifier;
    spec.level = item.level;
    const KyubuResult r = g_mod->creature_give_item(player, spec);
    char buf[256];
    if (r != KY_OK) {
        std::snprintf(buf, sizeof buf, "give_item: creature_give_item failed (result %d; the log says why)", static_cast<int>(r));
        say(print, ctx, buf);
        return;
    }
    uint32_t coins_after = 0;
    g_mod->api()->mem_read(inventory + kCoins, &coins_after, 4);
    if (item.type == 0x0C) {
        std::snprintf(buf, sizeof buf, "give_item: coins: the counter went from %u to %u", coins_before, coins_after);
        say(print, ctx, buf);
        return;
    }
    int tab = -1, slot = -1;
    int32_t count = 0;
    if (find_slot(inventory, item, &tab, &slot, &count))
        std::snprintf(buf, sizeof buf, "give_item: gave type %u sub_type %u level %d rarity %u material %u: it is in bag %d, slot %d (count %d)", item.type, item.sub_type,
                      item.level, item.rarity, item.material, tab, slot, count);
    else
        std::snprintf(buf, sizeof buf, "give_item: gave type %u sub_type %u level %d, but no such slot was found in the bags", item.type, item.sub_type, item.level);
    say(print, ctx, buf);
}

void KYUBU_CALL cmd_give(const char* args, KyubuPrintFn print, void* ctx, void*) {
    const char* s = args ? args : "";
    while (*s == ' ') ++s;
    const char* usage =
        "give_item: usage: give_item:give <type> <sub_type> [level] [rarity] [material] [modifier] | give_item:give random <level> [rarity] [category] | give_item:give coins <amount> [copper|silver|gold]";
    cube_ItemData item;
    std::memset(&item, 0, sizeof item);
    if (std::strncmp(s, "random", 6) == 0 && (s[6] == ' ' || s[6] == 0)) {
        s += 6;
        long level = 0, rarity = 0, category = -1;
        if (!parse_int(s, &level) || level < 1 || level > 32767) {
            say(print, ctx, usage);
            return;
        }
        parse_int(s, &rarity);
        parse_int(s, &category);
        const uintptr_t gen = g_mod->symbol("item_generate");
        const uintptr_t gc = controller();
        if (!gen || !gc) {
            say(print, ctx, "give_item: the item generator is unavailable");
            return;
        }
        reinterpret_cast<cw_item_generate_fn>(gen)(reinterpret_cast<void*>(gc + kWorldOffset), nullptr, &item, static_cast<int16_t>(level), static_cast<uint8_t>(rarity),
                                                   static_cast<int32_t>(category));
        give(item, print, ctx);
        return;
    }
    if (std::strncmp(s, "coins", 5) == 0 && (s[5] == ' ' || s[5] == 0)) {
        s += 5;
        long amount = 0;
        if (!parse_int(s, &amount) || amount < 1 || amount > 32767) {
            say(print, ctx, usage);
            return;
        }
        while (*s == ' ') ++s;
        item.type = 0x0C;
        item.material = std::strncmp(s, "gold", 4) == 0 ? 11 : std::strncmp(s, "silver", 6) == 0 ? 12 : 10;
        item.level = static_cast<int16_t>(amount);
        give(item, print, ctx);
        return;
    }
    long type = 0, sub = 0, level = 1, rarity = 0, material = 0, modifier = 0;
    if (!parse_int(s, &type) || !parse_int(s, &sub) || type < 1 || type > 0x19 || sub < 0 || sub > 255) {
        say(print, ctx, usage);
        return;
    }
    parse_int(s, &level);
    parse_int(s, &rarity);
    parse_int(s, &material);
    parse_int(s, &modifier);
    if (level < 1 || level > 32767 || rarity < 0 || rarity > 4 || material < 0 || material > 255) {
        say(print, ctx, usage);
        return;
    }
    item.type = static_cast<uint8_t>(type);
    item.sub_type = static_cast<uint8_t>(sub);
    item.level = static_cast<int16_t>(level);
    item.rarity = static_cast<uint8_t>(rarity);
    item.material = static_cast<uint8_t>(material);
    item.modifier = static_cast<uint32_t>(modifier);
    give(item, print, ctx);
}

}  // namespace

struct GiveItem {
    explicit GiveItem(kyubu::Mod& m) : mod(m) {
        if (m.side() != KY_SIDE_CLIENT) return;
        g_mod = &m;
        if (m.api()->register_command(m.handle(), "give_item:give",
                                      "give_item:give <type> <sub_type> [level] [rarity] [material] [modifier] | give_item:give random <level> [rarity] [category] | give_item:give coins <amount> [copper|silver|gold]: put an item into the local player's bags (the level is the stored level)",
                                      cmd_give, nullptr) != KY_OK)
            mod.warn("give_item: could not register the give command");
        mod.info("give_item: loaded");
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("give_item", KY_SIDE_CLIENT, GiveItem)
