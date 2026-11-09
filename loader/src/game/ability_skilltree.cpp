// Learning the fourth ability in the skill tree (design D13).
#include "ability_skilltree.h"

#include <algorithm>
#include <atomic>

#include "abilities.h"
#include "ability_bar.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "loader.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

constexpr const char* kSite = "cube::GameController::skillButton9HideSite";
constexpr const char* kController = "cube::game_controller";
constexpr uint32_t kButtonNodeOffset = 0x3C;   // a button object's node
constexpr uint32_t kNodeIndexOffset = 0x68;    // the node's index into its flag array
constexpr uint32_t kNodeFlagsOffset = 0x94;    // the node's flag array (4-byte flags)
constexpr uint32_t kFlagIndexMax = 4096;       // a bound on the index read from the node
constexpr uint32_t kWidgetRanksOffset = 0x160; // the SkillWidget's working copy of the 11 ranks (GameController.skill_widget)
constexpr uint32_t kWidgetSpecOffset = 0x18C;  // the SkillWidget's working specialization (int32): the one the open panel shows

KyubuId g_hook = 0;
uintptr_t g_controller_global = 0;
cube_Creature* g_test_player = nullptr;
std::atomic<bool> g_logged_icon{false};
const std_map* g_icon_map = nullptr;   // the skill_icons map Kyubu last wrote key 9 of, and the value it wrote
uint32_t g_icon_written = 0;

cube_GameController* controller() {
    return g_controller_global ? *(cube_GameController* const*)g_controller_global : nullptr;
}

const cube_Creature* local_player() {
    if (g_test_player) return g_test_player;
    const cube_GameController* gc = controller();
    return gc ? gc->player : nullptr;
}

// At the site [esp+0x10] is the game controller (`mov edi,[esp+0x10]` follows the hide call).
void on_hide_site(KyubuCpuContext* c) {
    auto* gc = *(cube_GameController**)(uintptr_t)(c->esp + 0x10);
    if (gc) skilltree_step(gc);
}

}  // namespace

int skilltree_shown_specialization(const cube_GameController* gc) {
    const cube_Creature* player = gc ? gc->player : nullptr;
    const int own = player ? player->data.specialization : 0;
    if (!gc || !gc->skills_open || !gc->skill_widget) return own;
    const int32_t shown = *(const int32_t*)((uintptr_t)gc->skill_widget + kWidgetSpecOffset);
    return (shown == 0 || shown == 1) ? shown : own;   // the game has two specializations per class
}

bool skilltree_learnable(int class_type, int specialization, uint32_t* number) {
    const AbilitySlotHolder h = ability_slot_holder(class_type, specialization);
    if (h.id.empty() || h.placeholder || h.number <= kAbilityPlaceholderNumber) return false;
    if (!ability_stat(h.number)) return false;
    if (number) *number = h.number;
    return true;
}

int32_t* skill_button_flag(const cube_GameController* gc, uint32_t index) {
    const std_vector* v = &gc->skill_buttons;
    const size_t n = ((uintptr_t)v->last - (uintptr_t)v->first) / sizeof(uint32_t);
    if (!v->first || index >= n) return nullptr;
    const uintptr_t button = ((const uintptr_t*)v->first)[index];
    const uintptr_t node = button ? *(const uintptr_t*)(button + kButtonNodeOffset) : 0;
    if (!node) return nullptr;
    const uint32_t at = *(const uint32_t*)(node + kNodeIndexOffset);
    const uintptr_t flags = *(const uintptr_t*)(node + kNodeFlagsOffset);
    if (!flags || at >= kFlagIndexMax) return nullptr;
    return (int32_t*)(flags + sizeof(int32_t) * at);
}

