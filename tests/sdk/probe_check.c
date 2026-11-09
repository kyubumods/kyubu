/* Compiles kyubu_probe.h, the finder interface of the kyubu_probe developer mod (SDK include/), as C and C++. */
#include "kyubu_probe.h"

typedef char probe_check_hit_size[sizeof(KyubuProbeHit) == 32 ? 1 : -1];
const char* probe_check_name = KYUBU_PROBE_FINDER;

int32_t probe_check_first(const KyubuProbeFinder* f, KyubuProbeHit* out) {
    int32_t s = f->create("@0x400000", 0x200, "u32", 0);
    if (s <= 0) return s;
    f->snapshot(s);
    f->filter(s, "unchanged", 0);
    s = f->result(s, 0, out);
    return s;
}
