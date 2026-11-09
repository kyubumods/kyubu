// auto_enter: skips the title menus (Cube.exe). Once the title screen has been up for delay_frames frames, it does what the clicks
// Start Game, Select (character list) and Select (world list) do, through the game's own handlers:
//   1. hide start_scene and show character_select, as the start menu's Start Game row does;
//   2. set selected_character and call character_select_click, which loads that character into the player and shows world_select;
//   3. on the next frame, set selected_world and call world_select_click, which hides world_select and calls change_world.
// The game loop then opens the world's database, and the loader logs "world entered". Everything runs on the game's main thread, in
// frame.begin. With no saved character or world it logs why and leaves the menu alone.
// With the option `join` (a host, such as 127.0.0.1) step 3 joins that server instead, as the world list's Connect to server and
// Connect do: it hides world_select and calls client_connect(host) (the game's own join, which blocks until the server answers);
// the loader logs "connected to" on a join. For the replays of a server with joined clients (kmap probe replay, fast_join).
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <cubeworld/cubeworld.h>

#include "kyubu.hpp"

namespace {

using ClickFn = void(__attribute__((thiscall)) *)(void* controller);
using ConnectFn = void(__attribute__((thiscall)) *)(void* controller, const void* host);   // host: const std::string&

constexpr uint32_t kNodeDisplay = 0x3C;                          // plasma::Node.display
constexpr uint32_t kDisplayKey = 0x68, kDisplayValues = 0x94;    // plasma::Display.key, visible_values
constexpr uint32_t kWorldName = 0x8, kWorldSeed = 0x20;          // WorldInfo: std::string name, int32 seed (as world_select_click reads them)

kyubu::Mod* g_mod = nullptr;
uintptr_t g_gc_global = 0;
ClickFn g_character_click = nullptr, g_world_click = nullptr;
ConnectFn g_connect = nullptr;
std::string g_join;         // a server to join instead of a world (empty: enter the world)

enum class Step { Wait, World, Done };
Step g_step = Step::Wait;
int g_title_frames = 0;
bool g_auto = true;
int g_delay = 20;
std::string g_want_character, g_want_world;
int g_world_index = -1;
bool g_requested = false;   // auto_enter:go asked for it

uintptr_t controller() {
    return g_gc_global ? *reinterpret_cast<uintptr_t*>(g_gc_global) : 0;
}

uintptr_t node_at(uintptr_t gc, uint32_t off) { return *reinterpret_cast<uintptr_t*>(gc + off); }

int32_t* visible_word(uintptr_t node) {
    if (!node) return nullptr;
    const uintptr_t display = *reinterpret_cast<uintptr_t*>(node + kNodeDisplay);
    if (!display) return nullptr;
    const int32_t key = *reinterpret_cast<int32_t*>(display + kDisplayKey);
    int32_t* values = *reinterpret_cast<int32_t**>(display + kDisplayValues);
    return values ? values + key : nullptr;
}
bool visible(uintptr_t node) {
    const int32_t* w = visible_word(node);
    return w && *w;
}
void set_visible(uintptr_t node, bool on) {
    if (int32_t* w = visible_word(node)) *w = on ? 1 : 0;
}

// An MSVC 2012 std::string: 16 bytes of text or a pointer, then size and capacity; the text is inline while capacity < 16.
// An MSVC 2012 std::string of `text` (at most 15 characters, inline), as the game's own functions take it.
struct MsvcString {
    char buf[16];
    uint32_t size, cap;
};
bool make_msvc_string(MsvcString& out, const std::string& text) {
    if (text.size() > 15) return false;
    std::memset(&out, 0, sizeof out);
    std::memcpy(out.buf, text.data(), text.size());
    out.size = static_cast<uint32_t>(text.size());
    out.cap = 15;
    return true;
}

std::string msvc_string(uintptr_t at) {
    const uint32_t size = *reinterpret_cast<uint32_t*>(at + 16), cap = *reinterpret_cast<uint32_t*>(at + 20);
    if (size > 4096) return {};
    const char* text = cap < 16 ? reinterpret_cast<const char*>(at) : *reinterpret_cast<const char* const*>(at);
    return text ? std::string(text, size) : std::string();
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Picks an entry: empty = the first, all digits = that index, else the first whose name matches (case ignored). -1 when none.
template <typename NameOf>
int pick(const std::string& want, int count, NameOf name_of) {
    if (count <= 0) return -1;
    if (want.empty()) return 0;
    char* end = nullptr;
    const long n = std::strtol(want.c_str(), &end, 10);
    if (end && *end == 0) return n >= 0 && n < count ? static_cast<int>(n) : -1;
    for (int i = 0; i < count; ++i)
        if (lower(name_of(i)) == lower(want)) return i;
    return -1;
}

int vector_count(const std_vector& v) {
    return v.first && v.last ? static_cast<int>((reinterpret_cast<uintptr_t>(v.last) - reinterpret_cast<uintptr_t>(v.first)) / 4) : 0;
}
uintptr_t vector_at(const std_vector& v, int i) { return reinterpret_cast<uintptr_t*>(v.first)[i]; }

std::string character_name(cube_GameController* gc, int i) {
    const uintptr_t c = vector_at(gc->characters, i);
    if (!c) return {};
    const char* name = reinterpret_cast<const cube_Creature*>(c)->data.name;
    return std::string(name, strnlen(name, sizeof reinterpret_cast<const cube_Creature*>(c)->data.name));
}
std::string world_name(cube_GameController* gc, int i) {
    const uintptr_t w = vector_at(gc->worlds, i);
    return w ? msvc_string(w + kWorldName) : std::string();
}

void list(cube_GameController* gc) {
    const int nc = vector_count(gc->characters), nw = vector_count(gc->worlds);
    for (int i = 0; i < nc; ++i) g_mod->info("auto_enter: character %d: %s", i, character_name(gc, i).c_str());
    for (int i = 0; i < nw; ++i)
        g_mod->info("auto_enter: world %d: %s (seed %d)", i, world_name(gc, i).c_str(), *reinterpret_cast<int32_t*>(vector_at(gc->worlds, i) + kWorldSeed));
}

// Steps 1 and 2. Returns false (and logs why) when there is nothing to enter.
bool enter_character(cube_GameController* gc) {
    list(gc);
    const int nc = vector_count(gc->characters), nw = vector_count(gc->worlds);
    const int ci = pick(g_want_character, nc, [gc](int i) { return character_name(gc, i); });
    if (ci < 0) {
        g_mod->info("auto_enter: no saved character matches '%s' (%d saved): staying at the menu", g_want_character.c_str(), nc);
        return false;
    }
    const int wi = g_join.empty() ? pick(g_want_world, nw, [gc](int i) { return world_name(gc, i); }) : 0;
    if (wi < 0) {
        g_mod->info("auto_enter: no saved world matches '%s' (%d saved): staying at the menu", g_want_world.c_str(), nw);
        return false;
    }
    if (g_join.empty())
        g_mod->info("auto_enter: entering character %d (%s), world %d (%s)", ci, character_name(gc, ci).c_str(), wi, world_name(gc, wi).c_str());
    else
        g_mod->info("auto_enter: joining %s with character %d (%s)", g_join.c_str(), ci, character_name(gc, ci).c_str());
    set_visible(reinterpret_cast<uintptr_t>(gc->start_scene), false);   // as Start Game does
    set_visible(reinterpret_cast<uintptr_t>(gc->character_select), true);
    gc->selected_character = ci;
    g_character_click(gc);
    if (!visible(reinterpret_cast<uintptr_t>(gc->world_select))) {
        g_mod->warn("auto_enter: the world list did not open after the character was selected: stopping");
        return false;
    }
    g_world_index = wi;
    return true;
}

void on_frame() {
    if (g_step == Step::Done || !g_character_click || !g_world_click) return;
    if (g_mod->api()->phase() < KY_PHASE_GAMEREADY) return;
    auto* gc = reinterpret_cast<cube_GameController*>(controller());
    if (!gc) return;
    if (g_step == Step::World) {
        g_step = Step::Done;
        if (!g_join.empty()) {
            MsvcString host;
            make_msvc_string(host, g_join);
            set_visible(reinterpret_cast<uintptr_t>(gc->world_select), false);   // as Connect to server does
            g_mod->info("auto_enter: connecting to %s", g_join.c_str());
            g_connect(gc, &host);
            return;
        }
        gc->selected_world = g_world_index;
        g_world_click(gc);
        g_mod->info("auto_enter: world selected; the game loads it now");
        return;
    }
    // Step::Wait: the title screen with its menu up (and not the options panel)
    const bool title = visible(reinterpret_cast<uintptr_t>(gc->start_scene)) && !visible(reinterpret_cast<uintptr_t>(gc->options_panel));
    if (!title) {
        g_title_frames = 0;
        return;
    }
    if (!g_requested && (!g_auto || ++g_title_frames < g_delay)) return;
    g_requested = false;
    if (enter_character(gc)) {
        g_step = Step::World;
    } else {
        g_auto = false;   // tried once; auto_enter:go can try again
    }
}

void say(KyubuPrintFn print, void* ctx, const char* text) {
    print(ctx, text);
    g_mod->info("%s", text);
}

// auto_enter:go [character] [world]: enter from the title screen on the next frame (the work runs in frame.begin).
void KYUBU_CALL cmd_go(const char* args, KyubuPrintFn print, void* ctx, void*) {
    std::string a = args ? args : "", first, second;
    const size_t s = a.find_first_not_of(' ');
    if (s != std::string::npos) {
        const size_t e = a.find(' ', s);
        first = a.substr(s, e == std::string::npos ? std::string::npos : e - s);
        if (e != std::string::npos) {
            const size_t t = a.find_first_not_of(' ', e);
            if (t != std::string::npos) second = a.substr(t, a.find_last_not_of(' ') - t + 1);
        }
    }
    if (!first.empty()) g_want_character = first;
    if (!second.empty()) g_want_world = second;
    if (g_step == Step::World) {
        say(print, ctx, "auto_enter: already entering a world");
        return;
    }
    g_step = Step::Wait;
    g_requested = true;
    say(print, ctx, "auto_enter: entering on the next frame at the title screen");
}

}  // namespace

struct AutoEnter {
    explicit AutoEnter(kyubu::Mod& m) : mod(m) {
        if (m.side() != KY_SIDE_CLIENT) return;
        g_mod = &m;
        g_gc_global = m.symbol("cube::game_controller");
        g_character_click = reinterpret_cast<ClickFn>(m.symbol(CW_SYM_character_select_click));
        g_world_click = reinterpret_cast<ClickFn>(m.symbol(CW_SYM_world_select_click));
        if (!g_gc_global || !g_character_click || !g_world_click) {
            m.warn("auto_enter: a symbol is missing (cube::game_controller, character_select_click or world_select_click): the mod does nothing");
            return;
        }
        g_auto = m.config_bool("enabled", true);
        g_want_character = m.config_string("character", "");
        g_want_world = m.config_string("world", "");
        g_delay = static_cast<int>(m.config_int("delay_frames", 20));
        g_join = m.config_string("join", "");
        if (!g_join.empty()) {
            MsvcString probe;
            g_connect = reinterpret_cast<ConnectFn>(m.symbol(CW_SYM_client_connect));
            if (!g_connect) {
                m.warn("auto_enter: join '%s' asked, but client_connect is missing: entering the world instead", g_join.c_str());
                g_join.clear();
            } else if (!make_msvc_string(probe, g_join)) {
                m.warn("auto_enter: join '%s' is longer than 15 characters: entering the world instead", g_join.c_str());
                g_join.clear();
            }
        }
        mod.on("frame.begin", [](KyubuEvent&) { on_frame(); });
        if (m.api()->register_command(m.handle(), "auto_enter:go", "auto_enter:go [character] [world]: enter a saved world from the title screen (empty: the first; a number: its place; else a name)", cmd_go, nullptr) != KY_OK)
            m.warn("auto_enter: could not register auto_enter:go");
        m.info("auto_enter: loaded (%s; character '%s', %s '%s', after %d frames)", g_auto ? "enters by itself" : "only on auto_enter:go",
               g_want_character.c_str(), g_join.empty() ? "world" : "join", g_join.empty() ? g_want_world.c_str() : g_join.c_str(), g_delay);
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("auto_enter", KY_SIDE_CLIENT, AutoEnter)
