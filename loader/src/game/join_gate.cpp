#include "join_gate.h"

#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "symbols.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace kyubu::game {

namespace {

constexpr const char* kClientSite = "Net::connectVersionSite";
constexpr const char* kServerSite = "Net::acceptVersionSite";

std::mutex g_mu;
uint32_t g_value = 0;
KyubuId g_patch = 0, g_hook = 0;
std::atomic<uint32_t> g_gate{0};   // read by the server hook
std::atomic<int32_t> g_disp{0};    // the stack slot the server hook rewrites
std::atomic<int> g_logged{0};

void on_accept(KyubuCpuContext* ctx) {
    const uint32_t gate = g_gate.load();
    if (!gate) return;
    uint32_t* slot = reinterpret_cast<uint32_t*>(ctx->ebp + g_disp.load());
    const uint32_t received = *slot;
    const uint32_t stored = gate_accept_slot(received, gate);
    *slot = stored;
    if (g_logged.fetch_add(1) < 20)
        KINFO("join gate: client version 0x%08X: %s", (unsigned)received, stored == kVanillaGateVersion ? "accepted" : "refused");
}

void undo_locked() {
    if (g_patch) Hooks::get().remove("kyubu", g_patch);
    if (g_hook) Hooks::get().remove("kyubu", g_hook);
    g_patch = g_hook = 0;
    g_gate = 0;
    g_value = 0;
}

}  // namespace

uint32_t gate_value(std::vector<GateMod> mods) {
    if (mods.empty()) return 0;
    std::sort(mods.begin(), mods.end(), [](const GateMod& a, const GateMod& b) {
        return a.id != b.id ? a.id < b.id : a.version < b.version;
    });
    uint32_t h = 2166136261u;   // FNV-1a over "id@version;" in sorted order
    auto feed = [&](const std::string& s) {
        for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    };
    for (const GateMod& m : mods) { feed(m.id); feed("@"); feed(m.version); feed(";"); }
    // 0 means "no gate", 3 is the vanilla version and 17 the refusal reply: move off them
    while (h == 0 || h == kVanillaGateVersion || h == 17) h += 0x9E3779B1u;
    return h;
}

std::array<uint8_t, 4> gate_bytes(uint32_t v) {
    return {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
}

bool gate_client_bytes_ok(const uint8_t* s, size_t len, size_t* imm_offset) {
    if (len < 10 || s[0] != 0xC7 || s[1] != 0x85) return false;
    static const uint8_t three[4] = {3, 0, 0, 0};
    if (memcmp(s + 6, three, 4) != 0) return false;
    if (imm_offset) *imm_offset = 6;
    return true;
}

bool gate_server_bytes_ok(const uint8_t* s, size_t len, int32_t* disp) {
    if (len < 7 || s[0] != 0x83 || s[1] != 0xBD || s[6] != 0x03) return false;
    int32_t d;
    memcpy(&d, s + 2, 4);
    if (d >= 0) return false;   // a local of the accept loop
    if (disp) *disp = d;
    return true;
}

uint32_t gate_accept_slot(uint32_t received, uint32_t gate) {
    if (received == gate) return kVanillaGateVersion;
    if (received == kVanillaGateVersion) return 0;
    return received;
}

uint32_t join_gate_set(const std::vector<GateMod>& mods) {
    std::lock_guard<std::mutex> lk(g_mu);
    undo_locked();
    const uint32_t value = gate_value(mods);
    if (!value) return 0;
    const bool server = L().host.host == KY_HOST_SERVER;
    const char* name = server ? kServerSite : kClientSite;
    // addr() resolves the symbol against its full signature; the bytes are checked again below
    const uintptr_t site = SymbolTable::get().addr(name);
    if (!site) {
        KERROR("join gate: %s is unavailable on this build: the gate is NOT in force (%s)", name,
               server ? "this server takes vanilla clients" : "this client can join vanilla servers");
        return 0;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(site);
    std::string err;
    if (server) {
        int32_t disp = 0;
        if (!gate_server_bytes_ok(p, 7, &disp)) {
            KERROR("join gate: the bytes at %s are not the version compare: the gate is NOT in force", name);
            return 0;
        }
        g_disp = disp;
        g_gate = value;
        if (Hooks::get().hook_mid("kyubu", site, KY_SIDE_SERVER, on_accept, &g_hook, &err) != KY_OK) {
            KERROR("join gate: cannot hook %s: %s: the gate is NOT in force", name, err.c_str());
            undo_locked();
            return 0;
        }
    } else {
        size_t off = 0;
        if (!gate_client_bytes_ok(p, 10, &off)) {
            KERROR("join gate: the bytes at %s are not the version store: the gate is NOT in force", name);
            return 0;
        }
        const auto b = gate_bytes(value);
        if (Hooks::get().patch("kyubu", site + off, b.data(), 4, &g_patch, &err) != KY_OK) {
            KERROR("join gate: cannot patch %s: %s: the gate is NOT in force", name, err.c_str());
            undo_locked();
            return 0;
        }
    }
    g_value = value;
    std::string ids;
    for (const GateMod& m : mods) ids += (ids.empty() ? "" : ", ") + m.id + " " + m.version;
    KINFO("join gate: version 0x%08X in force for %s (%s)", (unsigned)value, ids.c_str(),
          server ? "clients with other gate mods are refused" : "vanilla servers refuse this client");
    return value;
}

uint32_t join_gate_value() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_value;
}

}  // namespace kyubu::game
