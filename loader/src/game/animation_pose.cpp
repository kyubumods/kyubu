// The pose provider (design D4). The detour runs once per drawn creature, several hundred times a second: a vanilla mode of another
// creature pays two compares, no lock, no log. Reading the clip set takes no lock: a draw that may wear a clip (the local player, or a mod
// mode) takes a Hold on it, two atomic operations. A mod mode also takes the Registries mutex (name_of) and the binding's (bound_clip);
// a preview draw takes the preview's mutex. The join site's mid hook runs in every draw too: a draw with no tracks pays one pointer compare.
#include "animation_pose.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>

#include "abilities.h"
#include "ability_bar.h"
#include "animations.h"
#include "cubeworld/types.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "registry.h"
#include "symbols.h"

namespace kyubu::game {

namespace {

constexpr const char* kDrawCreature = "draw_creature";
constexpr const char* kJoinSite = "draw_creature_join_site";
constexpr const char* kController = "cube::game_controller";
constexpr uintptr_t kRecordOffset = 0x1498;   // the draw record sits at Creature + 0x1498

// The draw record's head: +0 the creature, +4 the mode time in ms, +8 the pose id.
struct DrawRecord {
    cube_Creature* creature;
    uint32_t time_ms;
    uint32_t pose_id;
};

// void __thiscall draw_creature(record, arg1, arg2, arg3, models, frame_ms, arg6, arg7, arg8, hide_mask, arg10), `ret 0x28`.
// __fastcall with an unused EDX has the thiscall ABI.
using DrawFn = void(__fastcall*)(DrawRecord*, void*, void*, void*, void*, void*, uint32_t, void*, void*, void*, uint32_t, void*);

// Where a part's vectors sit in the record: three floats each; -1 for a channel with no vector. In anim::Part order.
// `model` is the part's model slot in the record (probe pose-axes): a pointer, 0 when the game draws no model for the
// part. -1 for the root, which every creature has. Both hands share one slot.
struct PartSlot {
    int pos;       // the position's offset
    int pos_n;     // how many of its floats the game reads (the root's z is dropped)
    int rot;       // the euler's offset
    int model;     // the model slot's offset
};
constexpr PartSlot kSlots[anim::kPartCount] = {
    {0xAC, 2, 0x60, -1},   // root
    {0x6C, 3, 0x54, 0xC4}, // body
    {0x78, 3, 0x84, 0xBC}, // head
    {0x18, 3, 0x30, 0xCC}, // hand_r: the game's hand B
    {0x0C, 3, 0x24, 0xCC}, // hand_l: the game's hand A
    {-1, 0, 0x90, 0xD4},   // tail
};

void get_floats(const void* record, int offset, int n, float* out) { memcpy(out, (const uint8_t*)record + offset, n * sizeof(float)); }
void put_floats(void* record, int offset, int n, const float* in) { memcpy((uint8_t*)record + offset, in, n * sizeof(float)); }
bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// The parts the game hides with argument 9 of draw_creature (mapping atlas creatures, "Posing"): head 1 and body 8. The game clears their model slots.
uint32_t hidden_parts(uint32_t hide_mask) {
    uint32_t parts = 0;
    if (hide_mask & 0x1) parts |= 1u << static_cast<int>(anim::Part::Head);
    if (hide_mask & 0x8) parts |= 1u << static_cast<int>(anim::Part::Body);
    return parts;
}

// The parts with a model slot of 0 in this record: the game draws no model for them. A hidden part is not lacking.
uint32_t lacking_parts(const void* record, uint32_t hidden) {
    uint32_t lacked = 0;
    for (int i = 0; i < anim::kPartCount; ++i) {
        if (kSlots[i].model < 0 || (hidden & (1u << i))) continue;
        uint32_t slot = 0;
        memcpy(&slot, (const uint8_t*)record + kSlots[i].model, sizeof slot);
        if (slot == 0) lacked |= 1u << i;
    }
    return lacked;
}

// The draw the detour is in, for the join site: whose record it is and which tracks to write. Render thread only.
struct Pending {
    DrawRecord* rec = nullptr;
    const anim::Clip* clip = nullptr;
    anim::Timing timing;
    PoseSave save;
    bool written = false;
    uint32_t mode = 0;      // the creature's mode, the key of the log line
    uint32_t hidden = 0;    // the parts the game hides in this draw (part bits)
};

DrawFn g_next = nullptr;
KyubuId g_hook = 0;
KyubuId g_site_hook = 0;
Pending* g_pending = nullptr;
std::string g_tracks_why;        // why the join site is not hooked; set at install
int g_tracks_forced = -1;        // tests: force_pose_tracks_for_test
std::string g_tracks_forced_why;
uintptr_t g_controller_global = 0;   // the address of the game controller pointer, found once at install

// A line logged once per key, never per draw. The key names what the line is about: "mode 118", or the preview's clip id and text hash, so
// a clip redefined through the editor (a new hash) logs again. The mutex is taken only when a warning condition holds, never on a vanilla draw.
std::mutex g_once_mu;
std::set<std::string> g_once;

bool first_time(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_once_mu);
    return g_once.insert(key).second;
}

void warn_once(const std::string& key, const std::string& text) {
    if (first_time("warn " + key)) KWARN("animation: %s", text.c_str());
}

// The same for a clip whose tracks cannot be drawn: its own kind of line, so a borrow warning of the same key does not hide it.
void refuse_tracks_once(const std::string& key, const std::string& text) {
    if (first_time("refuse " + key)) KERROR("animation: %s", text.c_str());
}

// A throw out of the provider. Its own flags, so a warning of the same mode, or of the preview, does not hide it.
std::atomic<bool> g_failed[257];

void fail_once(uint32_t key, const std::string& text) {
    if (key > 256) key = 256;
    if (!g_failed[key].exchange(true)) KWARN("animation: %s", text.c_str());
}

// A track on a part the creature has no model for is skipped, and logged once per mode and part (one bit each).
std::atomic<uint8_t> g_lacking_logged[257];

void note_lacking(uint32_t key, uint32_t lacked) {
    if (key > 256) key = 256;
    for (int i = 0; i < anim::kPartCount; ++i) {
        if (!(lacked & (1u << i))) continue;
        if (first_lacking_part(key, static_cast<anim::Part>(i)))
            KWARN("animation: mode %u animates %s, which this creature does not draw: the track is skipped", key, anim::part_name(static_cast<anim::Part>(i)));
    }
}

// The join site, inside the original draw_creature: EDI is the draw record. Every drawn creature passes here.
void on_join_site(KyubuCpuContext* ctx) {
    Pending* p = g_pending;
    if (!p || (uintptr_t)p->rec != ctx->edi || p->written) return;
    try {
        uint32_t lacking = 0;
        p->written = apply_tracks(p->rec, *p->clip, p->timing, &p->save, &lacking, p->hidden);
        note_lacking(p->mode, lacking);
    } catch (const std::exception& e) {
        fail_once(p->mode, std::string("a clip's tracks failed, the game's own pose is drawn: ") + e.what());
    } catch (...) {
        fail_once(p->mode, "a clip's tracks failed, the game's own pose is drawn");
    }
}

const cube_Creature* local_player() {
    const cube_GameController* gc = g_controller_global ? *(cube_GameController* const*)g_controller_global : nullptr;
    return gc ? gc->player : nullptr;
}

// The draws that may wear a clip: the local player (the preview clock is read in plan_draw, after the hold is taken) and a mod mode.
// Only a vanilla mode of another creature skips the hold.
bool provider_may_wear(const DrawFacts& f) {
    if (f.is_local) return true;
    return f.mode >= kProviderFirstMode && f.mode <= kAbilityNumberMax;
}

void __fastcall draw_detour(DrawRecord* rec, void* edx, void* a1, void* a2, void* a3, void* models, uint32_t frame_ms, void* a6, void* a7,
                            void* a8, uint32_t hide_mask, void* a10) {
    BorrowPlan plan;
    bool wear = false;
    // The clip set stays alive for the whole draw: the join site reads the clip after g_next, and restore_pose runs after it. Only a draw
    // that may wear a clip takes the hold, so a vanilla draw of another creature pays nothing.
    std::optional<anim::ClipRegistry::Hold> hold;
    try {
        // A record that is not at Creature + 0x1498 of its creature is not one this provider knows: the game's own draw.
        if (rec && rec->creature && (uintptr_t)rec - (uintptr_t)rec->creature == kRecordOffset) {
            DrawFacts f;
            f.mode = rec->creature->data.current_mode;
            f.record_time_ms = rec->time_ms;
            f.is_local = rec->creature == local_player();
            f.creature_id = (uint64_t)rec->creature->id;
            if (provider_may_wear(f)) {
                hold.emplace(Animations::get().clips());
                wear = plan_draw(f, &plan);
                // The signals are read only for a clip whose `expr` tracks name one: a keys clip pays nothing.
                if (wear && plan.tracks &&
                    (anim::clip_expr_uses(*plan.tracks) & (anim::kUsesCharge | anim::kUsesSpeed | anim::kUsesRank)))
                    plan.timing.signals = creature_signals(rec->creature->data, f.is_local, f.mode);
            }
        }
    } catch (const std::exception& e) {
        wear = false;
        fail_once(256, std::string("the pose provider failed, the game's own pose is drawn: ") + e.what());
    } catch (...) {
        wear = false;
        fail_once(256, "the pose provider failed, the game's own pose is drawn");
    }
    if (!wear) {
        g_next(rec, edx, a1, a2, a3, models, frame_ms, a6, a7, a8, hide_mask, a10);
        return;
    }
    const uint32_t time = rec->time_ms, pose = rec->pose_id;
    rec->time_ms = plan.time_ms;
    rec->pose_id = plan.pose_id;
    Pending pending;
    Pending* const outer = g_pending;
    if (plan.tracks) {
        pending.rec = rec;
        pending.clip = plan.tracks;
        pending.timing = plan.timing;
        pending.mode = rec->creature->data.current_mode;
        pending.hidden = hidden_parts(hide_mask | plan.hide_bits);
        g_pending = &pending;
    }
    g_next(rec, edx, a1, a2, a3, models, frame_ms, a6, a7, a8, hide_mask | plan.hide_bits, a10);
    g_pending = outer;
    // +4 and +8 go back (`render` writes both again before the next draw), and so do the vectors the tracks wrote over: the rest of the
    // record is the game's smoothed state, which must ease on from its own values.
    if (pending.written) restore_pose(rec, pending.save);
    rec->time_ms = time;
    rec->pose_id = pose;
}

// The entry is valid while the draw's hold (draw_detour) lives: plan_draw and the join site use it within that draw.
const anim::ClipEntry* find_clip(const std::string& id) { return Animations::get().clips().set().find(id); }

// The plan of one clip. `key` is where its problems are logged once ("mode 118", or the preview's clip id and text hash) and `label` names it in the log.
bool plan_clip(const anim::Clip& clip, const std::string& label, const std::string& key, float elapsed_ms, float ability_duration_ms,
               uint32_t record_time_ms, BorrowPlan* out) {
    bool tracks = !clip.tracks.empty();
    if (tracks) {
        std::string why;
        if (!pose_tracks_on(&why)) {
            tracks = false;
            refuse_tracks_once(key, label + " has tracks, and tracks are not drawn: " + why +
                                        (clip.borrow.present ? " (its borrowed pose is drawn)" : " (the game's own pose is drawn)"));
        }
    }
    BorrowPlan p;
    anim::Timing t;
    t.elapsed_ms = elapsed_ms > 0 ? elapsed_ms : 0;
    t.duration_ms = anim::resolve_duration(clip, ability_duration_ms);
    t.run_ms = ability_duration_ms;
    // `duration = "ability"` with no ability's duration (the preview clock): no clip time, so apply_clip writes nothing. The tracks are
    // dropped here with one line, with a borrow too (the borrowed pose is still drawn).
    if (tracks && t.duration_ms <= 0) {
        warn_once(key, label + " has no duration here (duration = \"ability\" and no ability's duration): its tracks are not drawn" +
                           (clip.borrow.present ? " (its borrowed pose is drawn)" : " (the game's own pose is drawn)"));
        tracks = false;
    }
    if (clip.borrow.present) {
        if (!plan_borrow(clip, elapsed_ms, ability_duration_ms, &p)) {
            warn_once(key, label + " borrows pose " + std::to_string(clip.borrow.mode) + ", and pose ids go from 0 to " +
                               std::to_string(kMaxPoseId) + ": the game's own pose is drawn");
            return false;
        }
    } else {
        if (!tracks) return false;   // nothing to wear
        p.pose_id = 0;               // "none": the game builds its neutral pose, the base of the tracks
        p.time_ms = record_time_ms;
        p.hide_bits = game_hide_bits(anim::hide_mask(clip, t));
    }
    if (tracks) {
        p.tracks = &clip;
        p.timing = t;
    }
    *out = p;
    return true;
}

}  // namespace

