// The text of the console commands `anim list`, `anim show <id>` and `anim sample <id> <t>` (openspec add-ability-animation, task
// 3.7). Pure: it reads a ClipSet and returns lines, so the unit tests need no console and no game. `anim reload` is the
// console's own (it scans the mods' files).
#pragma once
#include <string>
#include <vector>

#include "animation_registry.h"

namespace kyubu::game::anim {

// `args` is what follows "anim" on the line. An empty or unknown subcommand gives the usage line.
std::vector<std::string> anim_command_lines(const ClipSet& set, const std::string& args);

constexpr const char* kAnimUsage = "usage: anim list | anim show <id> | anim sample <id> [<t>[,<t>...]] | anim reload";

// A mod adds a subcommand to `anim` by registering the console command `<mod id>:anim.<subcommand>`. The builtin hands on to it a line
// whose subcommand it does not know; it never hands on `list`, `show`, `sample` or `reload`, so a mod cannot replace those.
struct AnimExtension {
    std::string sub;       // "preview"
    std::string command;   // "anim_capture:anim.preview"
};

// The extensions among the registered command names, in the order of the names.
std::vector<AnimExtension> anim_extensions(const std::vector<std::string>& command_names);

// The first word of `args` and what follows it (both trimmed; empty for an empty line).
void split_anim_args(const std::string& args, std::string& sub, std::string& rest);

// The command that `anim <sub>` is handed on to, or "" when there is none (the first of the names when two mods claim `sub`).
std::string anim_extension_command(const std::vector<std::string>& command_names, const std::string& sub);

}  // namespace kyubu::game::anim
