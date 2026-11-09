// Layered asset resolution in front of data1-4.db (dev-assets > mods (reverse load order) > providers > vanilla).
#pragma once
#include "common.h"

#include <map>
#include <mutex>

namespace kyubu {

struct Mod;

struct ResolvedAsset {
    std::vector<uint8_t> data;   // plain (decoded) bytes
    std::string layer;           // "dev-assets", mod id, "provider:<mod>", "kyubu" or "vanilla"
};

class Assets {
public:
    static Assets& get();

    static std::string db_for_category(const std::string& cat);   // "models" -> "data1.db"
    static std::string category_for_db(const std::string& db);
    static bool is_dictionary(const std::string& key) { return key == "dict_en.xml" || key == "dict_de.xml"; }

    void build_index();
    // Overlay-only resolution (no vanilla). `shadowed` receives lower layers that also provide the key.
    std::optional<ResolvedAsset> resolve_overlay(const std::string& db, const std::string& key,
                                                 std::vector<std::string>* shadowed = nullptr);
    // Full resolution including vanilla and dictionary merging.
    std::optional<ResolvedAsset> resolve(const std::string& db, const std::string& key);
    // Merges every layer's dictionary entries (and text registry entries) into `vanilla_plain`.
    std::string merge_dictionaries(const std::string& key, const std::string& vanilla_plain);

    KyubuResult add_provider(Mod* mod, const std::string& db, const std::string& glob, KyubuAssetProviderFn fn,
                             void* user, KyubuId* out);
    void remove_providers(Mod* mod);
    void remove_provider(KyubuId id);

    std::string served_by(const std::string& db, const std::string& key);
    void record_served(const std::string& db, const std::string& key, const std::string& layer);
    void log_summary();

    // Game Database objects seen by the Database::open hook.
    void on_db_open(void* obj, const std::string& path);
    std::string db_name_of(void* obj);
    std::string db_path(const std::string& db);          // path the game used, e.g. "data1.db"
    std::optional<std::vector<uint8_t>> read_vanilla(const std::string& db, const std::string& key);

    // Dev mode: returns asset keys whose files changed since the last call.
    std::vector<std::pair<std::string, std::string>> poll_changes();   // (db, key)

    struct Source { std::string layer; Mod* mod = nullptr; std::string rel; std::string disk; uint64_t mtime = 0; };

    // Install the Database hooks (needs verified symbols). Returns false when unavailable.
    bool install_hooks();
    void* our_database(const std::string& db);   // a game Database object opened by the loader

private:
    std::optional<std::vector<uint8_t>> load_source(const std::string& db, const std::string& key, const Source& s,
                                                    std::string* err);

    std::recursive_mutex mu_;
    std::map<std::string, std::map<std::string, std::vector<Source>>> index_;   // db -> key -> sources (high prio first)
    struct Provider { KyubuId id; Mod* mod; std::string db, glob; KyubuAssetProviderFn fn; void* user; };
    std::vector<Provider> providers_;
    KyubuId next_id_ = 1;
    std::map<std::string, std::string> served_;          // "db/key" -> layer
    std::map<void*, std::string> db_objects_;            // Database* -> db file name
    std::map<std::string, std::string> db_paths_;        // db file name -> path given to open
    std::map<std::string, void*> own_dbs_;
    std::map<std::string, uint64_t> watched_;            // disk path -> mtime
    uint64_t last_poll_ = 0;
};

}  // namespace kyubu
