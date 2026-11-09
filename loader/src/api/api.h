#pragma once
#include <cstddef>

#include "kyubu_api.h"

namespace kyubu {
const KyubuApi* api_table();
// Whether a native mod built for API version v (KyubuModQuery::api_version) can load: same major, minor <= ours.
bool api_version_supported(uint32_t v);

// Side rules: which logical side may call each KyubuApi member (single source of truth, see api.cpp).
struct ApiSideRule { const char* name; KyubuSide sides; };
const ApiSideRule* api_side_rules(size_t* count);
KyubuSide api_side_of(const char* fn);                 // KY_SIDE_NONE for unknown names
bool api_side_allowed(const char* fn, KyubuSide caller);
}
