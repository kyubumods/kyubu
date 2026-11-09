// Per-mod configuration (docs/config.md): the options a mod declares in its package's config.toml, and the
// player's kyubu/config/<id>.toml that holds their values.
#pragma once
#include "common.h"

#include <map>
#include <mutex>
#include <variant>

namespace kyubu {

struct Mod;

enum class OptType { Bool, Int, Float, String };
const char* opt_type_name(OptType t);   // "bool", "int", "float", "string"

using OptValue = std::variant<bool, int64_t, double, std::string>;

struct OptionDecl {
    std::string key;
    OptType type = OptType::Bool;
    OptValue def;
    std::string description;
    std::optional<OptValue> min, max;   // int and float options
    std::vector<std::string> choices;   // string options
    std::string label;                  // settings-form text, at most 40 characters (1.2; only cosmetic)
    std::string group;                  // settings-form group (1.2; only cosmetic)
    bool restart = false;               // the mod reads the option only at start (D14; the value applies at once)
};

// Parses a package's config.toml. On failure returns nullopt and appends "<src>: key '...': ..." errors.
// Options come back in the order they are written in the file.
std::optional<std::vector<OptionDecl>> parse_config_decl(const std::string& text, const std::string& src,
                                                         std::vector<std::string>& errors);

std::string format_value(const OptValue& v);        // as TOML: true, 8, 8.0, "fire"
std::string describe_allowed(const OptionDecl& d);  // "a number from 1.0 to 32.0", "one of \"fire\", \"ice\""
// Converts v to the option's type (an integer is a valid float) and checks the limits.
bool check_value(const OptionDecl& d, const OptValue& v, OptValue* out);
// Parses a value typed as text (console, native API): "16", "true", "fire". False when it is not of the type.
bool parse_value_text(const OptionDecl& d, const std::string& text, OptValue* out);

// The comment lines and "key = default" line the loader writes for an option.
std::string option_block(const OptionDecl& d);
// Replaces the value of one top-level key in TOML text, keeping every other byte; adds the key before the first
// table header (or at the end) when it is missing. Callers re-parse the result to make sure it is what they meant.
std::string rewrite_key(const std::string& text, const std::string& key, const std::string& value_toml);

struct ModConfig {
    std::string mod_id, mod_name;
    std::string path;       // <game>/kyubu/config/<id>.toml
    std::string display;    // kyubu/config/<id>.toml, for messages
    std::vector<OptionDecl> decls;
    std::map<std::string, OptValue> values;     // effective values
    std::map<std::string, bool> from_file;      // true when the value came from the player's file
    bool file_broken = false;
    std::recursive_mutex mu;

    const OptionDecl* decl(const std::string& key) const;
    // Reads the player's file into `values`; a bad value or a broken file falls back to defaults with a warning.
    // With `write`, a missing file is created with every default and a readable file gains missing options.
    void load(bool write, std::vector<std::string>& warnings);
    // Validates and saves one value, rewriting only that key's line. *err says why on failure.
    KyubuResult set(const std::string& key, const OptValue& v, std::string* err, bool* changed);
};

// Mod loading: reads the package's config.toml (if any) and the player's file in `config_dir`. Returns false with
// *error set when the declaration is invalid; the mod must then not load.
bool config_attach(Mod* m, const std::string& config_dir, std::string* error);

// Reads an option of the calling mod. KY_E_NOT_FOUND for an undeclared key, KY_E_INVALID_ARG for the wrong type
// (want = nullopt accepts any type).
KyubuResult config_get(Mod* m, const std::string& key, std::optional<OptType> want, OptValue* out, std::string* err);
// Sets an option (API, console): validated, saved, and config.changed queued when the value changed.
KyubuResult config_set(Mod* m, const std::string& key, const OptValue& v, std::string* err);
// Same, with the value typed as text.
KyubuResult config_set_text(Mod* m, const std::string& key, const std::string& text, std::string* err);
// Re-reads the player's file; queues config.changed for every changed value. Returns what happened, one line each.
std::vector<std::string> config_reload(Mod* m);
// Emits the queued config.changed events (main thread: frame.begin, or the server tick on Server.exe).
void config_pump();

}  // namespace kyubu
