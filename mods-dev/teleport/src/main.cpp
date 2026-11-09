// teleport: moves the local player around the map (Cube.exe, single-player or joined to a server: the client decides its own position, and the server follows). It reads the position with the game API's creature_get and moves
// the player with creature_teleport (65,536 units per block, x east, y north, z up; the call also stops the player). The HP hold and the
// dungeon search still use raw access. No zone is loaded where the player lands, and the mod cannot read the ground, so a teleport to a long_hop place
// HOVERS the player (z fixed, velocity 0) until `teleport land`, or until kAutoLand has passed. After the landing the player's HP is held for a few seconds, so
// that a fall from the hover height does not kill.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <cubeworld/cubeworld.h>

#include "kyubu.hpp"

namespace {

constexpr double kUnitsPerBlock = 65536.0;
constexpr double kHoverBlocks = 250.0;   // the height of a teleport that names no z
constexpr double kFarBlocks = 100.0;
constexpr double kLimitBlocks = 1.0e9;
constexpr auto kProtect = std::chrono::seconds(12);
constexpr auto kAutoLand = std::chrono::seconds(10);   // a hover that nobody ended lands by itself after this

kyubu::Mod* g_mod = nullptr;
bool g_hover = false;
int64_t g_hover_z = 0;
std::chrono::steady_clock::time_point g_hover_until;
bool g_protect = false;
float g_protect_hp = 0;
std::chrono::steady_clock::time_point g_protect_until;

uintptr_t player_creature() {
    const uintptr_t global = g_mod->symbol("cube::game_controller");
    uint32_t controller = 0, player = 0;
    if (!global || g_mod->api()->mem_read(global, &controller, sizeof controller) != KY_OK || !controller) return 0;
    if (g_mod->api()->mem_read(controller + offsetof(cube_GameController, player), &player, sizeof player) != KY_OK) return 0;
    return player;
}

constexpr uint32_t kData = offsetof(cube_Creature, data);

// The local player as a game-API handle (0 before a world is loaded), its position through creature_get and its move through
// creature_teleport (which also zeroes the velocity).
KyubuCreature player_handle() {
    KyubuCreature h = 0;
    return g_mod->creature_player(h) == KY_OK ? h : 0;
}

bool read_pos(KyubuCreature player, int64_t out[3]) {
    KyubuCreatureInfo info;
    if (!player || g_mod->creature_get(player, info) != KY_OK) return false;
    std::memcpy(out, info.pos, 3 * sizeof(int64_t));
    return true;
}

bool move_player(KyubuCreature player, const int64_t p[3]) {
    const KyubuResult r = g_mod->creature_teleport(player, p);
    if (r != KY_OK) g_mod->warn("teleport: creature_teleport failed (result %d)", static_cast<int>(r));
    return r == KY_OK;
}

void say(KyubuPrintFn print, void* ctx, const char* text) {
    print(ctx, text);
    g_mod->info("%s", text);   // the log keeps it too: replays read the log
}

// The dungeons of the regions the World has built. The region pointer grid is at World+0xBC (1024 x 1024 slots). A region holds 64
// missions of 0x68 bytes from +0x14018: origin x u64 +0, origin y u64 +8 (world units, 65,536 per block, the centre of the area; for a
// dungeon it is the centre of the dungeon's zone tile), kind +0x18 (0xE is a dungeon), level +0x24. Same layout as level_rework's
// `levels areas`. Checked on 2026-10-03 against the zone tiles (kind byte 3 in the 4096 tiles from +0x18): region (r, c), tile index
// a * 64 + b lies at x = (r * 64 + a) * 256 + 128 blocks and y = (c * 64 + b) * 256 + 128, which is the mission's origin.
struct Dungeon {
    int level;
    int plus;      // the dungeon's "+p" (zone tile +0xC), -1 if the tile was not found
    double x, y;   // blocks
};

bool collect_dungeons(std::vector<Dungeon>* out) {
    constexpr uint32_t kGrid = 0xBC, kMissions = 0x14018;
    const uintptr_t global = g_mod->symbol("cube::game_controller");
    uint32_t controller = 0;
    if (!global || g_mod->api()->mem_read(global, &controller, sizeof controller) != KY_OK || !controller) return false;
    const uintptr_t grid = controller + offsetof(cube_GameController, world) + kGrid;
    std::vector<uint32_t> row(1024);
    int regions = 0;
    for (int r = 0; r < 1024 && regions < 64; ++r) {
        if (g_mod->api()->mem_read(grid + static_cast<uintptr_t>(r) * 1024 * 4, row.data(), 1024 * 4) != KY_OK) break;
        for (int c = 0; c < 1024 && regions < 64; ++c) {
            if (!row[c]) continue;
            ++regions;
            std::vector<uint8_t> missions(64 * 0x68), tiles(4096 * 16);
            if (g_mod->api()->mem_read(row[c] + kMissions, missions.data(), static_cast<uint32_t>(missions.size())) != KY_OK) continue;
            const bool have_tiles = g_mod->api()->mem_read(row[c] + 0x18, tiles.data(), static_cast<uint32_t>(tiles.size())) == KY_OK;
            for (uint32_t m = 0; m < 64; ++m) {
                uint32_t kind = 0;
                int32_t level = 0;
                uint64_t ox = 0, oy = 0;
                std::memcpy(&kind, &missions[m * 0x68 + 0x18], 4);
                if (kind != 0xE) continue;
                std::memcpy(&level, &missions[m * 0x68 + 0x24], 4);
                std::memcpy(&ox, &missions[m * 0x68], 8);
                std::memcpy(&oy, &missions[m * 0x68 + 8], 8);
                // the zone tile of the mission's origin: index a * 64 + b with x = (r * 64 + a) * 256 + 128 blocks, y = (c * 64 + b) * 256 + 128
                int plus = -1;
                const long a = static_cast<long>(ox / kUnitsPerBlock / 256) - r * 64, b = static_cast<long>(oy / kUnitsPerBlock / 256) - c * 64;
                if (have_tiles && a >= 0 && a < 64 && b >= 0 && b < 64 && tiles[(a * 64 + b) * 16] == 3) plus = tiles[(a * 64 + b) * 16 + 0xC];
                out->push_back({level, plus, ox / kUnitsPerBlock, oy / kUnitsPerBlock});
            }
        }
    }
    return true;
}

bool parse_blocks(const char*& s, double* out) {
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s || !std::isfinite(v) || std::fabs(v) > kLimitBlocks) return false;
    s = end;
    *out = v;
    return true;
}

