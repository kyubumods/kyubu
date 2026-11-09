#include "console.h"

#include "abilities.h"
#include "ability_rank_bind.h"
#include "animation_console.h"
#include "animations.h"
#include "channels.h"
#include "events.h"
#include "handles.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "modconfig.h"
#include "modmanager.h"
#include "overlay.h"
#include "runtime.h"
#include "symbols.h"
#include "world.h"

#include <algorithm>

namespace kyubu {

Console& Console::get() {
    static Console* c = new Console();
    return *c;
}

KyubuResult Console::reg(Mod* mod, const std::string& name, const std::string& help, KyubuCommandFn fn, void* user) {
    if (!fn || !mod) return KY_E_INVALID_ARG;
    auto colon = name.find(':');
    if (colon == std::string::npos || name.substr(0, colon) != mod->id || colon + 1 == name.size()) return KY_E_NAMESPACE;
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (cmds_.count(name)) return KY_E_DUPLICATE;
    Cmd c;
    c.mod = mod;
    c.help = help;
    c.fn = fn;
    c.user = user;
    cmds_[name] = c;
    return KY_OK;
}

void Console::reg_builtin(const std::string& name, const std::string& help, bool dev_only, BuiltinFn fn) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    Cmd c;
    c.help = help;
    c.builtin = std::move(fn);
    c.dev_only = dev_only;
    cmds_[name] = c;
}

void Console::remove_mod(Mod* mod) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (auto it = cmds_.begin(); it != cmds_.end();) it = it->second.mod == mod ? cmds_.erase(it) : std::next(it);
}

void Console::append(const std::string& line) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    history_.push_back(line);
    if (history_.size() > 2000) history_.erase(history_.begin(), history_.begin() + 500);
}

void Console::remove(const std::string& name) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    cmds_.erase(name);
}

std::vector<std::string> Console::history() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    return history_;
}

std::vector<std::string> Console::names() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<std::string> v;
    for (auto& [k, c] : cmds_) v.push_back(k);
    return v;
}

std::vector<Console::Completion> Console::complete(const std::string& token) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<Completion> prefix, inner;
    for (auto& [name, c] : cmds_) {
        if (c.dev_only && !L().dev_mode) continue;
        if (name.compare(0, token.size(), token) == 0) {
            prefix.push_back({name, c.help});
            continue;
        }
        auto colon = name.find(':');
        if (colon != std::string::npos && name.compare(colon + 1, token.size(), token) == 0) inner.push_back({name, c.help});
    }
    prefix.insert(prefix.end(), inner.begin(), inner.end());
    return prefix;
}

std::string Console::common_prefix(const std::vector<Completion>& names) {
    if (names.empty()) return "";
    std::string p = names[0].name;
    for (auto& n : names) {
        size_t i = 0;
        while (i < p.size() && i < n.name.size() && p[i] == n.name[i]) ++i;
        p.resize(i);
    }
    return p;
}

void Console::execute(const std::string& line_in, const std::function<void(const std::string&)>& print_in) {
    std::string line = trim(line_in);
    if (line.empty()) return;
    std::function<void(const std::string&)> print = [&](const std::string& s) {
        append(s);
        if (print_in) print_in(s);
    };
    append("> " + line);
    auto sp = line.find(' ');
    std::string name = line.substr(0, sp), args = sp == std::string::npos ? "" : trim(line.substr(sp + 1));
    if (!run(name, args, print)) print("unknown command '" + name + "' (try help)");
}

bool Console::run(const std::string& name, const std::string& args, const std::function<void(const std::string&)>& print) {
    Cmd c;
    {
        std::lock_guard<std::recursive_mutex> g(mu_);
        auto it = cmds_.find(name);
        if (it == cmds_.end()) return false;
        c = it->second;
    }
    if (c.dev_only && !L().dev_mode) { print("'" + name + "' requires dev mode (--kyubu-dev or loader.dev_mode = true)"); return true; }
    if (c.builtin) { c.builtin(args, print); return true; }
    struct Ctx { const std::function<void(const std::string&)>* p; } ctx{&print};
    auto cprint = [](void* p, const char* text) { (*((Ctx*)p)->p)(text ? text : ""); };
    ModManager::get().invoke(c.mod, KY_SIDE_CLIENT, name.c_str(), [&] { c.fn(args.c_str(), cprint, &ctx, c.user); });
    return true;
}

