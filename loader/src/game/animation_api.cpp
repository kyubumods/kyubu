// The animation editor's calls: animation_define_from_text, animation_preview, animation_preview_clear (openspec
// add-ability-animation), and the binding ability_animation_bind; docs/game-api.md. They hold no game memory: the clip registry and the preview clock live in
// Animations, and the pose provider (4.2, 5.1) reads them at the draw. So there is no GameCall and no primitive of the call's own; the
// binding only asks `animation_pose` whether the provider's entries are verified.
#include <cmath>
#include <cstring>

#include "animations.h"
#include "api.h"
#include "call.h"
#include "game.h"
#include "loader.h"
#include "log.h"
#include "modmanager.h"
#include "registry.h"

namespace kyubu::game {

namespace {

// The checks every editor call starts with, in the order the spec names them: a mod handle, a mod that is not faulted, the
// client side, full trust, dev mode. Returns the mod, or null with `*r` set (and the refusal logged once).
Mod* editor_guard(KyubuModHandle self, const char* fn, KyubuResult* r) {
    clear_refusal();
    KyubuSide side = KY_SIDE_NONE;
    Mod* mod = ModManager::get().from_handle(self, &side);
    if (!mod || (side != KY_SIDE_CLIENT && side != KY_SIDE_SERVER)) {
        *r = KY_E_INVALID_ARG;
        return nullptr;
    }
    if (mod->state == ModState::Faulted) {
        *r = KY_E_FAULTED;
        return nullptr;
    }
    if (!api_side_allowed(fn, side)) {
        *r = note_refusal(mod, side, fn, KY_E_SIDE, std::string(fn) + " is client-only and was called from the server side");
        return nullptr;
    }
    if (mod->manifest.trust != "full") {
        *r = note_refusal(mod, side, fn, KY_E_TRUST, std::string(fn) + " needs trust \"full\"; this mod is \"" + mod->manifest.trust + "\"");
        return nullptr;
    }
    if (!L().dev_mode) {
        *r = note_refusal(mod, side, fn, KY_E_DEV_ONLY, std::string(fn) + " needs dev mode (dev_mode = true in kyubu/config.toml)");
        return nullptr;
    }
    *r = KY_OK;
    return mod;
}

// Copies `s` into `buf` (cut, terminated); a NULL or empty buffer takes nothing.
void copy_cut(const std::string& s, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return;
    const size_t n = std::min<size_t>(s.size(), cap - 1);
    memcpy(buf, s.data(), n);
    buf[n] = 0;
}

KyubuResult KYUBU_CALL k_animation_define_from_text(KyubuModHandle self, const char* name, const char* text, char* err,
                                                    uint32_t err_cap) {
    copy_cut("", err, err_cap);
    KyubuResult r = KY_OK;
    Mod* mod = editor_guard(self, "animation_define_from_text", &r);
    if (!mod) return r;
    if (!name || !text) return note_refusal(mod, KY_SIDE_CLIENT, "animation_define_from_text", KY_E_INVALID_ARG, "name or text is NULL");
    std::string errors;
    if (Animations::get().define_from_text(mod->id, name, text, &errors)) return KY_OK;
    copy_cut(errors, err, err_cap);
    // The errors are the reason (Lua's second result) and are logged once per mod and text.
    return note_refusal(mod, KY_SIDE_CLIENT, "animation_define_from_text", KY_E_INVALID_ARG, errors);
}

KyubuResult KYUBU_CALL k_animation_preview(KyubuModHandle self, const char* clip_id, double t_ms) {
    KyubuResult r = KY_OK;
    Mod* mod = editor_guard(self, "animation_preview", &r);
    if (!mod) return r;
    if (!clip_id) return note_refusal(mod, KY_SIDE_CLIENT, "animation_preview", KY_E_INVALID_ARG, "clip_id is NULL");
    if (!std::isfinite(t_ms) || t_ms < 0)
        return note_refusal(mod, KY_SIDE_CLIENT, "animation_preview", KY_E_INVALID_ARG, "t_ms must be 0 or more and finite");
    std::string why;
    if (!Animations::get().set_preview(clip_id, t_ms, &why))
        return note_refusal(mod, KY_SIDE_CLIENT, "animation_preview", KY_E_NOT_FOUND, why);
    return KY_OK;
}

KyubuResult KYUBU_CALL k_animation_preview_clear(KyubuModHandle self) {
    KyubuResult r = KY_OK;
    if (!editor_guard(self, "animation_preview_clear", &r)) return r;
    Animations::get().clear_preview();
    return KY_OK;
}

// The checks of both binding calls: the ability is the caller's own mod ability and the clip is in the registry. Data, so any trust
// level, either side, and no GameCall (no World is needed, and a mod binds in the Register phase). The order of the checks: the mod,
// the arguments, the client side's provider, the ability, the clip. `clip_id` NULL passes only for ability_animation_bind_for (it
// drops the creature's binding).
KyubuResult check_binding(KyubuModHandle self, const char* fn, const char* ability_id, const char* clip_id, bool clip_may_be_null,
                          bool creature_bad) {
    clear_refusal();
    KyubuSide side = KY_SIDE_NONE;
    Mod* mod = ModManager::get().from_handle(self, &side);
    if (!mod || (side != KY_SIDE_CLIENT && side != KY_SIDE_SERVER)) return KY_E_INVALID_ARG;
    if (mod->state == ModState::Faulted) return KY_E_FAULTED;
    if (!ability_id || (!clip_id && !clip_may_be_null)) return note_refusal(mod, side, fn, KY_E_INVALID_ARG, "ability_id or clip_id is NULL");
    if (creature_bad) return note_refusal(mod, side, fn, KY_E_INVALID_ARG, "creature id 0 names no creature");
    if (side == KY_SIDE_CLIENT) {   // a server side draws nothing: it only records the binding
        std::string why;
        if (!available(Prim::animation_pose, &why))
            return note_refusal(mod, side, fn, KY_E_UNAVAILABLE, "the pose provider is unavailable on this build: " + why);
    }
    {
        Registries& regs = Registries::get();
        std::lock_guard<std::recursive_mutex> lock(regs.mutex());
        Registry* reg = regs.get_registry("kyubu:abilities");
        const RegEntry* e = reg ? reg->find(ability_id) : nullptr;
        if (!e) return note_refusal(mod, side, fn, KY_E_NOT_FOUND, std::string("no ability ") + ability_id + " (define it with ability_define first)");
        if (e->vanilla || e->owner == "cubeworld" || e->owner == "kyubu")
            return note_refusal(mod, side, fn, KY_E_INVALID_ARG, std::string(ability_id) + " is not a mod ability: a vanilla ability takes no clip");
        if (e->owner != mod->id)
            return note_refusal(mod, side, fn, KY_E_NAMESPACE, std::string(ability_id) + " belongs to mod " + e->owner + ", not " + mod->id);
    }
    if (clip_id && !Animations::get().clips().set().find(clip_id))
        return note_refusal(mod, side, fn, KY_E_NOT_FOUND, std::string("no clip ") + clip_id);
    return KY_OK;
}

KyubuResult KYUBU_CALL k_ability_animation_bind(KyubuModHandle self, const char* ability_id, const char* clip_id) {
    const KyubuResult r = check_binding(self, "ability_animation_bind", ability_id, clip_id, false, false);
    if (r != KY_OK) return r;
    Animations::get().bind(ability_id, clip_id);
    return KY_OK;
}

// ability_animation_bind_for: the same checks, for one creature by its game id; a NULL clip drops that creature's binding.
KyubuResult KYUBU_CALL k_ability_animation_bind_for(KyubuModHandle self, const char* ability_id, uint64_t creature_id, const char* clip_id) {
    const KyubuResult r = check_binding(self, "ability_animation_bind_for", ability_id, clip_id, true, creature_id == 0);
    if (r != KY_OK) return r;
    Animations::get().bind_for(ability_id, creature_id, clip_id ? clip_id : "");
    return KY_OK;
}

}  // namespace

void fill_animation_api(KyubuApi& a) {
    a.animation_define_from_text = k_animation_define_from_text;
    a.animation_preview = k_animation_preview;
    a.animation_preview_clear = k_animation_preview_clear;
    a.ability_animation_bind = k_ability_animation_bind;
    a.ability_animation_bind_for = k_ability_animation_bind_for;
}

}  // namespace kyubu::game