void skip_spaces(const char*& s) {
    while (*s == ' ') ++s;
}

// Ends the hover: the player falls, and its HP is held for kProtect so that the fall from the hover height does not kill.
void land(KyubuCreature player) {
    KyubuCreatureInfo info;
    const float hp = g_mod->creature_get(player, info) == KY_OK ? info.hp : 0;
    g_hover = false;
    g_protect = true;
    g_protect_hp = hp;
    g_protect_until = std::chrono::steady_clock::now() + kProtect;
}

// Each frame: hold a hovering player (landing it once kAutoLand has passed), and keep the HP of a landed one for kProtect.
void on_frame() {
    if (!g_hover && !g_protect) return;
    if (g_hover && std::chrono::steady_clock::now() >= g_hover_until) {
        const KyubuCreature handle = player_handle();
        if (handle) {
            land(handle);
            g_mod->info("teleport: landing by itself after %d s (your HP is held for %d s)", static_cast<int>(kAutoLand.count()), static_cast<int>(kProtect.count()));
        } else {
            g_hover = false;   // no player any more (back at the menu): nothing to hold
        }
    }
    if (g_hover) {
        const KyubuCreature handle = player_handle();
        int64_t p[3];
        if (handle && read_pos(handle, p)) {
            p[2] = g_hover_z;
            move_player(handle, p);   // also stops the player
        }
    }
    const uintptr_t player = g_protect ? player_creature() : 0;
    if (g_protect && player) {
        if (std::chrono::steady_clock::now() >= g_protect_until) {
            g_protect = false;
            return;
        }
        float hp = 0;
        const uintptr_t at = player + kData + offsetof(cube_CreatureData, hp);
        if (g_mod->api()->mem_read(at, &hp, sizeof hp) == KY_OK && hp < g_protect_hp) g_mod->api()->mem_write(g_mod->handle(), at, &g_protect_hp, sizeof g_protect_hp);
    }
}