anim::Signals creature_signals(const cube_CreatureData& data, bool is_local, uint32_t mode) {
    anim::Signals s;
    s.charge = std::isfinite(data.charged_mp) && data.charged_mp > 0 ? data.charged_mp : 0.0f;
    const double vx = data.velocity.x, vy = data.velocity.y;
    const double v = std::sqrt(vx * vx + vy * vy);
    s.speed = std::isfinite(v) ? static_cast<float>(v) : 0.0f;
    s.rank = 1;
    if (is_local && mode >= kProviderFirstMode && mode <= kAbilityNumberMax) {
        const uint32_t r = ability_rank_of(mode);
        s.rank = static_cast<float>(r);
    }
    return s;
}

uint32_t game_hide_bits(uint32_t groups) {
    struct Map { uint32_t group; uint32_t game; };
    static const Map map[] = {{anim::kHideHead, 0x01}, {anim::kHideHair, 0x02}, {anim::kHideBody, 0x08},
                              {anim::kHideShoulders, 0x04}, {anim::kHideWeapons, 0x40}};
    uint32_t out = 0;
    for (const Map& m : map)
        if (groups & m.group) out |= m.game;
    return out;
}

bool plan_borrow(const anim::Clip& clip, float elapsed_ms, float ability_duration_ms, BorrowPlan* out) {
    if (!clip.borrow.present || clip.borrow.mode > kMaxPoseId) return false;
    anim::Timing t;
    t.elapsed_ms = elapsed_ms > 0 ? elapsed_ms : 0;
    t.duration_ms = anim::resolve_duration(clip, ability_duration_ms);
    const float time = anim::borrowed_mode_time(clip, elapsed_ms);
    out->pose_id = clip.borrow.mode;
    out->time_ms = time >= 2147483647.0f ? 2147483647u : (uint32_t)(time + 0.5f);   // the record's clock is read as a signed int
    out->hide_bits = game_hide_bits(anim::hide_mask(clip, t));
    return true;
}