TreeStep skilltree_step(cube_GameController* gc) {
    if (!gc->skills_open) return TreeStep::PanelClosed;
    const cube_Creature* player = gc->player;
    if (!player) return TreeStep::NoPlayer;
    uint32_t number = 0;
    if (!skilltree_learnable(player->data.class_type, skilltree_shown_specialization(gc), &number)) return TreeStep::NotLearnable;
    int32_t* flag = skill_button_flag(gc, kSkillAbility4);
    if (!flag) return TreeStep::NoButton;
    *flag = 1;
    const AbilityStat* st = ability_stat(number);
    // The click handler adds a rank to any button that has its prerequisite, with no ceiling (a 3-rank ability could reach
    // rank 12, with a tooltip that did not change). The working copy of button 9 is held at the ability's max_rank every
    // frame, so the points over it are never spent.
    if (st && gc->skill_widget) {
        int32_t* rank = (int32_t*)((uintptr_t)gc->skill_widget + kWidgetRanksOffset) + kSkillAbility4;
        if (*rank > (int32_t)st->max_rank) *rank = (int32_t)st->max_rank;
    }
    // The panel's look-up of key 9 makes a null node on its first frame; the next frame finds it. The icon follows the
    // specialization the panel shows: Kyubu's own earlier value is replaced, a value it did not write is left alone.
    if (st && (st->icon || !st->icon_file.empty())) {
        const uint32_t prev = g_icon_map == &gc->skill_icons ? g_icon_written : 0;
        const uint32_t want = ability_icon_value(gc, number);
        const IconFill r = icon_swap(&gc->skill_icons, kSkillAbility4, want, prev);
        if (r == IconFill::Written) {
            g_icon_map = &gc->skill_icons;
            g_icon_written = want;
        }
        if (r == IconFill::Written) KINFO("abilities: skill button 9 shows the icon of ability %u (%s)", (unsigned)number, st->icon_file.empty() ? "a vanilla one" : st->icon_file.c_str());
        else if ((r == IconFill::NoSource || r == IconFill::NotATree) && !g_logged_icon.exchange(true))
            KWARN("abilities: skill button 9 has no icon (ability %u has no icon the panel can show, or a map is not a tree) (logged once)", (unsigned)number);
    }
    return TreeStep::Shown;
}

bool skilltree_active() { return g_hook != 0 || g_test_player != nullptr; }

bool skilltree_rank_get(uint32_t number, uint32_t* rank) {
    if (!skilltree_active()) return false;
    const cube_Creature* p = local_player();
    if (!p) return false;
    uint32_t holder = 0;
    if (!skilltree_learnable(p->data.class_type, p->data.specialization, &holder) || holder != number) return false;
    const AbilityStat* st = ability_stat(number);
    if (rank) *rank = std::min<uint32_t>(p->data.skills[kSkillAbility4], st ? st->max_rank : 1);
    return true;
}

bool skilltree_rank_set(uint32_t number, uint32_t rank, uint32_t* stored) {
    if (!skilltree_active()) return false;
    cube_Creature* p = const_cast<cube_Creature*>(local_player());
    if (!p) return false;
    uint32_t holder = 0;
    if (!skilltree_learnable(p->data.class_type, p->data.specialization, &holder) || holder != number) return false;
    const AbilityStat* st = ability_stat(number);
    const uint32_t v = std::min<uint32_t>(rank, st ? st->max_rank : 1);
    p->data.skills[kSkillAbility4] = v;
    if (stored) *stored = v;
    return true;
}

void install_ability_skilltree_hook() {
    if (g_hook) return;
    std::string why;
    if (L().host.host == KY_HOST_SERVER) return;   // Server.exe has no skill tree
    if (!ability_slot_available(&why)) return;   // game.cpp's check_entries has logged why
    if (!L().config.ability_skill_tree) {
        KINFO("abilities: the skill tree does not teach the fourth ability ([abilities] skill_tree is false): ranks come from the ranks file");
        return;
    }
    if (!available(Prim::ability_skilltree, &why)) return;
    const uintptr_t site = verified_symbol(kSite, &why);
    const uintptr_t controller_global = site ? verified_symbol(kController, &why) : 0;
    std::string err = why;
    if (!site || !controller_global ||
        Hooks::get().hook_mid("kyubu", site, KY_SIDE_CLIENT, on_hide_site, &g_hook, &err) != KY_OK) {
        g_hook = 0;
        KERROR("abilities: the skill tree cannot teach the fourth ability: cannot hook %s: %s (the ranks file is used)", kSite, err.c_str());
        return;
    }
    g_controller_global = controller_global;
    KINFO("abilities: the skill tree hook is installed at %s", kSite);
}

void remove_ability_skilltree_hook() {
    if (g_hook) Hooks::get().remove("kyubu", g_hook);
    g_hook = 0;
    g_controller_global = 0;
}

void set_skilltree_player_for_test(cube_Creature* player) {
    g_test_player = player;
    g_icon_map = nullptr;
    g_icon_written = 0;
}

}  // namespace kyubu::game
