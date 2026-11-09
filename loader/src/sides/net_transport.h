// The channel transport of a joined game (openspec add-skills-unleashed-multiplayer 10.4, design D10). A channel message to the
// other end travels as one Kyubu packet on the game's own connection:
//   u32 packet id (kKyubuPacketId), u32 length of the rest, u16 channel name length, channel name (UTF-8), payload.
// A mid hook at the end of each receive loop's packet switch (Net::clientReceiveNextSite, Net::connectionReceiveNextSite) reads
// the packet with recv on that thread and hands it to Channels, which delivers it at the next frame or tick. Sends are queued and
// written by the connection's own send thread, between two of the game's packets (Net::clientSendPassSite,
// connection_send_pass_site), so they never interleave with a game packet. No Lua and no mod code run on a network thread.
// The join gate (D4) is the handshake: the transport sends only while a gate value is in force.
#pragma once
#include "common.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace kyubu {

// No game build uses this id (Cube.exe reads 0 to 5, 10, 15; Server.exe 0, 6 to 12). The bytes read "UBYK".
inline constexpr uint32_t kKyubuPacketId = 0x4B594255u;
inline constexpr uint32_t kMaxChannelName = 255;
// The largest valid value of the length field: the name length, the longest name and the largest payload.
inline constexpr uint32_t kMaxFrameLength = 2 + kMaxChannelName + KY_CHANNEL_MAX_PAYLOAD;
// What one connection may have queued and not yet written.
inline constexpr size_t kMaxQueuedBytes = 4u * 1024 * 1024;

// The whole packet, id first. Empty when the name is empty, longer than kMaxChannelName, or the payload is over the limit.
std::vector<uint8_t> encode_frame(const std::string& name, const uint8_t* data, uint32_t len);

enum class FrameRead { ok, io_error, malformed };
// Reads one packet after its id. `read` must fill exactly n bytes or return false. A malformed packet (a length out of range, a
// name length that does not fit, an empty name or one without ':') leaves the stream unusable: the caller ends the connection.
FrameRead read_frame(const std::function<bool(void*, uint32_t)>& read, std::string* name, std::vector<uint8_t>* payload);

// The connection id a server gives a joined player: its creature id + 1 (players are 1 to 10, so 2 to 11; 0 is the server and 1
// the local client of single-player).
inline uint32_t conn_for_creature(int64_t id) { return (uint32_t)(id + 1); }

class NetTransport {
public:
    static NetTransport& get();

    using Writer = std::function<bool(const uint8_t*, size_t)>;   // writes all the bytes, false on a failure

    // Set by the hook installer: the hooks of this host are in place.
    void set_installed(bool on) { installed_ = on; }
    bool installed() const { return installed_; }
    // Tests: the gate value the transport sees (-1: the join gate's own).
    void force_gate_for_tests(int64_t v) { forced_gate_ = v; }

    // Channels::send for a joined game. KY_E_NO_REMOTE when no Kyubu peer can get it (no hooks, no gate, not joined, or the
    // server VM of a joined Cube.exe); KY_E_NOT_FOUND for an unknown connection; KY_E_TOO_LARGE when its queue is full.
    KyubuResult send(KyubuSide from, const std::string& name, uint32_t conn, const void* data, uint32_t len);

    // A send pass of the connection `conn` (KY_CONN_SERVER on Cube.exe) on socket `s`: registers the connection and writes what
    // is queued for it, in order. A new socket for a known id (a player who left and one who joined) drops the old queue.
    void pass(uint32_t conn, uintptr_t s, const Writer& write);
    // A received packet, already read: queued for the receiving side's next update.
    void received(KyubuSide side, uint32_t from_conn, std::string name, std::vector<uint8_t> payload);
    // The socket closed (server) or the client left the server: its connection and queue are gone.
    void forget_socket(uintptr_t s);
    void reset();   // the client's connect and disconnect; tests

    // For tests and the log.
    size_t queued(uint32_t conn) const;
    std::vector<uint32_t> connections() const;
    uint64_t frames_sent() const { return frames_sent_; }
    uint64_t frames_received() const { return frames_received_; }

private:
    struct Peer {
        uintptr_t socket = 0;
        std::deque<std::vector<uint8_t>> out;
        size_t bytes = 0;
        bool warned = false;
    };
    bool can_send(KyubuSide from) const;
    mutable std::mutex mu_;
    std::map<uint32_t, Peer> peers_;   // the server's joined players; the client's one entry, KY_CONN_SERVER
    bool installed_ = false;
    int64_t forced_gate_ = -1;
    uint64_t frames_sent_ = 0, frames_received_ = 0;
};

// Boot: the four mid hooks of this host, when the net_transport primitive is available (the log says why when not).
void install_net_transport_hooks();
void remove_net_transport_hooks();   // tests

}  // namespace kyubu
