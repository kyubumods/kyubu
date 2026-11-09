/*
 * kyubu_probe.h - the field finder of the kyubu_probe developer mod, for other mods (live-probes).
 *
 * kyubu_probe publishes this table as the interface "kyubu_probe:finder", version 1, when it is active (dev mode):
 *
 *   void* p; uint32_t v;
 *   if (api->get_interface(KYUBU_PROBE_FINDER, 1, &p, &v) == KY_OK) { const KyubuProbeFinder* f = p; ... }
 *
 * From Lua (full trust), lua/kyubu_probe.lua wraps it. Every function may be called from any side and thread.
 * Errors are negative KyubuResult values; finder->error() returns the text of the last one.
 */
#ifndef KYUBU_PROBE_H
#define KYUBU_PROBE_H

#include "kyubu_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KYUBU_PROBE_FINDER "kyubu_probe:finder"
#define KYUBU_PROBE_FINDER_VERSION 1

typedef struct KyubuProbeHit {   /* 32 bytes, no padding under any alignment rule */
    double value;             /* in the latest snapshot */
    double previous;          /* in the snapshot before it (= value after the first snapshot) */
    uint32_t offset;          /* from the start of the search's range */
    uint32_t address;         /* runtime address */
    uint32_t changes;         /* snapshots in which it changed */
    uint32_t reserved;
} KyubuProbeHit;

typedef struct KyubuProbeFinder {
    uint32_t size;            /* sizeof(KyubuProbeFinder) */
    /* A search over `length` bytes at `address` (an address expression: "0x1234ABCD", "@0x4ED0EA" for a mapping
       address, a symbol name, "[x]+0x10"...). value_type: i8 u8 i16 u16 i32 u32 f32 f64. align 0 = the value's
       size. Returns the search id (> 0). */
    int32_t (KYUBU_CALL* create)(const char* address, uint32_t length, const char* value_type, uint32_t align);
    /* Reads the range. Returns the number of candidate offsets left. */
    int32_t (KYUBU_CALL* snapshot)(int32_t search);
    /* Keeps the candidates for which `op` holds between the last two snapshots: "changed", "unchanged",
       "increased", "decreased", "delta" (changed by `value`), or in the latest one: "equal" (to `value`).
       Returns the number of candidates left. */
    int32_t (KYUBU_CALL* filter)(int32_t search, const char* op, double value);
    /* The candidate at `rank` (0 = most often changed, then lowest offset). KY_E_NOT_FOUND past the end. */
    int32_t (KYUBU_CALL* result)(int32_t search, uint32_t rank, KyubuProbeHit* out);
    int32_t (KYUBU_CALL* destroy)(int32_t search);
    /* Copies the text of the last error into buf. */
    int32_t (KYUBU_CALL* error)(char* buf, uint32_t cap);
} KyubuProbeFinder;

#ifdef __cplusplus
}
#endif
#endif /* KYUBU_PROBE_H */