void register_builtin_commands() {
    auto& con = Console::get();
    con.reg_builtin("help", "list commands", false, [](const std::string&, auto& print) {
        auto& c = Console::get();
        std::lock_guard<std::recursive_mutex> g(c.mu_);
        for (auto& [n, cmd] : c.cmds_)
            print(fmt("  %-22s %s%s", n.c_str(), cmd.help.c_str(), cmd.dev_only ? " (dev mode)" : ""));
    });
    con.reg_builtin("mods", "list mods with version, sides and state", false, [](const std::string&, auto& print) {
        for (auto& l : ModManager::get().summary_lines()) print(l);
    });
    con.reg_builtin("log", "log [n]: show the last n log lines (default 20)", false, [](const std::string& a, auto& print) {
        int n = a.empty() ? 20 : atoi(a.c_str());
        for (auto& l : Log::get().tail(n > 0 ? n : 20)) print(l.text);
    });
    con.reg_builtin("sym", "sym [name]: resolve a symbol (no name: list all)", false, [](const std::string& a, auto& print) {
        if (a.empty()) {
            for (auto& s : SymbolTable::get().all()) {
                uintptr_t addr = 0;
                bool ok = SymbolTable::get().resolve(s.name, &addr) == KY_OK;
                print(fmt("%-40s %-9s %s", s.name.c_str(), s.status.c_str(),
                          ok ? fmt("0x%08X", (unsigned)addr).c_str() : "unavailable"));
            }
            return;
        }
        uintptr_t addr = 0;
        std::string why;
        auto r = SymbolTable::get().resolve(a, &addr, &why);
        const Symbol* s = SymbolTable::get().find(a);
        if (r == KY_OK)
            print(fmt("%s = 0x%08X (%s, %s, %s)", a.c_str(), (unsigned)addr, s->status.c_str(), s->convention.c_str(),
                      describe_address(addr).c_str()));
        else print(a + ": " + (why.empty() ? "not found" : why));
    });
    con.reg_builtin("assets", "assets <key>: which layer serves an asset key", false, [](const std::string& a, auto& print) {
        std::string key = a;
        std::string db = "data1.db";
        if (ends_with(lower(key), ".wav")) db = "data2.db";
        else if (ends_with(lower(key), ".png")) db = "data3.db";
        else if (ends_with(lower(key), ".xml")) db = "data4.db";
        print(db + "/" + key + " <- " + Assets::get().served_by(db, key));
    });
    con.reg_builtin("manifest", "print the session manifest", false, [](const std::string&, auto& print) {
        print(session_manifest_json());
    });
    con.reg_builtin("events", "events: how often each event has been emitted", false, [](const std::string&, auto& print) {
        for (auto& [n, c] : Events::get().counts()) print(fmt("  %-24s %llu", n.c_str(), (unsigned long long)c));
    });
    con.reg_builtin("hooks", "hooks [mod]: list installed hooks", false, [](const std::string& a, auto& print) {
        for (auto& h : Hooks::get().list(a))
            print(fmt("  #%u %-8s %-12s %s", h.id, hook_kind_name(h.kind), h.owner.c_str(), h.description.c_str()));
    });
    con.reg_builtin("game", "game [hold <id>|all]: the game API's Worlds, game thread, lock, primitives and handles", false, [](const std::string& a, auto& print) {
        const std::string arg = trim(a);
        if (arg.rfind("hold", 0) == 0) {
            // dev mode: a client-side handle for a creature, so that a later `game` shows whether it is still live
            const std::string id = trim(arg.substr(4));
            const std::string line = !L().dev_mode ? "game hold requires dev mode (--kyubu-dev or loader.dev_mode = true)"
                                     : id == "all" ? game::hold_creature(0, true)
                                     : id.empty() || id.find_first_not_of("0123456789") != std::string::npos
                                         ? "usage: game hold <creature id>|all"
                                         : game::hold_creature(std::stoull(id));
            print(line);
            KINFO("game: %s", line.c_str());
            return;
        }
        // logged too, so a replay's log shows it
        for (auto& line : game::report()) {
            print(line);
            KINFO("game: %s", line.c_str());
        }
    });
    con.reg_builtin("ability", "ability list|slots|rank: every ability with its number and claim | the holder of slot 4 per class | the played character's rank in a mod ability (ability rank <id> [rank])", false,
                    [](const std::string& a, auto& print) {
        const std::string arg = trim(a);
        const std::vector<std::string> lines = arg == "list"    ? ability_list_lines()
                                               : arg == "slots" ? ability_slot_lines()
                                               : arg == "rank" || arg.rfind("rank ", 0) == 0 ? game::ability_rank_lines(arg.substr(4))
                                                                : std::vector<std::string>{"usage: ability list | ability slots | ability rank <id> [rank]"};
        // logged too, so a replay's log shows it (as `game` does)
        for (auto& line : lines) {
            print(line);
            KINFO("ability: %s", line.c_str());
        }
    });
    con.reg_builtin("anim", "anim list|show <id>|sample <id> [<t>]|reload: the loaded animation clips, one clip's ranges, its pose at fractions of its duration, a rescan of the mods' files (a mod adds more: anim with no word lists them)", false,
                    [](const std::string& a, auto& print) {
        const std::string arg = trim(a);
        // a subcommand that a mod registered as `<mod id>:anim.<subcommand>` (`anim preview` of anim_capture): handed on, with the rest of the line
        std::string sub, rest;
        game::anim::split_anim_args(arg, sub, rest);
        if (!sub.empty()) {
            const std::string command = game::anim::anim_extension_command(Console::get().names(), sub);
            if (!command.empty()) {
                Console::get().run(command, rest, print);
                return;
            }
        }
        if (arg == "reload") {
            Animations::get().scan();
            const auto held = Animations::get().clips();
            const game::anim::ClipSet& set = held.set();
            const std::string line = fmt("anim: scanned the mods' clips: %u clip(s), hash %.12s", (unsigned)set.clips.size(), set.hash.c_str());
            print(line);
            KINFO("%s", line.c_str());
            return;
        }
        // logged too, so a replay's log shows it (as `ability` does)
        const std::vector<std::string> lines = game::anim::anim_command_lines(Animations::get().clips().set(), arg);
        for (auto& line : lines) {
            print(line);
            KINFO("anim: %s", line.c_str());
        }
        if (lines.size() == 1 && lines[0] == game::anim::kAnimUsage) {   // the usage line: add the mods' subcommands
            for (auto& e : game::anim::anim_extensions(Console::get().names())) {
                const std::string line = "  anim " + e.sub + "  (mod command " + e.command + ")";
                print(line);
                KINFO("anim: %s", line.c_str());
            }
        }
    });
    con.reg_builtin("reload","reload <mod>: reload a mod's scripts and assets", true, [](const std::string& a, auto& print) {
        print(runtime_reload_mod(a));
    });
    con.reg_builtin("config", "config [<mod> [<key> <value>] | reload <mod>]: mod options (docs/config.md)", false,
                    [](const std::string& a, auto& print) {
        std::vector<std::string> w;
        for (auto& part : split(a, ' ')) if (!trim(part).empty()) w.push_back(trim(part));
        auto with_options = [&](const std::string& id) -> Mod* {
            Mod* m = ModManager::get().find(id);
            if (m && m->config) return m;
            print("no mod '" + id + "' with options (type config for the list)");
            return nullptr;
        };
        if (w.empty()) {
            int n = 0;
            for (auto& m : ModManager::get().all()) {
                if (!m->config) continue;
                ++n;
                print(fmt("  %-20s %2u option(s)  %s", m->id.c_str(), (unsigned)m->config->decls.size(), m->config->display.c_str()));
            }
            if (!n) print("no mod declares options");
        } else if (w[0] == "reload" && w.size() == 2) {
            if (Mod* m = with_options(w[1])) for (auto& l : config_reload(m)) print(l);
        } else if (w.size() == 1) {
            Mod* m = with_options(w[0]);
            if (!m) return;
            ModConfig& c = *m->config;
            std::lock_guard<std::recursive_mutex> g(c.mu);
            print(m->id + ": " + c.display + (c.file_broken ? " (not valid TOML: every option uses its default)" : ""));
            for (auto& d : c.decls)
                print(fmt("  %s = %s  (default %s, from the %s)", d.key.c_str(), format_value(c.values[d.key]).c_str(),
                          format_value(d.def).c_str(), c.from_file[d.key] ? "file" : "default"));
        } else if (w.size() >= 3) {
            Mod* m = with_options(w[0]);
            if (!m) return;
            // the value is the rest of the line; quotes around a string are optional
            size_t at = a.find(w[1], a.find(w[0]) + w[0].size()) + w[1].size();
            std::string value = trim(a.substr(at));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            std::string err;
            if (config_set_text(m, w[1], value, &err) != KY_OK) { print(err); return; }
            OptValue v;
            config_get(m, w[1], std::nullopt, &v, nullptr);
            print(m->id + ": " + w[1] + " = " + format_value(v) + " (saved to " + m->config->display + ")");
        } else {
            print("usage: config | config <mod> | config <mod> <key> <value> | config reload <mod>");
        }
    });
    con.reg_builtin("lua", "lua <client|server> <code>: run Lua in a side's VM", true, [](const std::string& a, auto& print) {
        auto sp = a.find(' ');
        std::string side = a.substr(0, sp), code = sp == std::string::npos ? "" : a.substr(sp + 1);
        if (side != "client" && side != "server") { print("usage: lua <client|server> <code>"); return; }
        print(runtime_lua_eval(side == "client" ? KY_SIDE_CLIENT : KY_SIDE_SERVER, code));
    });
}