void KYUBU_CALL cmd_teleport(const char* args, KyubuPrintFn print, void* ctx, void*);

void KYUBU_CALL cmd_teleport(const char* args, KyubuPrintFn print, void* ctx, void*) {
    const char* s = args ? args : "";
    skip_spaces(s);
    const KyubuCreature player = player_handle();
    int64_t p[3];
    if (!player || !read_pos(player, p)) {
        say(print, ctx, "teleport: no player yet (enter a world first)");
        return;
    }
    char buf[200];
    if (!*s) {
        std::snprintf(buf, sizeof buf, "teleport: you are at x %.1f, y %.1f, z %.1f (blocks)%s", p[0] / kUnitsPerBlock, p[1] / kUnitsPerBlock, p[2] / kUnitsPerBlock,
                      g_hover ? "; hovering: `teleport:tp land` drops you, or wait for the automatic landing" : "");
        say(print, ctx, buf);
        return;
    }
    if (std::strncmp(s, "dungeon", 7) == 0 && (s[7] == 's' || s[7] == ' ' || s[7] == 0)) {
        const bool jump = s[7] != 's';   // `dungeons [level]` lists, `dungeon <level>` goes to the nearest one of that level
        s += s[7] == 's' ? 8 : 7;
        skip_spaces(s);
        char* end = nullptr;
        const long want = *s ? std::strtol(s, &end, 10) : -1;
        long want_plus = -1;   // an optional "+p" after the level
        if (end && end != s && *end == ' ') {
            const char* q = end;
            skip_spaces(q);
            char* end2 = nullptr;
            want_plus = std::strtol(q, &end2, 10);
            if (end2 == q || *end2) want_plus = -2;
            else end = end2;
        }
        if (*s && (end == s || *end || want_plus == -2)) {
            say(print, ctx, "teleport: usage: teleport:tp dungeons [level [+p]] | teleport:tp dungeon <level> [+p]");
            return;
        }
        if (jump && want < 0) {
            say(print, ctx, "teleport: usage: teleport:tp dungeon <level>");
            return;
        }
        std::vector<Dungeon> all, chosen;
        if (!collect_dungeons(&all)) {
            say(print, ctx, "teleport: no world yet");
            return;
        }
        const double px = p[0] / kUnitsPerBlock, py = p[1] / kUnitsPerBlock;
        for (const Dungeon& d : all)
            if ((want < 0 || d.level == want) && (want_plus < 0 || d.plus == want_plus)) chosen.push_back(d);
        std::sort(chosen.begin(), chosen.end(), [&](const Dungeon& a, const Dungeon& b) {
            return std::hypot(a.x - px, a.y - py) < std::hypot(b.x - px, b.y - py);
        });
        if (chosen.empty()) {
            std::snprintf(buf, sizeof buf, "teleport: no dungeon of level %ld among the %zu the world has built", want, all.size());
            say(print, ctx, buf);
            return;
        }
        if (jump) {
            char cmd[96];
            std::snprintf(cmd, sizeof cmd, "%.1f %.1f", chosen[0].x, chosen[0].y);
            std::snprintf(buf, sizeof buf, "teleport: the nearest level %ld dungeon is %.0f blocks away", want, std::hypot(chosen[0].x - px, chosen[0].y - py));
            say(print, ctx, buf);
            cmd_teleport(cmd, print, ctx, nullptr);
            return;
        }
        for (size_t i = 0; i < chosen.size() && i < 24; ++i) {
            std::snprintf(buf, sizeof buf, "teleport: dungeon level %d +p %d at x %.0f, y %.0f (%.0f blocks away)", chosen[i].level, chosen[i].plus, chosen[i].x, chosen[i].y,
                          std::hypot(chosen[i].x - px, chosen[i].y - py));
            say(print, ctx, buf);
        }
        std::snprintf(buf, sizeof buf, "teleport: %zu dungeon(s) listed of %zu built (nearest first)", std::min<size_t>(chosen.size(), 24), all.size());
        say(print, ctx, buf);
        return;
    }
    if (std::strncmp(s, "land", 4) == 0 && (s[4] == 0 || s[4] == ' ')) {
        if (!g_hover) {
            say(print, ctx, "teleport: you are not hovering");
            return;
        }
        land(player);
        say(print, ctx, "teleport: landing (your HP is held for 12 s)");
        return;
    }
    bool relative = false;
    if (std::strncmp(s, "rel", 3) == 0 && (s[3] == ' ')) {
        relative = true;
        s += 3;
    }
    double x = 0, y = 0, z = 0;
    skip_spaces(s);
    if (!parse_blocks(s, &x)) goto usage;
    skip_spaces(s);
    if (!parse_blocks(s, &y)) goto usage;
    skip_spaces(s);
    {
        const bool has_z = *s != 0;
        if (has_z && !parse_blocks(s, &z)) goto usage;
        skip_spaces(s);
        if (*s) goto usage;
        double nx = relative ? p[0] / kUnitsPerBlock + x : x, ny = relative ? p[1] / kUnitsPerBlock + y : y;
        double nz = has_z ? (relative ? p[2] / kUnitsPerBlock + z : z) : (relative ? p[2] / kUnitsPerBlock : kHoverBlocks);
        // A hop of more than kFarBlocks lands where no zone is loaded yet: go up and hover (a z that was asked for is kept)
        const bool long_hop = std::fabs(nx - p[0] / kUnitsPerBlock) > kFarBlocks || std::fabs(ny - p[1] / kUnitsPerBlock) > kFarBlocks;
        if (long_hop && !has_z && nz < kHoverBlocks) nz = kHoverBlocks;
        if (nx < 0 || ny < 0 || nz < 0) {
            say(print, ctx, "teleport: x, y and z must not be negative");
            return;
        }
        const int64_t q[3] = {static_cast<int64_t>(nx * kUnitsPerBlock), static_cast<int64_t>(ny * kUnitsPerBlock), static_cast<int64_t>(nz * kUnitsPerBlock)};
        if (!move_player(player, q)) {
            say(print, ctx, "teleport: the move was refused (see the log)");
            return;
        }
        // a jump to a place or a far hop hovers; a short hop on the ground falls by itself, and a hover goes on at the new height
        if (!relative || long_hop) g_hover = true;
        if (g_hover) {
            g_hover_z = q[2];
            g_hover_until = std::chrono::steady_clock::now() + kAutoLand;   // every new move restarts the wait
        }
        std::snprintf(buf, sizeof buf, "teleport: moved to x %.1f, y %.1f, z %.1f%s", nx, ny, nz, g_hover ? "; hovering, landing by itself in 10 s (or `teleport:tp land`)" : "");
        say(print, ctx, buf);
        return;
    }
usage:
    say(print, ctx, "teleport: usage: teleport:tp | teleport:tp <x> <y> [z] | teleport:tp rel <dx> <dy> [dz] | teleport:tp land | teleport:tp dungeons [level] | teleport:tp dungeon <level> (blocks; a jump hovers at z 250 and lands by itself after 10 s)");
}

}  // namespace

struct Teleport {
    explicit Teleport(kyubu::Mod& m) : mod(m) {
        if (m.side() != KY_SIDE_CLIENT) return;
        g_mod = &m;
        mod.on("frame.begin", [](KyubuEvent&) { on_frame(); });
        if (m.api()->register_command(m.handle(), "teleport:tp", "teleport:tp | teleport:tp <x> <y> [z] | teleport:tp rel <dx> <dy> [dz] | teleport:tp land | teleport:tp dungeons [level] | teleport:tp dungeon <level>: move the local player (blocks)", cmd_teleport, nullptr) != KY_OK)
            mod.warn("teleport: could not register the teleport command");
        mod.info("teleport: loaded");
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("teleport", KY_SIDE_CLIENT, Teleport)
