// The Animator's `In game` button (openspec add-ability-animation, task 7.6; design decision 7): it drops a request file in the game's
// dev-mode inbox, `<game>/kyubu/inbox/`, that holds `anim preview <clip> <t_ms>`; the capture mod (mods-dev/anim_capture/src/inbox.h)
// runs it and answers with `<name>.reply`. Pure and header only: no windows.h, no ImGui, no game, so a unit test builds it
// (tests/unit/test_anim_inbox.cpp).
//
// The button is off by default and sends nothing until the author turned it on and named the game folder. The file is written under a
// temporary name and renamed, so the mod never reads half a line.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace kanimator {

// The clip's id in the game: `<mod id>:<file stem>` for a clip at `<mod>/assets/animations/<stem>.anim.toml` (the mod's folder name is
// its id in the repository's mods). Empty when the path does not have that shape: the author types the id then.
inline std::string ingame_clip_id(const std::string& clip_path, const std::string& stem) {
    std::string p = clip_path;
    for (char& c : p)
        if (c == '\\') c = '/';
    const std::string tail = "/assets/animations/" + stem + ".anim.toml";
    if (stem.empty() || p.size() <= tail.size() || p.compare(p.size() - tail.size(), tail.size(), tail) != 0) return std::string();
    const std::string head = p.substr(0, p.size() - tail.size());
    const size_t slash = head.find_last_of('/');
    const std::string mod = slash == std::string::npos ? head : head.substr(slash + 1);
    return mod.empty() ? std::string() : mod + ":" + stem;
}

// The console line the button sends. `t_ms` is the scrub time.
inline std::string ingame_preview_line(const std::string& clip_id, double t_ms) {
    char t[32];
    std::snprintf(t, sizeof t, "%.1f", t_ms < 0 ? 0.0 : t_ms);
    return "anim preview " + clip_id + " " + t;
}

inline std::string ingame_inbox_dir(const std::string& game_folder) {
    std::string d = game_folder;
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    return d + "\\kyubu\\inbox";
}

// Writes `line` as a request file in the game's inbox. False, with the reason, when it cannot (no game folder, no inbox folder: the game
// has not run the capture mod in dev mode yet).
inline bool ingame_drop(const std::string& game_folder, const std::string& line, std::string* why) {
    if (game_folder.empty()) {
        *why = "no game folder: type the folder that holds Cube.exe";
        return false;
    }
    const std::string dir = ingame_inbox_dir(game_folder);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char name[48];
    std::snprintf(name, sizeof name, "animator-%013lld", ms);
    const std::string tmp = dir + "\\" + name + ".tmp";
    const std::string fin = dir + "\\" + name + ".txt";
    {
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) {
            *why = "no folder " + dir + ": run the game once in dev mode with the anim_capture mod";
            return false;
        }
        const std::string text = line + "\n";
        const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        if (!wrote) {
            std::remove(tmp.c_str());
            *why = "could not write " + tmp;
            return false;
        }
    }
    if (std::rename(tmp.c_str(), fin.c_str()) != 0) {
        std::remove(tmp.c_str());
        *why = "could not rename the request into " + dir;
        return false;
    }
    return true;
}

}  // namespace kanimator
