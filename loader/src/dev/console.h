// Console commands (built-in and mod-registered) and their execution.
#pragma once
#include "common.h"

#include <map>
#include <mutex>

namespace kyubu {

struct Mod;

class Console {
public:
    static Console& get();

    KyubuResult reg(Mod* mod, const std::string& name, const std::string& help, KyubuCommandFn fn, void* user);
    using BuiltinFn = std::function<void(const std::string& args, const std::function<void(const std::string&)>& print)>;
    void reg_builtin(const std::string& name, const std::string& help, bool dev_only, BuiltinFn fn);
    void remove_mod(Mod* mod);
    void remove(const std::string& name);

    // Executes one command line; output lines go to `print` (and the console history).
    void execute(const std::string& line, const std::function<void(const std::string&)>& print);
    // Runs the registered command `name` with `args`, on the line's behalf: no history entry, no completion. False when there is none.
    // A builtin hands a line on to a mod's command with it (`anim preview`, docs/dev-tools.md).
    bool run(const std::string& name, const std::string& args, const std::function<void(const std::string&)>& print);
    std::vector<std::string> names();

    // Completion of a command name (the first word of a line). A command matches when `token` starts its name, or the part of
    // its name after the mod's `id:` ("tp" finds "teleport:tp"); prefix matches come first. Commands that need dev mode are left
    // out while it is off.
    struct Completion { std::string name, help; };
    std::vector<Completion> complete(const std::string& token);
    // The longest prefix that every name starts with (empty for no names).
    static std::string common_prefix(const std::vector<Completion>& names);

    // Console text history shown by the overlay.
    void append(const std::string& line);
    std::vector<std::string> history();

private:
    friend void register_builtin_commands();
    struct Cmd { Mod* mod = nullptr; std::string help; KyubuCommandFn fn = nullptr; void* user = nullptr; BuiltinFn builtin; bool dev_only = false; };
    std::recursive_mutex mu_;
    std::map<std::string, Cmd> cmds_;
    std::vector<std::string> history_;
};

void register_builtin_commands();

// Dev mode: runs the console lines of each <game>/kyubu/inbox/<name>.cmd as if typed (at most every 250 ms; call it
// on the game thread). The file is removed first; each line and its output go to the log as `inbox <name>.cmd: ...`.
void console_inbox_poll();

}  // namespace kyubu
