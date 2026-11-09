// The ranks file (design D9): a character's rank in each ability, kept as Save\characters.kyubu-abilities.json next to
// the game's own character save. This is the store only; it knows no registry and no game. ability_rank_bind binds it to the
// played character and to the rank functions.
#pragma once
#include "common.h"

#include "json.hpp"

#include <mutex>

namespace kyubu {

class AbilityRanks {
public:
    // Reads the file. A missing file is an empty store. An unreadable one (not JSON, wrong format, wrong shape) is
    // logged, treated as empty, and moved to "<path>.bad" before the first write, so the next save never destroys it.
    // Returns false only for the unreadable case.
    bool load(const std::string& path);

    // Picks the character that is played: the slot and its name (cube::CreatureData.name). An entry of that slot with
    // another name is not applied: its ranks read as the default, and the first change replaces it. Selecting a
    // character does not write the file.
    void select(int slot, const std::string& name);
    void deselect();
    bool selected() const { return slot_ >= 0; }
    int slot() const { return slot_; }

    // The rank of the selected character in an ability, by its string id ("mod:name"): the stored rank, or 1 when there
    // is none (or no character is selected), clamped to 0..max_rank. The stored value is not changed.
    uint32_t get(const std::string& id, uint32_t max_rank) const;

    // Sets the rank, clamped to 0..max_rank, and writes the file. Returns the rank stored, or -1 when no character is
    // selected or the file could not be written (the rank then still holds in memory).
    int set(const std::string& id, uint32_t rank, uint32_t max_rank);

    // Writes the file when something changed (a temporary file, then a rename over the old one).
    bool save();

    const std::string& path() const { return path_; }
    bool dirty() const { return dirty_; }
    bool unreadable() const { return unreadable_; }

private:
    const nlohmann::json* entry() const;        // the selected character's entry, if it holds this name (mu_ held)
    bool save_locked();

    mutable std::mutex mu_;
    std::string path_;
    nlohmann::json root_ = empty_root();
    int slot_ = -1;
    std::string name_;
    bool dirty_ = false;
    bool unreadable_ = false;

    static nlohmann::json empty_root();
};

}  // namespace kyubu
