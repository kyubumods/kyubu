# Items and power

Atlas page for the `items` subsystem: the item data, how an item is named and described, equipment, crafting, the
vendors, and the adaption and enchant screens. Reference pages: [docs/game/items.md](../../docs/game/items.md).

## Purpose

Items are what the player picks up, carries, equips, crafts, buys and sells: weapons and armour, consumables,
crafting materials, formulas (recipes), pets, coins, and the travel items (hang glider, boat). An item is a small
value record (`cube::ItemData`) that is copied between the ground, the inventory, the equipment slots and the network.
Its name and its info box are computed from that record when the UI draws it; its stats feed
[combat](combat.md) through the equipped items.

## Key classes

There is no item class in RTTI: items are plain data. From cuwo (candidate, `cuwo:cuwo/entity.pyx`) and confirmed in
part by the code below:

| Type | What it is |
|---|---|
| [`cube::ItemData`](../../docs/game/types/cube_ItemData.md) | one item, 0x118 bytes: type +0, sub_type +1, modifier +4, minus_modifier +8, rarity +0xC, material +0xD, flags +0xE, level +0x10, up to 32 upgrades from +0x14, upgrade_count +0x114. [item_name](../../docs/game/items.md) reads type, sub_type, modifier, material and the upgrades at these offsets, and [item_received_message](../../docs/game/items.md) reads type, material and level: the layout holds in memory for those fields |
| [`cube::ItemUpgrade`](../../docs/game/types/cube_ItemUpgrade.md) | 8 bytes: x, y, z, material, level; probably a voxel added to the item's model (see the voxel widget below) |
| [`cube::ItemId`](../../docs/game/types/cube_ItemId.md) | cuwo's item type and sub-type values (weapons, armour, FORMULA, COIN, PLATINUM_COIN, PET, HANG_GLIDER, BOAT ...) |
| `cube::CreatureData.equipment` | the 13 equipped items of a creature (cuwo) |

The item screens are `plasma::Widget` subclasses, `verified` as classes (RTTI) and listed on the [ui](ui.md) page:
[`cube::InventoryWidget`](../../docs/game/types/cube_InventoryWidget.md) (three instances: the bags),
[`cube::EnchantWidget`](../../docs/game/types/cube_EnchantWidget.md),
[`cube::AdaptionWidget`](../../docs/game/types/cube_AdaptionWidget.md) (the weapon adaption, paid in platinum coins),
[`cube::BlueprintPreviewWidget`](../../docs/game/types/cube_BlueprintPreviewWidget.md) (probably the crafting preview)
and [`cube::VoxelWidget`](../../docs/game/types/cube_VoxelWidget.md) (its draw slot uses the string Weapon
Customization).

## Entry points

All candidate, Cube.exe:

- [item_name](../../docs/game/items.md) (0x5972A0): (out string, item) on a lookup object. Builds the display name:
  a quality word from the item:shabby … item:fabulous keys, the material, the type name (0x59E4C0 on type and
  sub-type), Formula: and a recursive call for recipes, Pet: for pets. Called by the inventory's draw slot, the item
  info box, the blueprint preview's draw slot (0x430110), [item_received_message](../../docs/game/items.md) and
  `cube::GameController::update()`.
- [material_name](../../docs/game/items.md) (0x59E850): a switch from the material id to its Material&lt;Name&gt; key
  (1 iron, 2 wood, 5 obsidian, 7 bone, 10 copper, 11 gold, 12 silver, 13 emerald …).
- [item_tooltip](../../docs/game/items.md) (0x4A2A80): the item info box: level, power, the DMG, ARMOR, RESI, TEMPO,
  CRIT, REG and HP lines, " (adapted)", the class restriction, Already known (formulas), Ridable (pets). Called by the
  draw slots (vtable slot 1) of the adaption and enchant widgets and by 0x4D3DF0.