bool plan_draw(const DrawFacts& facts, BorrowPlan* out) {
    Animations& anims = Animations::get();
    if (facts.is_local && anims.preview_on()) {
        Animations::Preview p;
        if (!anims.preview(&p)) return false;
        const anim::ClipEntry* e = find_clip(p.clip_id);
        if (!e) return false;   // a clip that went away: the game's pose
        return plan_clip(e->clip, "preview clip " + p.clip_id, "preview " + p.clip_id + " " + e->text_hash, p.t_ms, 0, facts.record_time_ms, out);
    }
    if (facts.mode < kProviderFirstMode || facts.mode > kAbilityNumberMax) return false;
    std::string ability, clip_id;
    if (Registries::get().name_of("kyubu:abilities", facts.mode, &ability) != KY_OK) return false;
    if (!anims.bound_clip(ability, facts.creature_id, &clip_id)) return false;
    const anim::ClipEntry* e = find_clip(clip_id);
    if (!e) {
        warn_once("mode " + std::to_string(facts.mode), "clip " + clip_id + " of " + ability + " is gone: the game's own pose is drawn");
        return false;
    }
    const AbilityStat* stat = ability_stat(facts.mode);
    return plan_clip(e->clip, "clip " + clip_id + " of " + ability, "mode " + std::to_string(facts.mode), (float)facts.record_time_ms,
                     stat ? (float)stat->duration_ms : 0,
                     facts.record_time_ms, out);
}

