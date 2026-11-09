// Model slot extension: new model slots after vanilla's 2569, vanilla remaps, reloads, save id maps.
#pragma once
#include "common.h"

namespace kyubu {

class Models {
public:
    static Models& get();
    bool install_hooks();                 // ModelLoader::loadAll and Model::load
    bool ready() const { return loader_ != nullptr; }

    uint32_t slot_count();                // current size of the game's model vector
    void* model_at(uint32_t slot);        // Model* or nullptr
    std::string key_of_slot(uint32_t slot);

    // Reloads every slot whose asset key is `key` (hot reload). Main thread only.
    int reload_key(const std::string& key);
    // Re-applies registry numbers to the vector after a save id map changed them.
    void apply_numbers();

    void on_data1_opened();            // called by the Database::open hook during loadAll
    void on_loadall_done(void* loader);

private:
    void extend();
    void* loader_ = nullptr;   // ModelLoader*
    void* db_ = nullptr;       // Database* for data1.db
};

}  // namespace kyubu
