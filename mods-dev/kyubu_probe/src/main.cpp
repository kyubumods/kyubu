// kyubu_probe: the entry points of the developer mod. Everything else is in probe.cpp.
#include <cstring>

#include "probe.h"

extern "C" KYUBU_EXPORT int KYUBU_CALL kyubu_mod_query(KyubuModQuery* q) {
    q->api_version = KYUBU_API_VERSION;
    std::strncpy(q->id, kprobe::kModId, sizeof q->id - 1);
    q->sides = KY_SIDE_CLIENT | KY_SIDE_SERVER;
    return 0;
}

extern "C" KYUBU_EXPORT int KYUBU_CALL kyubu_mod_init(const KyubuApi* api, KyubuModHandle self, KyubuSide side) {
    return kprobe::Probe::init(api, self, side);
}

extern "C" KYUBU_EXPORT void KYUBU_CALL kyubu_mod_shutdown(KyubuSide side) { kprobe::Probe::shutdown(side); }