void read_pose(const void* record, anim::Pose* out) {
    *out = anim::Pose();
    for (int i = 0; i < anim::kPartCount; ++i) {
        if (kSlots[i].pos >= 0) get_floats(record, kSlots[i].pos, kSlots[i].pos_n, out->parts[i].pos);
        if (kSlots[i].rot >= 0) get_floats(record, kSlots[i].rot, 3, out->parts[i].rot);
    }
}

bool first_lacking_part(uint32_t key, anim::Part part) {
    if (key > 256) key = 256;
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<int>(part));
    return !(g_lacking_logged[key].fetch_or(bit) & bit);
}

bool apply_tracks(void* record, const anim::Clip& clip, const anim::Timing& timing, PoseSave* save, uint32_t* lacking, uint32_t hidden) {
    PoseSave s;
    read_pose(record, &s.base);
    anim::Pose pose = s.base;
    const anim::Applied a = anim::apply_clip(clip, timing, pose);
    const uint32_t lacked = lacking_parts(record, hidden);
    // The parts the clip animates and the creature has no model for: reported even when nothing is written.
    if (lacking) *lacking = (a.pos_parts | a.rot_parts) & lacked;
    if (!a.ok) return false;
    for (int i = 0; i < anim::kPartCount; ++i) {
        const uint32_t bit = 1u << i;
        if (lacked & bit) continue;   // the track of a part with no model is skipped; the rest of the clip applies
        // A value that is not a number is never given to the game: that channel keeps the game's value.
        if ((a.pos_parts & bit) && kSlots[i].pos >= 0 && finite3(pose.parts[i].pos)) s.pos_parts |= bit;
        if ((a.rot_parts & bit) && kSlots[i].rot >= 0 && finite3(pose.parts[i].rot)) s.rot_parts |= bit;
    }
    if (!s.pos_parts && !s.rot_parts) return false;
    for (int i = 0; i < anim::kPartCount; ++i) {
        const uint32_t bit = 1u << i;
        if (s.pos_parts & bit) put_floats(record, kSlots[i].pos, kSlots[i].pos_n, pose.parts[i].pos);
        if (s.rot_parts & bit) put_floats(record, kSlots[i].rot, 3, pose.parts[i].rot);
    }
    *save = s;
    return true;
}

