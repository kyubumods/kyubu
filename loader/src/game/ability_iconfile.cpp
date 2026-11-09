// A mod's own ability icon (design D14).
#include "ability_iconfile.h"

#include <map>

#include "abilities.h"
#include "game.h"
#include "gametypes.h"
#include "log.h"
#include "overlay.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

constexpr const char* kLoadIcon = "cube::GameController::loadIcon";
constexpr const char* kIconDb = "data3.db";
constexpr uint32_t kIconFlagsOffset = 0x30;   // five flags; the second and third are the ones the game clears

// void* __thiscall loadIcon(GameController* this, const std::string* key, cube::Database* db), `ret 8`, the icon object in EAX.
// __fastcall with an unused EDX has the thiscall ABI.
using LoadIconFn = void*(__fastcall*)(cube_GameController*, void*, const VcString*, void*);

IconFileOps g_ops = {};
bool g_ops_set = false;
std::map<uint32_t, uint32_t> g_cache;   // ability number -> icon object (0: tried and failed)

// The objects loadIcon returns hold five 4-byte flags at +0x30 (the engine copies the {1,1,1,1,1} that 0x658110 passes when it makes the
// texture). The game's own icons end up with {1,0,0,1,1}: probe ability-icon-compare dumped smash's object (made by the constructor) and a copy of
// smash.png made by Kyubu, which differed in these two flags only (apart from the pointers and the id), and the copy drew softer than the original.
// Something in the game clears the two after the constructor's loads; this does the same for an icon loaded later. Only a pair of 1s is changed.
void match_vanilla_icon_flags(void* icon) {
    uint32_t* flags = (uint32_t*)((uint8_t*)icon + kIconFlagsOffset);
    if (flags[1] == 1 && flags[2] == 1) flags[1] = flags[2] = 0;
}

bool real_exists(const std::string& key) { return Assets::get().resolve(kIconDb, key).has_value(); }

uint32_t real_load(cube_GameController* gc, const std::string& key) {
    std::string why;
    const uintptr_t fn = verified_symbol(kLoadIcon, &why);
    void* db = fn ? Assets::get().our_database(kIconDb) : nullptr;
    if (!fn || !db) {
        KWARN("abilities: no custom icon: %s", fn ? "cannot open data3.db" : why.c_str());
        return 0;
    }
    VcString k;
    vc_string_init(&k, key);
    void* icon = ((LoadIconFn)fn)(gc, nullptr, &k, db);
    vc_string_free(&k);
    if (icon) match_vanilla_icon_flags(icon);
    return (uint32_t)(uintptr_t)icon;
}

}  // namespace

uint32_t ability_file_icon(cube_GameController* gc, uint32_t number) {
    const AbilityStat* st = ability_stat(number);
    if (!st || st->icon_file.empty()) return 0;
    const auto hit = g_cache.find(number);
    if (hit != g_cache.end()) return hit->second;
    if (!gc) return 0;   // not tried yet, and loadIcon needs the controller
    uint32_t icon = 0;
    const bool real = !g_ops_set;
    std::string why;
    if (real && !available(Prim::ability_iconfile, &why)) {
        KWARN("abilities: ability %u's icon_file %s is not drawn: %s (the vanilla icon is used)", (unsigned)number, st->icon_file.c_str(), why.c_str());
    } else if (!(real ? real_exists(st->icon_file) : g_ops.exists(st->icon_file))) {
        KWARN("abilities: ability %u's icon_file %s is not in data3.db (put it in the mod's assets/icons/): the vanilla icon is used",
              (unsigned)number, st->icon_file.c_str());
    } else {
        icon = real ? real_load(gc, st->icon_file) : g_ops.load(gc, st->icon_file);
        if (icon) KINFO("abilities: ability %u draws its own icon %s (object 0x%X)", (unsigned)number, st->icon_file.c_str(), (unsigned)icon);
        else KWARN("abilities: loadIcon returned no icon for ability %u's %s: the vanilla icon is used", (unsigned)number, st->icon_file.c_str());
    }
    g_cache[number] = icon;
    return icon;
}

void set_icon_file_ops_for_test(const IconFileOps* ops) {
    g_cache.clear();
    g_ops_set = ops != nullptr;
    if (ops) g_ops = *ops;
}

}  // namespace kyubu::game
