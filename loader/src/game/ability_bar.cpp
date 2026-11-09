// The push onto the skill bar (design D4).
#include "ability_bar.h"

#include <atomic>

#include "abilities.h"
#include "ability_iconfile.h"
#include "ability_rank_bind.h"
#include "ability_skilltree.h"
#include "game.h"
#include "hooks.h"
#include "int_map.h"
#include "loader.h"
#include "log.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

constexpr const char* kSite = "cube::GameController::abilityBarBuiltSite";
constexpr const char* kPushBack = "IntVector::push_back";

// void __thiscall IntVector::push_back(std::vector<int>* v, const int* value), `ret 4`: __fastcall with an unused EDX
// has the same ABI.
using PushBackFn = void(__fastcall*)(std_vector*, void*, const int*);
PushBackFn g_push_back = nullptr;
KyubuId g_hook = 0;
uint32_t (*g_rank_source)(uint32_t) = nullptr;
std::atomic<bool> g_logged_foreign{false};
std::atomic<bool> g_logged_no_icon{false};

void game_push(std_vector* bar, const int* value) { g_push_back(bar, nullptr, value); }

// The icon map is std::map<int, pointer>: the key at node+0x10, the 4-byte value at node+0x14 (int_map.h).
uint32_t* icon_slot(const std_map* m, int key, bool* bad) { return int_map_find(m, key, bad); }

// The hook runs on the game thread inside GameController::update, so the World's lock is the game's own. At the site
// [esp+0x10] is the game controller and [esp+0x34] the bar vector.
void on_bar_built(KyubuCpuContext* c) {
    auto* gc = *(cube_GameController**)(uintptr_t)(c->esp + 0x10);
    auto* bar = *(std_vector**)(uintptr_t)(c->esp + 0x34);
    if (!gc || bar != &gc->ability_bar) return;   // not the player's bar
    const cube_Creature* player = gc->player;
    if (!player) return;                          // no World or player yet: the bar stays as vanilla
    const BarFill bf = bar_fill(bar, player->data.class_type, player->data.specialization, game_push);
    if (bf != BarFill::Pushed) return;
    // The HUD's look-up of the new number (right after this site) makes its icon node on the first frame; the next frame
    // finds it.
    const uint32_t number = (uint32_t)((const int*)bar->last)[-1];
    const AbilityStat* st = ability_stat(number);
    if (!st || (!st->icon && st->icon_file.empty())) return;
    const IconFill r = ability_icon_fill(gc, &gc->ability_icons, number, number);
    if (r == IconFill::Written) KINFO("abilities: ability %u shows its icon (%s)", (unsigned)number, st->icon_file.empty() ? "a vanilla one" : st->icon_file.c_str());
    else if (r == IconFill::NoSource && !g_logged_no_icon.exchange(true))
        KWARN("abilities: ability %u has no icon the HUD can show (its icon names ability %u, which has none): the slot stays empty (logged once)",
              (unsigned)number, (unsigned)st->icon);
    else if (r == IconFill::NotATree && !g_logged_no_icon.exchange(true))
        KWARN("abilities: the icon map is not a tree: slot 4 has no icon (logged once)");
}

}  // namespace

IconFill icon_put(const std_map* map, uint32_t key, uint32_t value) {
    bool bad = false;
    uint32_t* dst = icon_slot(map, (int)key, &bad);
    if (!dst) return bad ? IconFill::NotATree : IconFill::NoNode;
    if (*dst) return IconFill::AlreadySet;
    if (!value) return IconFill::NoSource;
    *dst = value;
    return IconFill::Written;
}

IconFill icon_copy(const std_map* dst_map, uint32_t dst_key, const std_map* src_map, uint32_t src_key) {
    bool bad = false;
    uint32_t* dst = icon_slot(dst_map, (int)dst_key, &bad);
    if (!dst) return bad ? IconFill::NotATree : IconFill::NoNode;
    if (*dst) return IconFill::AlreadySet;
    const uint32_t* src = icon_slot(src_map, (int)src_key, &bad);
    if (!src || !*src) return bad ? IconFill::NotATree : IconFill::NoSource;
    *dst = *src;
    return IconFill::Written;
}