void restore_pose(void* record, const PoseSave& save) {
    for (int i = 0; i < anim::kPartCount; ++i) {
        const uint32_t bit = 1u << i;
        if (save.pos_parts & bit) put_floats(record, kSlots[i].pos, kSlots[i].pos_n, save.base.parts[i].pos);
        if (save.rot_parts & bit) put_floats(record, kSlots[i].rot, 3, save.base.parts[i].rot);
    }
}

bool pose_tracks_on(std::string* why) {
    if (g_tracks_forced >= 0) {
        if (!g_tracks_forced && why) *why = g_tracks_forced_why;
        return g_tracks_forced != 0;
    }
    if (g_site_hook) return true;
    if (why) *why = g_tracks_why.empty() ? std::string("the join site ") + kJoinSite + " is not hooked" : g_tracks_why;
    return false;
}

void force_pose_tracks_for_test(int state, const char* why) {
    g_tracks_forced = state;
    g_tracks_forced_why = why ? why : "";
    std::lock_guard<std::mutex> lock(g_once_mu);
    for (auto it = g_once.begin(); it != g_once.end();) it = it->rfind("refuse ", 0) == 0 ? g_once.erase(it) : std::next(it);
}

void forget_logs_for_test() {
    {
        std::lock_guard<std::mutex> lock(g_once_mu);
        g_once.clear();
    }
    for (auto& f : g_failed) f = false;
    for (auto& f : g_lacking_logged) f = 0;
}

void install_animation_pose_hook() {
    if (g_hook) return;
    std::string why;
    if (!available(Prim::animation_pose, &why)) {
        KINFO("animation: the pose provider is off: %s; mod abilities keep the look the game gives them", why.c_str());
        return;
    }
    const uintptr_t draw = verified_symbol(kDrawCreature, &why);
    const uintptr_t controller = draw ? verified_symbol(kController, &why) : 0;
    if (!draw || !controller) {
        KERROR("animation: the pose provider is off: %s", why.c_str());
        return;
    }
    g_controller_global = controller;
    std::string err;
    if (Hooks::get().hook_function("kyubu", draw, (void*)&draw_detour, kLoaderPriority, (void**)&g_next, &g_hook, &err) != KY_OK) {
        g_hook = 0;
        g_next = nullptr;
        g_controller_global = 0;
        KERROR("animation: the pose provider is off: cannot detour %s: %s", kDrawCreature, err.c_str());
        return;
    }
    // The join site of phase 2. A refusal (KY_E_UNSAFE: no safe site) leaves the borrowed look working and turns tracks off.
    const uintptr_t site = verified_symbol(kJoinSite, &why);
    err = why;
    if (!site || Hooks::get().hook_mid("kyubu", site, KY_SIDE_CLIENT, on_join_site, &g_site_hook, &err) != KY_OK) {
        g_site_hook = 0;
        g_tracks_why = std::string("cannot hook ") + kJoinSite + ": " + err;
        KERROR("animation: tracks are off: %s; a clip with tracks is not drawn, borrowed looks still work", g_tracks_why.c_str());
        KINFO("animation: the pose provider is installed (%s, borrowed looks)", kDrawCreature);
        return;
    }
    g_tracks_why.clear();
    KINFO("animation: the pose provider is installed (%s, borrowed looks; %s, tracks)", kDrawCreature, kJoinSite);
}

void remove_animation_pose_hook() {
    if (g_site_hook) Hooks::get().remove("kyubu", g_site_hook);
    if (g_hook) Hooks::get().remove("kyubu", g_hook);
    g_site_hook = 0;
    g_tracks_why.clear();
    g_hook = 0;
    g_next = nullptr;
    g_controller_global = 0;
}

bool animation_pose_hook_installed() { return g_hook != 0; }

}  // namespace kyubu::game
