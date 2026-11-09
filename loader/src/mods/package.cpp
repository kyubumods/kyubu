#include "package.h"

#include <map>

#include "miniz.h"

namespace kyubu {

std::string normalize_rel(std::string rel) {
    for (auto& c : rel) if (c == '\\') c = '/';
    while (starts_with(rel, "./")) rel = rel.substr(2);
    while (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
    return rel;
}

static bool unsafe_rel(const std::string& rel) {
    return rel.find("..") != std::string::npos || rel.find(':') != std::string::npos;
}

namespace {

class FolderPackage : public Package {
public:
    explicit FolderPackage(const std::string& p) { path_ = p; }
    std::string full(const std::string& rel) {
        std::string r = normalize_rel(rel);
        for (auto& c : r) if (c == '/') c = '\\';
        return join_path(path_, r);
    }
    bool exists(const std::string& rel) override { return !unsafe_rel(rel) && file_exists(full(rel)); }
    std::optional<std::vector<uint8_t>> read(const std::string& rel) override {
        if (unsafe_rel(rel)) return std::nullopt;
        return read_file(full(rel));
    }
    void walk(const std::string& dir, const std::string& rel, std::vector<std::string>& out) {
        for (auto& n : list_dir(dir)) {
            std::string p = join_path(dir, n);
            std::string r = rel.empty() ? n : rel + "/" + n;
            if (dir_exists(p)) walk(p, r, out);
            else out.push_back(r);
        }
    }
    std::vector<std::string> list(const std::string& prefix) override {
        std::vector<std::string> out;
        std::string pre = normalize_rel(prefix);
        while (!pre.empty() && pre.back() == '/') pre.pop_back();
        walk(pre.empty() ? path_ : full(pre), pre, out);
        return out;
    }
    uint64_t mtime(const std::string& rel) override { return file_mtime(full(rel)); }
    std::optional<std::string> materialize(const std::string& rel, const std::string&) override {
        if (!exists(rel)) return std::nullopt;
        return full(rel);
    }
    bool is_zip() const override { return false; }
};

class ZipPackage : public Package {
public:
    explicit ZipPackage(const std::string& p) {
        path_ = p;
        auto data = read_file(p);
        if (data) bytes_ = std::move(*data);
        memset(&zip_, 0, sizeof zip_);
        ok_ = !bytes_.empty() && mz_zip_reader_init_mem(&zip_, bytes_.data(), bytes_.size(), 0);
        if (ok_) {
            mz_uint n = mz_zip_reader_get_num_files(&zip_);
            // Support archives that wrap everything in a single top-level folder.
            std::vector<std::string> names;
            for (mz_uint i = 0; i < n; ++i) {
                char name[1024];
                mz_zip_reader_get_filename(&zip_, i, name, sizeof name);
                if (!mz_zip_reader_is_file_a_directory(&zip_, i)) names.push_back(normalize_rel(name));
            }
            bool has_root_manifest = false;
            for (auto& s : names) if (s == "mod.toml") has_root_manifest = true;
            if (!has_root_manifest) {
                for (auto& s : names) {
                    auto slash = s.find('/');
                    if (slash != std::string::npos && s.substr(slash + 1) == "mod.toml") { root_ = s.substr(0, slash + 1); break; }
                }
            }
            for (mz_uint i = 0; i < n; ++i) {
                char name[1024];
                mz_zip_reader_get_filename(&zip_, i, name, sizeof name);
                if (mz_zip_reader_is_file_a_directory(&zip_, i)) continue;
                std::string s = normalize_rel(name);
                if (!root_.empty()) { if (!starts_with(s, root_)) continue; s = s.substr(root_.size()); }
                index_[s] = i;
            }
        }
        stamp_ = file_mtime(p);
    }
    ~ZipPackage() override { if (ok_) mz_zip_reader_end(&zip_); }
    bool exists(const std::string& rel) override { return index_.count(normalize_rel(rel)) != 0; }
    std::optional<std::vector<uint8_t>> read(const std::string& rel) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = index_.find(normalize_rel(rel));
        if (it == index_.end()) return std::nullopt;
        size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip_, it->second, &size, 0);
        if (!p) return std::nullopt;
        std::vector<uint8_t> v((uint8_t*)p, (uint8_t*)p + size);
        mz_free(p);
        return v;
    }
    std::vector<std::string> list(const std::string& prefix) override {
        std::string pre = normalize_rel(prefix);
        if (!pre.empty() && pre.back() != '/') pre += '/';
        std::vector<std::string> out;
        for (auto& [k, v] : index_) if (pre.empty() || starts_with(k, pre)) out.push_back(k);
        return out;
    }
    uint64_t mtime(const std::string&) override { return stamp_; }
    std::optional<std::string> materialize(const std::string& rel, const std::string& cache_dir) override {
        auto data = read(rel);
        if (!data) return std::nullopt;
        std::string r = normalize_rel(rel);
        for (auto& c : r) if (c == '/') c = '\\';
        std::string out = join_path(cache_dir, r);
        auto existing = read_file(out);
        if (!existing || *existing != *data) write_file(out, data->data(), data->size());
        return out;
    }
    bool is_zip() const override { return true; }
    bool ok() const { return ok_; }

private:
    std::vector<uint8_t> bytes_;
    mz_zip_archive zip_;
    bool ok_ = false;
    std::string root_;
    std::map<std::string, mz_uint> index_;
    std::mutex mu_;
    uint64_t stamp_ = 0;
};

}  // namespace

std::unique_ptr<Package> Package::open(const std::string& path) {
    if (dir_exists(path)) return std::make_unique<FolderPackage>(path);
    if (ends_with(lower(path), ".zip") && file_exists(path)) {
        auto z = std::make_unique<ZipPackage>(path);
        if (z->ok()) return z;
    }
    return nullptr;
}

}  // namespace kyubu