- [item_received_message](../../docs/game/items.md) (0x481470): the loot line, "You receive …" when the creature is
  the player (GameController+0x8006C8) and "… receives …" otherwise.
- [ModelLoader::itemModel](../../docs/game/items.md) (0x4EB290, task 8.4): the model an item is drawn with, from the
  model table (`ModelLoader`, GameController + 0x2F8). It switches on the `cube::ItemData` type (+0), then the
  sub_type (+1) or the material (+0xD). Weapons (type 3) go through
  [weapon_model_slot](../../docs/game/items.md) (0x51ACE0: sub_type, modifier, rarity, material; mostly a base by
  sub_type and material + (modifier % 11) × 5 + min(rarity, 4)). The armour types 4 to 9 use a base by type and
  material + (modifier % 5) × 5 + min(rarity, 4); an iron (1) chest (4) piece with modifier 0 and rarity 0 is slot
  0x464. Consumables go through 0x4EB200 on the sub_type, and several types and special materials have fixed slots.
  So the modifier and the rarity choose among five or eleven variants of a model. Callers: `draw_creature` for the
  equipment ([creatures](creatures.md)), `cube::GameController::update()` and `render()`, and the item widgets'
  draw slots. A mod gives one item a model of its own by hooking this function (probe [bow-model](../probes/bow-model.toml),
  2026-10-12): a Lua detour `void*(__thiscall*)(void*, void*)` that tests a mark on the item (a bow whose `minus_modifier` was
  0x4C49) and returns `ModelLoader::get(slot)` for the mod's slot (from `kyubu.model.add`, 2569 on) drew a white bow of the
  mod's in the hand and left every other item alone. The function ran about 2000 times a second (every frame, for the items on
  the screen), so the test must be cheap.
- The widget constructors on the [ui](ui.md) page, for example `cube::InventoryWidget::InventoryWidget()` and
  `cube::EnchantWidget::EnchantWidget()`, all called by `cube::GameController::GameController()`.

Item power and the equip gate (read statically on 2026-09-30):

