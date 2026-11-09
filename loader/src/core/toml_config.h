// Single place that configures toml++ for the loader (no exceptions).
#pragma once
#define TOML_EXCEPTIONS 0
#define TOML_ENABLE_FORMATTERS 1
#include "toml.hpp"

#include <string>
#include <sstream>

namespace kyubu {
inline std::string fmt_toml_error(const toml::parse_error& e) {
    std::ostringstream os;
    os << (e.source().path ? *e.source().path : std::string("<toml>")) << ":" << e.source().begin.line << ":"
       << e.source().begin.column << ": " << e.description();
    return os.str();
}
}  // namespace kyubu