IconFill ability_icon_fill(cube_GameController* gc, const std_map* dst, uint32_t dst_key, uint32_t number) {
    const AbilityStat* st = ability_stat(number);
    if (!st) return IconFill::NoSource;
    if (const uint32_t own = ability_file_icon(gc, number)) {
        const IconFill r = icon_put(dst, dst_key, own);
        if (r != IconFill::NoSource) return r;
    }
    if (!st->icon) return IconFill::NoSource;
    return icon_copy(dst, dst_key, &gc->ability_icons, st->icon);
}

uint32_t ability_icon_value(cube_GameController* gc, uint32_t number) {
    const AbilityStat* st = ability_stat(number);
    if (!st) return 0;
    if (const uint32_t own = ability_file_icon(gc, number)) return own;
    if (!st->icon || !gc) return 0;
    bool bad = false;
    const uint32_t* src = icon_slot(&gc->ability_icons, (int)st->icon, &bad);
    return src ? *src : 0;
}

IconFill icon_swap(const std_map* map, uint32_t key, uint32_t value, uint32_t prev) {
    bool bad = false;
    uint32_t* dst = icon_slot(map, (int)key, &bad);
    if (!dst) return bad ? IconFill::NotATree : IconFill::NoNode;
    if (*dst && *dst != prev) return IconFill::AlreadySet;   // the game's own value, or another writer's
    if (!value) return IconFill::NoSource;
    if (*dst == value) return IconFill::AlreadySet;
    *dst = value;
    return IconFill::Written;
}

IconFill icon_fill(const std_map* icons, uint32_t number, uint32_t icon_number) {
    return icon_copy(icons, number, icons, icon_number);
}

uint32_t ability_rank_of(uint32_t number) {
    if (g_rank_source) return g_rank_source(number);
    uint32_t tree = 0;
    if (skilltree_rank_get(number, &tree)) return tree;   // the holder's rank is skills[9] (design D13)
    return ability_rank_stored(number);   // the ranks file: 1 while ability_ranks is unavailable
}

BarFill bar_fill(std_vector* bar, int class_type, int specialization, BarPushFn push) {
    const size_t n = ((uintptr_t)bar->last - (uintptr_t)bar->first) / sizeof(int);
    if (n != kVanillaBarSize) {
        if (!g_logged_foreign.exchange(true))
            KWARN("abilities: the skill bar holds %u entries, not vanilla's %u: slot 4 is left alone (logged once)", (unsigned)n,
                  (unsigned)kVanillaBarSize);
        return BarFill::NotVanillaBar;
    }
    const AbilitySlotHolder h = ability_slot_holder(class_type, specialization);
    if (h.id.empty() || h.number == 0) return BarFill::NoHolder;
    if (ability_rank_of(h.number) < 1) return BarFill::NotLearned;
    const int value = (int)h.number;
    push(bar, &value);
    return BarFill::Pushed;
}

void install_ability_bar_hook() {
    if (g_hook) return;
    std::string why;
    if (L().host.host == KY_HOST_SERVER) return;   // Server.exe has no bar
    if (!ability_slot_available(&why)) return;   // game.cpp's check_entries has logged why
    const uintptr_t site = verified_symbol(kSite, &why);
    const uintptr_t push_back = site ? verified_symbol(kPushBack, &why) : 0;
    std::string err = why;
    if (!site || !push_back ||
        Hooks::get().hook_mid("kyubu", site, KY_SIDE_CLIENT, on_bar_built, &g_hook, &err) != KY_OK) {
        g_hook = 0;
        KERROR("abilities: the fourth slot is off: cannot hook %s: %s", kSite, err.c_str());
        return;
    }
    g_push_back = (PushBackFn)push_back;
    KINFO("abilities: the skill bar push is installed at %s", kSite);
}

void remove_ability_bar_hook() {
    if (g_hook) Hooks::get().remove("kyubu", g_hook);
    g_hook = 0;
    g_push_back = nullptr;
}

void set_rank_source_for_test(uint32_t (*rank)(uint32_t number)) { g_rank_source = rank; }
void reset_bar_log_for_test() { g_logged_foreign = false; g_logged_no_icon = false; }

}  // namespace kyubu::game