// `.cmd` keeps clear of the anim_capture mod's `<name>.txt` requests in the same folder. A writer should write under
// another name and rename (kmap probe replay's `console` input step does), so a half-written file is never read.
void console_inbox_poll() {
    static uint64_t next = 0;
    uint64_t now = GetTickCount64();
    if (now < next) return;
    next = now + 250;
    std::string dir = join_path(L().kyubu_dir, "inbox");
    if (!dir_exists(dir)) return;
    std::vector<std::string> names;
    for (auto& n : list_dir(dir))
        if (n.size() > 4 && n[0] != '.' && ends_with(lower(n), ".cmd")) names.push_back(n);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        std::string path = join_path(dir, n);
        auto data = read_file(path);
        if (!data) continue;
        if (!DeleteFileW(widen(path).c_str())) continue;   // removed first: a line that crashes the game is not run again
        std::string text(data->begin(), data->end());
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
        for (auto& raw : split(text, '\n')) {
            std::string line = trim(raw);
            if (line.empty() || line[0] == '#') continue;
            KINFO("inbox %s: > %s", n.c_str(), line.c_str());
            Console::get().execute(line, [&](const std::string& s) { KINFO("inbox %s: %s", n.c_str(), s.c_str()); });
        }
    }
}

}  // namespace kyubu
