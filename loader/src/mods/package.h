// Read-only access to a mod package: a folder or a .zip archive.
#pragma once
#include "common.h"

#include <memory>
#include <mutex>

namespace kyubu {

class Package {
public:
    virtual ~Package() = default;
    virtual bool exists(const std::string& rel) = 0;
    virtual std::optional<std::vector<uint8_t>> read(const std::string& rel) = 0;
    // Files (relative, '/' separated) under a directory prefix, recursively.
    virtual std::vector<std::string> list(const std::string& prefix) = 0;
    // Last-modified stamp of a file (0 when unknown); zip packages report the archive's mtime.
    virtual uint64_t mtime(const std::string& rel) = 0;
    // A path on disk for the file (extracting from a zip into cache_dir when needed).
    virtual std::optional<std::string> materialize(const std::string& rel, const std::string& cache_dir) = 0;
    virtual bool is_zip() const = 0;
    const std::string& path() const { return path_; }

    static std::unique_ptr<Package> open(const std::string& path);   // folder or .zip

protected:
    std::string path_;
};

std::string normalize_rel(std::string rel);   // backslashes -> '/', strip "./"

}  // namespace kyubu
