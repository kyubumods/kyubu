// The join gate (openspec add-skills-unleashed-multiplayer, design D4): one protocol value for all gameplay mods.
// A mod with `gate = true` in mod.toml is a gate mod. The loader derives one 32-bit value from the sorted ids and versions of the
// gate mods that loaded, patches the client's version store to send it, and makes the server compare it (a mid hook that rewrites
// the received version into the 3 the game's own compare wants). A client with another set of gate mods, another version of one,
// or none is refused by the game's own check. Every patch is made at checked bytes and undone by join_gate_set({}).
#pragma once
#include "common.h"

#include <array>

namespace kyubu::game {

struct GateMod {
    std::string id;
    std::string version;   // the semantic version as text
};

inline constexpr uint32_t kVanillaGateVersion = 3;   // what a vanilla client sends and a vanilla server accepts

// The value for a set of gate mods; the same for every load order. 0 for an empty set (no gate). Never 0, 3 or 17.
uint32_t gate_value(std::vector<GateMod> mods);

// The little-endian bytes the game stores and sends.
std::array<uint8_t, 4> gate_bytes(uint32_t value);

// Client: the store `mov dword [ebp+disp], 3` (C7 85 disp32 03 00 00 00) of Net::connectVersionSite. True when the 10 bytes at
// `site` are that store with the vanilla 3; `imm_offset` is the offset of the immediate (6).
bool gate_client_bytes_ok(const uint8_t* site, size_t len, size_t* imm_offset);

// Server: the compare `cmp dword [ebp+disp], 3` (83 BD disp32 03) of Net::acceptVersionSite. True when the 7 bytes at `site` are
// that compare; `disp` is the stack slot of the version the client sent.
bool gate_server_bytes_ok(const uint8_t* site, size_t len, int32_t* disp);

// What the server writes into the received-version slot before the compare: the gate value becomes the 3 the compare wants,
// the vanilla 3 becomes 0 (so it is refused), any other value stays (and is refused).
uint32_t gate_accept_slot(uint32_t received, uint32_t gate);

// Installs the gate for these mods on the running host (client patch, server hook), after undoing the earlier one. An empty set
// removes it. Returns the value now in force (0: none). Does nothing when the host's sites are unavailable (logged).
uint32_t join_gate_set(const std::vector<GateMod>& mods);
uint32_t join_gate_value();

}  // namespace kyubu::game