- [item_power](../../docs/game/items.md) (0x4C75D0, candidate): ⌊100·level_curve(item level) + 1⌋. This is the "+N"
  of item names (the " +" suffix in `cube::GameController::update()` and item_name's callers) and the Power line of
  the info box. It is 1 at item level 1, 84 at level 100, and 100 only at level 1981.
- [can_equip_item](../../docs/game/items.md) (0x43ECC0, candidate): the power gate. It fails when the item's power
  exceeds the creature's own power, computed from `cube::CreatureData.level` the same way; after that it runs the
  class check 0x4C6E50.
- [level_up_teach_formulas](../../docs/game/items.md) (0x47FFA0, candidate): on each level-up it adds formulas to
  the player's known list, chosen by level mod 3, 4 and 5 and by class (see Data flow 5).

Item stats (read statically on 2026-09-30, all candidate). [item_tooltip](../../docs/game/items.md) calls each
getter right before it prints the getter's label, which names them. Each takes the `cube::ItemData` in ECX and
returns a float in st0:

| Getter | Address | Info-box label | Also called by |
|---|---|---|---|
| [item_damage](../../docs/game/items.md) | 0x4C7E00 | DMG (weapons only) | [creature_damage](../../docs/game/combat.md), [creature_projectile_damage](../../docs/game/combat.md) |
| [item_hp](../../docs/game/items.md) | 0x4C6FE0 | HP | `cube::Creature::maxHp()` (six calls) |
| [item_armor](../../docs/game/items.md) | 0x4C69D0 | ARMOR | `cube::Creature::armor()` (four calls) |
| [item_resistance](../../docs/game/items.md) | 0x4C7990 | RESI | `cube::Creature::resistance()` (four calls) |
| [item_tempo](../../docs/game/items.md) | 0x4C7AA0 | TEMPO | [creature_tempo](../../docs/game/combat.md) (nine calls) |
| [item_crit](../../docs/game/items.md) | 0x4C6AE0 | CRIT | [creature_crit](../../docs/game/combat.md) (nine calls) |
| [item_regeneration](../../docs/game/items.md) | 0x4C7760 | REG | [creature_regeneration](../../docs/game/combat.md) (nine calls) |
| [item_heal](../../docs/game/items.md) | 0x4C6D50 | HP + (consumables) | `cube::World::update()` |

- [stat_scale](../../docs/game/items.md) (0x446560): the multiplier all the getters share,
  2^(3·level_curve(L)) · 2^(rarity/4), where the getters pass L = level + 0.1 · upgrade_count. So an item's stats
  grow with its level exactly like the level factor of max HP (see [combat](combat.md), "Levels, power and
  experience"), each rarity step is ×1.19, and each upgrade adds a tenth of a level.
- [item_usable_by_class](../../docs/game/items.md) (0x4C6E50): the class check that
  [can_equip_item](../../docs/game/items.md) runs after the power gate (item, class type). The info box asks it for
  classes 1 to 4 to print Warrior, Ranger, Mage, Rogue.
- [item_value](../../docs/game/items.md) (0x4C7610): an item's price in coins, an int (the formula is below; no
  probe has bought anything yet). Its callers are `cube::GameController::onMouseDown()` and the inventory, enchant and
  preview draw slots.

What the getters compute (read from the code on 2026-10-01, all candidate; the tables and constants are re-checked by
`kyubu-mapping/evidence/item-formulas-2026-10-01/item_formulas.py`). Let S be
[stat_scale](../../docs/game/items.md) of (level + 0.1 · upgrade count, rarity), and r = (modifier × 8) mod 21, an
unsigned remainder from 0 to 20 (`modifier` is `cube::ItemData` +4). "Chest" is item type 4:

| Getter | Item types | Formula |
|---|---|---|
| [item_hp](../../docs/game/items.md) | 3 to 7, else 0 | S · 5.0 · (chest 1.0, else 0.5) · (2 − r/20 + bonus); bonus by material: 1 → +1.0, 26 → +0.5, 27 → +0.75 |
| [item_regeneration](../../docs/game/items.md) | 3 to 7, else 0 | S · (chest 0.2, else 0.1) · (r/20 + bonus); bonus: material 26 → +0.5, 27 → +1.0 |
| [item_armor](../../docs/game/items.md) | 4 to 7, else 0 | S · (chest 1.0, else 0.5) · f; f by material: 18 → 0.8, 19, 26 and 27 → 0.85, 23 and 25 → 0.75, else 1 |
| [item_resistance](../../docs/game/items.md) | 4 to 7, else 0 | S · (chest 1.0, else 0.5) · f; f by material: 1 and 19 → 0.85, 26 and 27 → 0.75, else 1 |
| [item_damage](../../docs/game/items.md) | 3 (weapons), else 0 | S · k; k by sub_type: 2 for 3, 4 and 13; 8 for 6, 7, 8, 10, 11 and 15 to 18; 4 for every other, including 5 |
| [item_value](../../docs/game/items.md) | all | max(1, int(stat_scale(level, rarity) · 10 · f)), the stored level only; f by item type: 3 and 4 → 2, 5 and 6 → 1, 7 and 8 → 1.5, 9 → 1, 10 to 22 → 0.2, 23 → 100; a weapon with sub_type 5, 6, 7, 8, 10, 11 or 15 to 18 doubles f again |

- The roll r is the only per-item randomness: one modifier always rolls the same. For HP it spans ×2.0 (r = 0) to ×1.0
  (r = 20); for regeneration ×0 to ×1.0. The instruction after the remainder adds an entry of a two-entry table
  (0.0 and 2^32) by the sign bit: it is the compiler's unsigned-to-double conversion, not an offset.
- The material numbers are the `material` byte (+0xD) of `cube::ItemData`; which material is which is not mapped here
  except by [material_name](../../docs/game/items.md).
- Rarity and level act only through S: each rarity step is ×1.19. A price does not include the upgrades.
- These are vanilla's. CubicExpansion's getters have the same structure with its own numbers (×0.5 for HP, a final ×0.8
  on armour and resistance, a roll offset by −1.5, its own prices), so its constants do not carry over; see the
  evidence page.

The getters were first found in another build of the game by ToufouMaster's CubicExpansion mod. The match to this
build, and the leads it gave that are not matched yet (crafting, resource drops), are in
`kyubu-mapping/evidence/toufoumaster-crossmatch-2026-09-30/`; the formulas above, its Server.exe twins and the leads
from a second reading of its source are in `kyubu-mapping/evidence/item-formulas-2026-10-01/`.

Not named yet: the widgets' draw slots (vtable slot 1): 0x4C1F80 (inventory: item frames, Platinum Coins),
0x40FAE0 (adaption: Adapt, COST:), 0x44F0C0 (enchant), 0x586E00 (voxel widget: Weapon Customization); 0x435620 on
the character sheet (Weapon Rating, Armor Rating); the sound loader 0x484820 (pickup.wav, drop-item.wav, craft.wav,
disenchant2.wav).

## Data flow

1. **Start-up.** `ModelLoader::loadAll()` loads the item models by name (gold-ring1-random1.cub …, life-potion.cub,
   gold-coin.cub, formula.cub). `cube::GameController::GameController()` loads the inventory, crafting and vendor
   tabs (inventory-equipment.png, inventory-items.png, inventory-ingredients.png, inventory-pets.png, craft-*.png for
   the 12 crafting categories, buy.png, buy-back.png) and finds the nodes itembox, wideitembox, itemselector,
   equipmentboxleft, equipmentboxright, equippedpreview, quickitembutton, craftbutton and craftbar.
2. **Getting an item.** Loot and pickups end in [item_received_message](../../docs/game/items.md), called from
   `cube::GameController::update()` and two other functions (0x470C00, 0x47BB30), which prints the chat line. Where
   the item is added to the inventory is not traced yet.
3. **Showing an item.** Each frame the item widgets' draw slots call [item_name](../../docs/game/items.md) and, for
   the hovered item, [item_tooltip](../../docs/game/items.md). Names and info are computed from the
   `cube::ItemData` each time, not stored.
4. **Equipping.** The equipped items are `cube::CreatureData.equipment` (13 slots, cuwo); the character sheet
   (0x435620) turns them into the weapon and armour ratings. A creature's stats add up the item getters over the worn
   items: `cube::Creature::maxHp()` calls [item_hp](../../docs/game/items.md), `cube::Creature::armor()` calls
   [item_armor](../../docs/game/items.md), and so on (the table under Entry points). An item's stat is a base by type,
   times [stat_scale](../../docs/game/items.md) of its level, upgrades and rarity. Equipping is gated
   by [can_equip_item](../../docs/game/items.md): the item's power must not exceed the player's. Each frame
   `draw_creature` draws the items in slots 1 to 7 on the creature, with the models from
   [ModelLoader::itemModel](../../docs/game/items.md) (the part table is on the [creatures](creatures.md) page).
5. **Crafting, adapting, enchanting.** Formulas are items of type FORMULA whose name is Formula: plus the product's
   name. The crafting screen (craftbutton, craftbar, the craft-*.png categories, craft.wav and craft-proc.wav) and the
   adaption screen (paid in platinum coins) are drawn by their widgets; the code that consumes the ingredients is not
   traced yet.
   - Known formulas are a list of `cube::ItemData` records in the player style (Creature+0x1D28, list at +0x14).
   - [level_up_teach_formulas](../../docs/game/items.md) adds to it on each level-up:
     - potions and food: level mod 4 = 1 and 3, and level mod 5 = 1 to 4;
     - starter items at level 1, which depend on the class;
     - class armour or weapon formulas cycling with level mod 3, some with a random variant.
6. **Saving.** The character, with its inventory and equipment, goes to Save/characters.db ([saves](saves.md)). The
   blob layout is decoded there:
   - equipment: 13 `cube::ItemData` copied raw;
   - inventory: 4 tabs of 50 slots of 0x11C bytes;
   - known formulas: `cube::ItemData` records.

   The padding bytes of the saved items hold garbage.
7. **Item levels in the level_rework mod** (add-level-rework, tasks 6.2 to 6.6; `Cube.exe` only, the `Server.exe` twins
   are 8.x). The stored level word (+0x10) of an item the mod makes is `2000 + N`, N = 1 to 100 the mod level; a word
   below 2000 is a vanilla item. The mod reads it through these entries:
   - the generators ([item_generate](../../docs/game/items.md), item_generate_alt, item_generate_any) get N from the
     kill drop (kill_drop_level_site), the creature set-up, the zone bonus and the area level, and the mod stores
     `2000 + N` (only when the whole item scale below is installed; otherwise it stores the old conversion);
   - [item_power](../../docs/game/items.md) returns N for a mod item, so the "+N" of every name and the Power line follow;
   - [stat_scale](../../docs/game/items.md) and stat_scale_eighth give one tier per mod level to the 20 item callers, the
     sheet's 6 partners use the creature's level on the same tier, and the 3 creature sites keep the original (routed
     by return address);
   - [item_level_reroll](../../docs/game/items.md) re-rolls a mod item inside N ± 5 (the kill drop's window);
   - the equip gate compares N with the character's level;
   - the adaption screen prices the gap between N and the player's level (hooks on the price 0x40F701, the draw
     0x410D95, the coin check 0x47CCC5, the store 0x47CCFE and the preview's copy 0x40FE65); a vanilla item above the
     player's level costs 0 and keeps its level. The game opens the screen when the player uses an entity of type 0x89
     (found in the interaction code near 0x488D54, which shows the node for the entity types of the stations);
     probe item-adaption opens it by writing the node's visible flag instead.

   The pet's XP line of [item_tooltip](../../docs/game/items.md) (0x4A39B1) is not a Power line and has no hook.

## Client and server

- The names, the info box, the screens and the sounds are Cube.exe only: Server.exe has none of the item:,
  Material… or inventory strings, and none of these functions correlate.
- The stats do run on the server. Server.exe has [stat_scale](../../docs/game/items.md) (0x410890),
  [item_armor](../../docs/game/items.md) (0x413350), [item_hp](../../docs/game/items.md) (0x413680),
  [item_resistance](../../docs/game/items.md) (0x413C00) and [item_damage](../../docs/game/items.md) (0x413EF0), each
  the same length as Cube.exe's with 84 to 91% of the bytes equal, called by its own creature stat functions
  ([combat](combat.md)). A mod that changes item stats has to hook both builds.
- Server.exe does know the item names of its world-start table (0x4C8580, twin of Cube.exe's 0x58D370 by a unique
  string: Formula, HangGlider, Boat …) and loads formula.cub through its `ModelLoader::loadAll()`.
- Per cuwo, drops, pickups and the items in the creature updates travel as `cube::ItemData` in packets, and the
  server decides what drops. Not traced in the binaries yet ([network](network.md), 6.6).

## Hook points of interest

- [item_name](../../docs/game/items.md) (candidate): every place an item's name is shown; a mod can rename items or
  add a prefix.
- [item_tooltip](../../docs/game/items.md) (candidate): add lines to the item info box (a mod's own stat).
- [item_received_message](../../docs/game/items.md) (candidate): observe loot as it arrives, with the item and the
  receiving creature.
- `cube::InventoryWidget::InventoryWidget()` (candidate, [ui](ui.md)): the inventory screens after construction.
- [ModelLoader::itemModel](../../docs/game/items.md): every item's model, in the world and in the widgets. It is the
  one place where a mod item could be given a model slot of its own (add-mod-loader 7.3).
- [item_power](../../docs/game/items.md) and [can_equip_item](../../docs/game/items.md) (candidates): change how an
  item's level becomes power, and the equip gate. Note that can_equip_item computes the player's power inline rather
  than through item_power. level_rework hooks item_power (a mod item returns its level N) and gates on a whole-number
  compare at can_equip_item_level_site.
- [stat_scale](../../docs/game/items.md) (candidate, both builds): the one place where an item's level, upgrades and
  rarity become stats, so it is where a level rework converts item levels. It is also called by the character
  sheet's ratings, so a hook changes those too. It has a twin, stat_scale_eighth (0x4465F0), for item_crit, item_tempo
  and one more getter. level_rework hooks both and tells the callers apart by return address (29 sites).
- The item getters (candidates, the table under Entry points): change one stat of one kind of item. They receive only
  the item, not who wears it, so a rule that depends on the wearer (its level, say) needs the caller's creature.
- [cube::GameController::equipmentSlotUnderCursor](../../docs/game/items.md) (candidate, read statically 2026-10-17): the
  player's equipment slot under the mouse, or null while the character window is closed. Every click that takes an
  equipped item out starts from it. A right click sends the item to the bags (inventory_add) and empties the slot. A left
  click swaps it with the item on the cursor (the cursor's count at Creature+0x11E8, its item at +0x11EC). A left click
  on the left hand while the right holds a two-handed weapon (bows too) bags that weapon. When the detour returns null, all
  of these do nothing. `GameController::update` asks it for the hovered item's info box, which a null hides. skills_unleashed
  returns null for both weapon slots while its bow is in the hand (task 8.10). Not replayed: the play-test checks it.

## Open questions

- Where an item enters the inventory, and where the inventory lives (the player's creature or the game controller).
  Static trace from [item_received_message](../../docs/game/items.md)'s callers, then the inspector.
- The item stats are answered statically (Entry points, the formula table): what stays open is a probe that confirms a
  getter in game (hook [item_hp](../../docs/game/items.md) and read the HP line of the info box; buy or sell one item
  to confirm [item_value](../../docs/game/items.md)), and which material number is which.
- Where item levels come from (task 6.1 of add-level-rework): every generated item gets its level word (+0x10) from
  [item_generate](../../docs/game/items.md) or its twin item_generate_alt, whose second stack argument is the level;
  item_generate_any is a front end for both. A kill's gear drop (1 kill in 10, via kill_drop_level_site) passes the
  victim's level, or one less by a coin flip; a spawn's loot list passes the Spawn's level (read statically; not reached on a dungeon visit). The store
  and the call in onMouseDown are not sources. The probes item-level-sites and item-level-sites-dungeon observed the
  level argument equal to the creature level (61 and 60 for level-61 victims). With level_rework 6.6 the same sites
  log `item level 61 -> 2061` (kills, and the dungeon at 0x5F6EC3). Open: which trigger made the dungeon's
  one call (through 0x5F6EBE), and where a chest's loot is rolled.
- Crafting: the recipe table and the function that consumes the ingredients (from the craftbutton callback).
  CubicExpansion's recipe hook is a lead (a station for each craft, ten stations; its recipes include its own
  additions): see the leads in `kyubu-mapping/evidence/item-formulas-2026-10-01/README.md`.
- Adaption and enchanting: what they change in the `cube::ItemData` (modifier? upgrades?). Upgrades count in the
  stats: the item getters pass [stat_scale](../../docs/game/items.md) the level plus 0.1 per upgrade (upgrade_count,
  +0x114).
- What the lookup object of [item_name](../../docs/game/items.md) is (a name table, possibly filled by the
  world-start function 0x58D370).
- What the voxel widget edits: the `cube::ItemUpgrade` voxels (x, y, z, material) look like weapon customization.
- The cuwo `cube::ItemData` field not read yet (flags; rarity at +0xC is read by the item getters, which pass it to
  [stat_scale](../../docs/game/items.md)), and the whole layout, with the inspector
  ([probes](../../docs/probes.md)).
- Name the widgets' draw slots (vtable slot 1) once one is read; the [ui](ui.md) page leaves the slot meanings open.
- Server.exe's item code: drops and loot, starting from the packet handlers (6.6).

## Depth

reconnaissance, reviewed 2026-09-30

## Level rework sites

Sites of item levels that the `level_rework` mod uses or watches (change `add-level-rework`, design D5; tasks 6.x and 8.x). Addresses are
`Cube.exe`, then `Server.exe`. Item 7 of the data flow above says what the mod does with them. The mod's tables are in
the `level_rework` repository.

- **Where the game makes items.** The level word of a new item is the creature's, the dungeon's or the area's level N; the mod stores
  `2000 + N`.
  - [kill_drop_typed_item_site](../../docs/game/items.md) (0x599F80, 0x4D3000): the 1 to 3 other drops of a kill get ESI as their
    level word (1 when the gear branch did not run).
  - [spawn_loot_site](../../docs/game/items.md) (0x5D827A, 0x50D3BA): the two items rolled into each new Spawn's list (chests too);
    the level is Spawn+0x34. The returns (0x5D827F and 0x5D82DB; 0x50D3BF and 0x50D41B) are converted in a generator detour. No
    replay reached them.
  - [bonus_item_site](../../docs/game/items.md) (0x5952A6, 0x4CF9E6): the zone-completion item of `cube::World::applyHit`, with the
    mission zone's level and its "+p" as the rarity. The `Cube.exe` entry is still a `guess`.
  - [spawn_elite_item_site](../../docs/game/items.md) (0x5EBDBB, 0x520F0B): an elite (flag 0x200) gets an extra item of type 0x0B whose
    level word is Spawn+0x34, with no generator call. Not read further.
  - [item_generate_level_one](../../docs/game/items.md) (0x5F63F0): the third generator of a kill drop; its level word is always 1, so
    it needs no hook.
- **The gear creatures wear.** [spawn_equipment](../../docs/game/items.md) (0x5C6170, 0x4FB220) gives each of a Spawn's nine worn items the
  Spawn's +0x34 as its level word, and the creature set-up re-rolls the words downward (`item_level_reroll`, window 0.05). No generator runs,
  so the mod hooks the function and writes `2000 + N` afterwards; left alone, a level-55 creature wore and dropped vanilla power 68 to 73.
  The zone generator (0x5EF6F0) has 20 stores of its own for Spawns of a fixed level of 20; they are not hooked. Probe: `worn-gear`.
- **The equip gate.** [can_equip_item_refused_site](../../docs/game/items.md) (0x43ED14) and
  [can_equip_item_class_site](../../docs/game/items.md) (0x43ED1C) are `verified` observation points after the power compare (the engine
  cannot hook the jump before them). The mod compares whole numbers at [can_equip_item_level_site](../../docs/game/items.md) instead.
- **The adaption.** [adaption_commit_level_site](../../docs/game/items.md) (0x47CCFE) is the store of the player's level over the item's
  stored level after payment. The mod stores `2000 + N` when the item's level is below N, and the same word otherwise.
- `Server.exe` has twins of the sites above, found by shifting inside one block and matching the instructions; it has no twin of
  `item_regeneration`. Probes: `item-level-sites`, `item-level-sites-dungeon`, `item-scale`, `item-adaption`, and on `Server.exe`
  `server-level-sites`, `server-level-sites-items` and `server-kill-drop-sites`. Not reached by any probe: `item_generate_level_one`, the
  spawn loot returns, the quest bonus item, and a dungeon chest.
