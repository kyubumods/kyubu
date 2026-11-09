/* zlib_parity: the proxy's zlib exports must behave exactly like the original zlib1.dll (task 2.1).
 * Run in a folder containing the proxy as zlib1.dll and the original as zlib1_vanilla.dll:
 *   wine zlib_parity.exe            -> prints "zlib parity: ok (...)" and exits 0
 * Loads each DLL, compresses/decompresses/checksums the same buffers through both, and compares. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef int (__cdecl* compress2_fn)(unsigned char*, unsigned long*, const unsigned char*, unsigned long, int);
typedef int (__cdecl* uncompress_fn)(unsigned char*, unsigned long*, const unsigned char*, unsigned long);
typedef unsigned long (__cdecl* sum_fn)(unsigned long, const unsigned char*, unsigned int);
typedef const char* (__cdecl* version_fn)(void);

struct Z { HMODULE h; compress2_fn compress2; uncompress_fn uncompress; sum_fn crc32, adler32; version_fn version; };

static int load(const char* name, struct Z* z) {
    z->h = LoadLibraryA(name);
    if (!z->h) { printf("cannot load %s\n", name); return 0; }
    z->compress2 = (compress2_fn)GetProcAddress(z->h, "compress2");
    z->uncompress = (uncompress_fn)GetProcAddress(z->h, "uncompress");
    z->crc32 = (sum_fn)GetProcAddress(z->h, "crc32");
    z->adler32 = (sum_fn)GetProcAddress(z->h, "adler32");
    z->version = (version_fn)GetProcAddress(z->h, "zlibVersion");
    return z->compress2 && z->uncompress && z->crc32 && z->adler32 && z->version;
}

int main(void) {
    static unsigned char in[200000], a[260000], b[260000], back[200000];
    struct Z proxy, orig;
    unsigned int seed = 12345, i;
    int level, fails = 0, cases = 0;
    if (!load("zlib1.dll", &proxy) || !load("zlib1_vanilla.dll", &orig)) return 2;
    if (proxy.h == orig.h) { printf("zlib1.dll is not the proxy\n"); return 2; }
    for (i = 0; i < sizeof in; ++i) { seed = seed * 1103515245u + 12345u; in[i] = (i % 7 == 0) ? (unsigned char)(seed >> 16) : (unsigned char)(i / 97); }
    if (strcmp(proxy.version(), orig.version()) != 0) ++fails;
    for (level = 0; level <= 9; ++level) {
        unsigned long la = sizeof a, lb = sizeof b, lback = sizeof back;
        int ra = proxy.compress2(a, &la, in, sizeof in, level);
        int rb = orig.compress2(b, &lb, in, sizeof in, level);
        ++cases;
        if (ra != rb || la != lb || memcmp(a, b, la) != 0) { printf("compress2 level %d differs\n", level); ++fails; }
        if (proxy.uncompress(back, &lback, a, la) != 0 || lback != sizeof in || memcmp(back, in, sizeof in) != 0) {
            printf("uncompress level %d failed\n", level); ++fails;
        }
    }
    for (i = 1; i < sizeof in; i = i * 3 + 1) {
        ++cases;
        if (proxy.crc32(0, in, i) != orig.crc32(0, in, i) || proxy.adler32(1, in, i) != orig.adler32(1, in, i)) { ++fails; break; }
    }
    printf("zlib parity: %s (zlib %s, %d cases)\n", fails ? "FAILED" : "ok", proxy.version(), cases);
    return fails ? 1 : 0;
}
