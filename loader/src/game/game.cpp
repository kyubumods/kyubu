// The game API's base: the entry list (entries.inc) checked against the running build at boot, game_available and
// game_enum.
#include "game.h"

#include "ability_bar.h"
#include "ability_rank_bind.h"
#include "ability_skilltree.h"
#include "ability_stats.h"
#include "animation_pose.h"
#include "ability_step.h"
#include "cubeworld/layout.h"
#include "handles.h"
#include "creature_body.h"
#include "creature_flags.h"
#include "lights.h"
#include "loader.h"
#include "log.h"
#include "symbols.h"
#include "world.h"
#include "world_step.h"
#include "net_transport.h"
#include "shots.h"
#include "server_hits.h"

namespace kyubu::game {

namespace {

enum class Kind { field, type, global, call, detour, site };

// The builds an entry is needed on, as a mask of KyubuHost values.
constexpr unsigned kBuild_CLIENT = 1u << KY_HOST_CLIENT;
constexpr unsigned kBuild_SERVER = 1u << KY_HOST_SERVER;
constexpr unsigned kBuild_BOTH = kBuild_CLIENT | kBuild_SERVER;

struct Entry {
    Prim prim;
    const char* name;
    Kind kind;
    unsigned builds;
};

const Entry kEntries[] = {
#define GAME_ENTRY(primitive, entry, kind, builds) {Prim::primitive, entry, Kind::kind, kBuild_##builds},
#include "entries.inc"
};

// The primitives a GAME_BUILDS line limits; any other primitive is for both builds.
struct BuildLimit {
    Prim prim;
    unsigned builds;
};
const BuildLimit kBuildLimits[] = {
#define GAME_BUILDS(primitive, builds) {Prim::primitive, kBuild_##builds},
#include "entries.inc"
    {Prim::count, 0}   // keeps the array non-empty when entries.inc has no GAME_BUILDS line
};

// Tests: a limit that stands in for a GAME_BUILDS line (0 = none).
unsigned g_limit_override[(int)Prim::count];
struct Forced { int state = -1; };
Forced g_forced[(int)Prim::count];   // see force_primitive_for_test (-1: not forced)

unsigned prim_builds(Prim p) {
    if (g_limit_override[(int)p]) return g_limit_override[(int)p];
    for (auto& l : kBuildLimits)
        if (l.prim == p) return l.builds;
    return kBuild_BOTH;
}

const char* const kPrimNames[] = {
#define GAME_PRIMITIVE(id, section) #id,
#include "entries.inc"
};

// The primitives the fourth ability slot needs (the spec's "All or nothing"). ability_stun and ability_ranks are the
// other two `ability_` primitives: they fall back instead of switching the slot off.
constexpr Prim kAbilitySlotSet[] = {Prim::ability_state, Prim::ability_step, Prim::ability_bar, Prim::ability_stats,
                                    Prim::ability_icons};

// Server.exe has no bar, icons or input: its slot is the state, the step runner and the stats (add-skills-unleashed-multiplayer 3.2).
constexpr Prim kAbilityServerSet[] = {Prim::ability_state, Prim::ability_step, Prim::ability_stats};

bool is_ability_primitive(int p) { return strncmp(kPrimNames[p], "ability_", 8) == 0; }

struct PrimState {
    bool ok = false;
    std::string why = "the game API has not checked its entries yet";
};
PrimState g_prims[(int)Prim::count];

const CwLayoutType* layout_type(const std::string& name) {
    for (auto& t : cw_layout_types)
        if (name == t.name) return &t;
    return nullptr;
}

const CwLayoutEnum* layout_enum(const char* name) {
    for (auto& en : cw_layout_enums)
        if (*en.name && strcmp(name, en.name) == 0) return &en;
    return nullptr;
}

// The positions of an enum's values in value order (the header keeps the mapping's order), built once for all.
const std::vector<uint32_t>& value_order(const CwLayoutEnum* en) {
    static const std::vector<std::vector<uint32_t>> orders = [] {
        std::vector<std::vector<uint32_t>> all;
        for (auto& e : cw_layout_enums) {
            std::vector<uint32_t> o(e.value_count);
            for (uint32_t i = 0; i < e.value_count; ++i) o[i] = i;
            std::stable_sort(o.begin(), o.end(), [&](uint32_t a, uint32_t b) { return e.values[a].value < e.values[b].value; });
            all.push_back(std::move(o));
        }
        return all;
    }();
    return orders[en - cw_layout_enums];
}

}  // namespace

// Why a `type` entry can't be used: the struct or enum is not in the loader's compiled layouts, or is a candidate.
std::string type_problem(const std::string& name) {
    if (const CwLayoutType* t = layout_type(name)) return t->verified ? "" : "candidate in the loader's layouts";
    for (auto& en : cw_layout_enums)
        if (name == en.name) return en.verified ? "" : "candidate in the loader's layouts";
    return "not in the loader's layouts";
}

// A verified field's offset stands on its own: its struct may still be a candidate for its size or its other fields
// (cube::AppearanceData.scale, add-skills-unleashed-multiplayer 10.7). A `type` entry needs the struct verified.
std::string field_problem(const std::string& name) {
    auto dot = name.rfind('.');
    const CwLayoutType* t = dot == std::string::npos ? nullptr : layout_type(name.substr(0, dot));
    if (t)
        for (uint32_t i = 0; i < t->field_count; ++i)
            if (name.compare(dot + 1, std::string::npos, t->fields[i].name) == 0)
                return t->fields[i].verified ? "" : "candidate in the loader's layouts";
    return "not in the loader's layouts";
}

const char* enum_value_name(const char* enum_name, int64_t value) {
    const CwLayoutEnum* en = enum_name ? layout_enum(enum_name) : nullptr;
    if (!en || !en->verified) return nullptr;
    for (uint32_t i = 0; i < en->value_count; ++i)
        if (en->values[i].value == value) return en->values[i].name;
    return nullptr;
}

namespace {

// Why an entry can't be used on the running build; empty when it can. Fields and types come from the loader's
// compiled layouts (cubeworld/layout.h), everything else from the build's symbol map.
std::string problem(const Entry& e) {
    const std::string name = e.name;
    if (e.kind == Kind::field) return field_problem(name);
    if (e.kind == Kind::type) {
        return type_problem(name);
    }
    std::string why;
    if (!verified_symbol(e.name, &why)) return why;
    const Symbol* s = SymbolTable::get().find(e.name);
    if (s && s->global != (e.kind == Kind::global)) return s->global ? "a global, not a function" : "a function, not a global";
    return "";
}

void copy_why(const std::string& s, char* buf, uint32_t cap) {
    if (!buf || !cap) return;
    size_t n = std::min<size_t>(s.size(), cap - 1);
    memcpy(buf, s.data(), n);
    buf[n] = 0;
}

KyubuResult KYUBU_CALL k_game_available(const char* primitive, char* why, uint32_t cap) {
    if (!primitive) return KY_E_INVALID_ARG;
    for (int p = 0; p < (int)Prim::count; ++p) {
        if (strcmp(kPrimNames[p], primitive) != 0) continue;
        std::string w;
        bool ok = available((Prim)p, &w);
        copy_why(w, why, cap);
        return ok ? KY_OK : KY_E_UNAVAILABLE;
    }
    copy_why(std::string("no game-API primitive '") + primitive + "'", why, cap);
    return KY_E_NOT_FOUND;
}

// The index-th value (in value order) of a verified enum of the loader's layouts, and its name in buf. Only verified
// enums: for cube::EntityType these are the values creature_spawn accepts.
KyubuResult KYUBU_CALL k_game_enum(const char* name, uint32_t index, int64_t* value, char* buf, uint32_t cap) {
    if (!name) return KY_E_INVALID_ARG;
    const CwLayoutEnum* en = layout_enum(name);
    if (!en) {
        copy_why(std::string("no enum '") + name + "' in the loader's layouts", buf, cap);
        return KY_E_NOT_FOUND;
    }
    if (!en->verified) {
        copy_why(std::string(name) + " is candidate in the loader's layouts, not verified", buf, cap);
        return KY_E_UNAVAILABLE;
    }
    if (index >= en->value_count) {
        copy_why("", buf, cap);
        return KY_E_NOT_FOUND;
    }
    const CwLayoutValue& v = en->values[value_order(en)[index]];
    if (value) *value = v.value;
    return copy_out(v.name, buf, cap);
}

}  // namespace

const char* prim_name(Prim p) { return (int)p >= 0 && p < Prim::count ? kPrimNames[(int)p] : "?"; }

void check_entries() {
    const Loader& l = L();
    const unsigned build = 1u << l.host.host;
    for (auto& s : g_prims) s = PrimState{true, ""};
    for (int p = 0; p < (int)Prim::count; ++p)
        if (!(prim_builds((Prim)p) & build))
            g_prims[p] = PrimState{false, std::string("not mapped for ") + (l.host.host == KY_HOST_SERVER ? "Server.exe" : "Cube.exe") + " yet"};
    for (const Entry& e : kEntries) {
        PrimState& s = g_prims[(int)e.prim];
        if (!(e.builds & build) || !s.ok) continue;   // the first blocking entry names the reason
        std::string p = problem(e);
        if (!p.empty()) s = PrimState{false, std::string(e.name) + ": " + p + " (" + l.host.build_id + ")"};
    }
    const PrimState world = g_prims[(int)Prim::world];
    for (int p = 0; p < (int)Prim::count; ++p)
        if (p != (int)Prim::world && g_prims[p].ok && !world.ok) g_prims[p] = PrimState{false, "needs world, which is unavailable"};
    std::string ok_list;
    for (int p = 0; p < (int)Prim::count; ++p) {
        if (g_prims[p].ok) ok_list += (ok_list.empty() ? "" : ", ") + std::string(kPrimNames[p]);
        else if (!is_ability_primitive(p) && !(p == (int)Prim::animation_pose && (build & kBuild_SERVER))) KWARN("game API: %s is unavailable on this build: %s", kPrimNames[p], g_prims[p].why.c_str());
    }
    KINFO("game API: available on this build: %s", ok_list.empty() ? "none" : ok_list.c_str());
    // The ability primitives are summarised, so that a build without a skill bar (Server.exe) is not a warning, and the
    // log names the blocking entry of the slot once.
    std::string why;
    if (ability_slot_available(&why)) KINFO("game API: the fourth ability slot is available");
    else if (build & kBuild_SERVER) KINFO("game API: the fourth ability slot is off on Server.exe: %s", why.c_str());
    else KWARN("game API: the fourth ability slot is off: %s", why.c_str());
    for (Prim p : {Prim::ability_stun, Prim::ability_ranks, Prim::ability_skilltree, Prim::ability_iconfile}) {
        if (g_prims[(int)p].ok) continue;
        const char* fallback = p == Prim::ability_stun ? "a stun ends an ability with interrupted"
                             : p == Prim::ability_ranks ? "every ability rank reads 1"
                             : p == Prim::ability_iconfile ? "a mod ability shows its vanilla icon, never its icon_file"
                                                        : "the skill tree does not show the fourth ability and ranks come from the ranks file";
        KINFO("game API: %s is unavailable on this build: %s (%s)", kPrimNames[(int)p], g_prims[(int)p].why.c_str(), fallback);
    }
}

void boot() {
    check_entries();
    install_world_hooks();    // only when world is available
    install_handle_hooks();   // only when handles is available
    install_ability_stat_hooks();   // only when the fourth slot is available
    install_ability_bar_hook();   // only when the fourth slot is available
    install_ability_step_hook();  // only when the fourth slot is available
    install_ability_rank_hooks(); // only when ability_ranks is available
    install_ability_skilltree_hook(); // only when the fourth slot and ability_skilltree are available
    install_animation_pose_hook();    // only when animation_pose is available; says why in the log when not
    install_creature_body_hooks();    // only when creature_body is available; says why in the log when not
    install_creature_flag_hooks();    // only when creature_flags is available; says why in the log when not
    install_light_hook();             // only when lights is available; says why in the log when not
    install_net_transport_hooks();    // the channel transport of a joined game (D10); says why in the log when off
    install_world_step_hooks();       // only when world_step is available; says why in the log when not
    install_shot_hooks();             // Server.exe, only when player_shots is available; says why in the log when not
    install_server_hit_hooks();       // Server.exe, only when server_hits is available; says why in the log when not
}

void limit_builds_for_test(Prim p, unsigned host_mask) {
    if ((int)p >= 0 && p < Prim::count) g_limit_override[(int)p] = host_mask;
}

void force_primitive_for_test(Prim p, int state) {
    if ((int)p >= 0 && p < Prim::count) g_forced[(int)p].state = state;
}

bool available(Prim p, std::string* why) {
    if ((int)p < 0 || p >= Prim::count) return false;
    if (g_forced[(int)p].state >= 0) {
        if (why) *why = g_forced[(int)p].state ? "" : "forced unavailable by a test";
        return g_forced[(int)p].state != 0;
    }
    const PrimState& s = g_prims[(int)p];
    if (why) *why = s.why;
    return s.ok;
}

bool ability_slot_available(std::string* why) {
    if (why) why->clear();
    const bool server = L().host.host == KY_HOST_SERVER;
    auto check = [&](Prim p) {
        std::string w;
        if (available(p, &w)) return true;
        if (why) *why = std::string(kPrimNames[(int)p]) + ": " + w;
        return false;
    };
    if (server) {
        for (Prim p : kAbilityServerSet)
            if (!check(p)) return false;
        return true;
    }
    for (Prim p : kAbilitySlotSet)
        if (!check(p)) return false;
    return true;
}

void fill_table(KyubuApi& a) {
    a.game_available = k_game_available;
    a.game_enum = k_game_enum;
    fill_entity_state(a);
    fill_spawn(a);
    fill_items(a);
    fill_ability_api(a);
    fill_animation_api(a);
    fill_ui(a);
    fill_particles(a);
    fill_lights(a);
    fill_creature_flags(a);
    fill_creature_body(a);
    fill_terrain(a);
    fill_world_step(a);
    fill_shots(a);
}

}  // namespace kyubu::game
