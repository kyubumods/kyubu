// The items primitive: putting an item into a creature's bags with the game's own insert (docs/game-api.md, creature_give_item;
// mapping/atlas/primitives.md, Items).
#include <cstddef>
#include <cstring>

#include "call.h"
#include "cubeworld/functions.h"
#include "game.h"
#include "log.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

constexpr uint32_t kItemMinSize = offsetof(KyubuItem, level) + sizeof(int32_t);
constexpr uint32_t kCoinMaterialCopper = 10, kCoinMaterialGold = 11, kCoinMaterialSilver = 12;   // what inventory_add counts

KyubuResult KYUBU_CALL k_creature_give_item(KyubuModHandle self, KyubuCreature c, const KyubuItem* item) {
    GameCall g(self, Prim::items, "creature_give_item");   // client-only (kSideRules)
    if (!g) return g.result();
    if (!item || item->size < kItemMinSize)
        return g.refuse(KY_E_INVALID_ARG, item ? fmt("item->size is %u, below %u", item->size, kItemMinSize) : "item is NULL");
    if (item->type < 1 || item->type > KY_ITEM_TYPE_MAX)
        return g.refuse(KY_E_INVALID_ARG, fmt("type is %u, outside 1 to %u", (unsigned)item->type, KY_ITEM_TYPE_MAX));
    if (item->rarity > 4) return g.refuse(KY_E_INVALID_ARG, fmt("rarity is %u, above 4", (unsigned)item->rarity));
    if (item->level < 1 || item->level > 32767)
        return g.refuse(KY_E_INVALID_ARG, fmt("level is %d, outside 1 to 32767", (int)item->level));
    if (item->type == KY_ITEM_TYPE_COINS && item->material != kCoinMaterialCopper && item->material != kCoinMaterialGold &&
        item->material != kCoinMaterialSilver)
        return g.refuse(KY_E_INVALID_ARG, fmt("coins need material 10, 11 or 12, not %u", (unsigned)item->material));
    cube_Creature* cr = g.creature(c);
    if (!cr || !g.owns(cr)) return g.result();
    const cw_inventory_add_fn add = (cw_inventory_add_fn)verified_symbol(CW_SYM_inventory_add);
    if (!add) return g.refuse(KY_E_UNAVAILABLE, "the game's inventory insert is not mapped for this build");

    cube_ItemData data;
    memset(&data, 0, sizeof data);
    data.type = item->type;
    data.sub_type = item->sub_type;
    data.modifier = item->modifier;
    data.rarity = item->rarity;
    data.material = item->material;
    data.level = (int16_t)item->level;
    add(&cr->inventory, nullptr, &data, -1);
    return KY_OK;
}

}  // namespace

void fill_items(KyubuApi& a) { a.creature_give_item = k_creature_give_item; }

}  // namespace kyubu::game
