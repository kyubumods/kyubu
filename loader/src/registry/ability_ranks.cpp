#include "ability_ranks.h"

#include "log.h"

namespace kyubu {

namespace {

constexpr int kFormat = 1;

std::string dump(const nlohmann::json& j) {
    return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

}  // namespace

nlohmann::json AbilityRanks::empty_root() {
    return nlohmann::json{{"format", kFormat}, {"characters", nlohmann::json::object()}};
}

bool AbilityRanks::load(const std::string& path) {
    std::lock_guard<std::mutex> g(mu_);
    path_ = path;
    root_ = empty_root();
    dirty_ = false;
    unreadable_ = false;
    auto data = read_file(path);
    if (!data) return true;   // no file yet
    auto j = nlohmann::json::parse(data->begin(), data->end(), nullptr, false);
    const char* why = nullptr;
    if (j.is_discarded()) why = "not valid JSON";
    else if (!j.is_object()) why = "not an object";
    else if (!j.contains("format") || !j["format"].is_number_integer() || j["format"].get<int>() != kFormat)
        why = "unknown format";
    else if (!j.contains("characters") || !j["characters"].is_object()) why = "no \"characters\" object";
    if (why) {
        KWARN("Ability ranks: %s is unreadable (%s). Every rank reads as the default; the file is kept as %s.bad on the next write.",
              path.c_str(), why, path.c_str());
        unreadable_ = true;
        return false;
    }
    root_ = std::move(j);
    return true;
}

void AbilityRanks::select(int slot, const std::string& name) {
    std::lock_guard<std::mutex> g(mu_);
    slot_ = slot;
    name_ = name;
}

void AbilityRanks::deselect() {
    std::lock_guard<std::mutex> g(mu_);
    slot_ = -1;
    name_.clear();
}

const nlohmann::json* AbilityRanks::entry() const {
    if (slot_ < 0) return nullptr;
    auto it = root_["characters"].find(std::to_string(slot_));
    if (it == root_["characters"].end() || !it->is_object()) return nullptr;
    auto n = it->find("name");
    if (n == it->end() || !n->is_string() || n->get<std::string>() != name_) return nullptr;
    return &*it;
}

uint32_t AbilityRanks::get(const std::string& id, uint32_t max_rank) const {
    std::lock_guard<std::mutex> g(mu_);
    uint32_t r = 1;
    if (const nlohmann::json* e = entry()) {
        auto ranks = e->find("ranks");
        if (ranks != e->end() && ranks->is_object()) {
            auto v = ranks->find(id);
            if (v != ranks->end() && v->is_number_integer() && v->get<long long>() >= 0)
                r = (uint32_t)std::min<long long>(v->get<long long>(), 0xFFFFFFFFLL);
        }
    }
    return std::min(r, max_rank);
}

int AbilityRanks::set(const std::string& id, uint32_t rank, uint32_t max_rank) {
    std::lock_guard<std::mutex> g(mu_);
    if (slot_ < 0) return -1;
    rank = std::min(rank, max_rank);
    nlohmann::json& chars = root_["characters"];
    nlohmann::json& e = chars[std::to_string(slot_)];
    if (!entry()) e = nlohmann::json{{"name", name_}, {"ranks", nlohmann::json::object()}};   // new, or another name's
    if (!e.contains("ranks") || !e["ranks"].is_object()) e["ranks"] = nlohmann::json::object();
    e["ranks"][id] = rank;
    dirty_ = true;
    return save_locked() ? (int)rank : -1;
}

bool AbilityRanks::save() {
    std::lock_guard<std::mutex> g(mu_);
    return save_locked();
}

bool AbilityRanks::save_locked() {
    if (!dirty_) return true;
    if (path_.empty()) return false;
    if (unreadable_ && file_exists(path_))
        MoveFileExW(widen(path_).c_str(), widen(path_ + ".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
    std::string tmp = path_ + ".tmp";
    if (!write_text(tmp, dump(root_)) ||
        !MoveFileExW(widen(tmp).c_str(), widen(path_).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        KWARN("Ability ranks: could not write %s", path_.c_str());
        DeleteFileW(widen(tmp).c_str());
        return false;
    }
    dirty_ = false;
    unreadable_ = false;
    return true;
}

}  // namespace kyubu
