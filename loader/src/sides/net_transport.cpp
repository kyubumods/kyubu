#include "net_transport.h"

#include <winsock2.h>

#include <cstddef>

#include "channels.h"
#include "cubeworld/functions.h"
#include "cubeworld/types.h"
#include "game.h"
#include "hooks.h"
#include "join_gate.h"
#include "loader.h"
#include "log.h"
#include "symbols.h"

namespace kyubu {

// ------------------------------------------------------------------ frames

std::vector<uint8_t> encode_frame(const std::string& name, const uint8_t* data, uint32_t len) {
    if (name.empty() || name.size() > kMaxChannelName || len > KY_CHANNEL_MAX_PAYLOAD || (len && !data)) return {};
    const uint32_t length = 2 + (uint32_t)name.size() + len;
    std::vector<uint8_t> out(8 + length);
    uint8_t* p = out.data();
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) *p++ = (uint8_t)(v >> (8 * i)); };
    put32(kKyubuPacketId);
    put32(length);
    *p++ = (uint8_t)name.size();
    *p++ = (uint8_t)(name.size() >> 8);
    memcpy(p, name.data(), name.size());
    p += name.size();
    if (len) memcpy(p, data, len);
    return out;
}

FrameRead read_frame(const std::function<bool(void*, uint32_t)>& read, std::string* name, std::vector<uint8_t>* payload) {
    uint8_t head[6];
    if (!read(head, 4)) return FrameRead::io_error;
    const uint32_t length = head[0] | (head[1] << 8) | (head[2] << 16) | ((uint32_t)head[3] << 24);
    if (length < 3 || length > kMaxFrameLength) return FrameRead::malformed;
    if (!read(head + 4, 2)) return FrameRead::io_error;
    const uint32_t nlen = head[4] | (head[5] << 8);
    if (nlen == 0 || nlen > kMaxChannelName || nlen > length - 2) return FrameRead::malformed;
    name->assign(nlen, '\0');
    if (!read(&(*name)[0], nlen)) return FrameRead::io_error;
    if (name->find(':') == std::string::npos) return FrameRead::malformed;
    payload->assign(length - 2 - nlen, 0);
    if (!payload->empty() && !read(payload->data(), (uint32_t)payload->size())) return FrameRead::io_error;
    return FrameRead::ok;
}

// ------------------------------------------------------------------ queues

NetTransport& NetTransport::get() {
    static NetTransport* t = new NetTransport();
    return *t;
}

bool NetTransport::can_send(KyubuSide from) const {
    const Loader& l = L();
    const uint32_t gate = forced_gate_ >= 0 ? (uint32_t)forced_gate_ : game::join_gate_value();
    if (!installed_ || gate == 0) return false;   // no gate: the peer may be vanilla (D10)
    if (l.host.host == KY_HOST_SERVER) return from == KY_SIDE_SERVER;
    return from == KY_SIDE_CLIENT && l.mode == KY_MODE_REMOTE_CLIENT;   // not the server VM of a joined Cube.exe
}

KyubuResult NetTransport::send(KyubuSide from, const std::string& name, uint32_t conn, const void* data, uint32_t len) {
    if (!can_send(from)) return KY_E_NO_REMOTE;
    std::vector<uint8_t> f = encode_frame(name, (const uint8_t*)data, len);
    if (f.empty()) return name.size() > kMaxChannelName ? KY_E_INVALID_ARG : KY_E_TOO_LARGE;
    std::lock_guard<std::mutex> g(mu_);
    auto push = [&](uint32_t id, Peer& p) {
        if (p.bytes + f.size() > kMaxQueuedBytes) {
            if (!p.warned) KWARN("channel transport: connection %u has %u bytes queued: %s dropped", id, (unsigned)p.bytes, name.c_str());
            p.warned = true;
            return false;
        }
        p.bytes += f.size();
        p.out.push_back(f);
        return true;
    };
    if (L().host.host != KY_HOST_SERVER) {   // the client's one peer; its send thread may not have run yet
        return push(KY_CONN_SERVER, peers_[KY_CONN_SERVER]) ? KY_OK : KY_E_TOO_LARGE;
    }
    if (conn == KY_CONN_ALL_CLIENTS) {
        for (auto& [id, p] : peers_) push(id, p);
        return KY_OK;
    }
    auto it = peers_.find(conn);
    if (it == peers_.end()) return KY_E_NOT_FOUND;
    return push(conn, it->second) ? KY_OK : KY_E_TOO_LARGE;
}

void NetTransport::pass(uint32_t conn, uintptr_t s, const Writer& write) {
    std::deque<std::vector<uint8_t>> batch;
    {
        std::lock_guard<std::mutex> g(mu_);
        Peer& p = peers_[conn];
        if (p.socket != s) {
            if (p.socket && !p.out.empty())
                KINFO("channel transport: connection %u has a new socket: %u frame(s) dropped", conn, (unsigned)p.out.size());
            if (p.socket) p.out.clear();
            p.socket = s;
            p.bytes = 0;
        }
        batch.swap(p.out);
        p.bytes = 0;
        p.warned = false;
    }
    for (auto& f : batch) {
        if (!write(f.data(), f.size())) return;   // the game's next send fails too and ends the connection
        std::lock_guard<std::mutex> g(mu_);
        ++frames_sent_;
    }
}

void NetTransport::received(KyubuSide side, uint32_t from_conn, std::string name, std::vector<uint8_t> payload) {
    {
        std::lock_guard<std::mutex> g(mu_);
        ++frames_received_;
    }
    Channels::get().deliver(side, std::move(name), from_conn, std::move(payload));
}

void NetTransport::forget_socket(uintptr_t s) {
    std::lock_guard<std::mutex> g(mu_);
    for (auto it = peers_.begin(); it != peers_.end();) it = it->second.socket == s ? peers_.erase(it) : std::next(it);
}

void NetTransport::reset() {
    std::lock_guard<std::mutex> g(mu_);
    peers_.clear();
}

size_t NetTransport::queued(uint32_t conn) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = peers_.find(conn);
    return it == peers_.end() ? 0 : it->second.out.size();
}

std::vector<uint32_t> NetTransport::connections() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<uint32_t> out;
    for (auto& [id, p] : peers_) out.push_back(id);
    return out;
}

// ------------------------------------------------------------------ hooks (network threads: copy bytes only)

namespace {

// Frame slots of the four sites (mapping notes of each site).
constexpr int32_t kClientPacketId = -0x1220;      // Net::clientReceiveNextSite
constexpr int32_t kClientRecvDisconnected = -0x11D1;
constexpr int32_t kClientSendFailed = -0x2425;    // Net::clientSendPassSite
constexpr int32_t kServerPacketId = -0x13C8;      // Net::connectionReceiveNextSite
constexpr int32_t kServerRecvDisconnected = -0x13A5;
constexpr size_t kClosureConnections = 4, kClosureSlot = 8;   // both server threads' closures (EDI, EBX)

KyubuId g_hooks[2] = {0, 0};

template <class T> T& at(uintptr_t a) { return *reinterpret_cast<T*>(a); }

bool write_all(uintptr_t s, const uint8_t* p, size_t n) {
    while (n) {
        int r = ::send((SOCKET)s, (const char*)p, (int)n, 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

bool read_all(uintptr_t s, void* buf, uint32_t n) {
    char* p = (char*)buf;
    while (n) {
        int r = ::recv((SOCKET)s, p, (int)n, 0);
        if (r <= 0) return false;
        p += r;
        n -= (uint32_t)r;
    }
    return true;
}

// Reads the packet on `s`; false when the connection must end.
bool read_packet(uintptr_t s, KyubuSide side, uint32_t from_conn) {
    std::string name;
    std::vector<uint8_t> payload;
    FrameRead r = read_frame([s](void* b, uint32_t n) { return read_all(s, b, n); }, &name, &payload);
    if (r == FrameRead::malformed) KERROR("channel transport: a malformed Kyubu packet from connection %u: closing it", from_conn);
    if (r != FrameRead::ok) return false;
    NetTransport::get().received(side, from_conn, std::move(name), std::move(payload));
    return true;
}

// The server closure's cube::Connection, or 0.
uintptr_t closure_connection(uintptr_t closure) {
    const uintptr_t vec = at<uintptr_t>(closure + kClosureConnections);
    if (!vec) return 0;
    const uintptr_t begin = at<uintptr_t>(vec), end = at<uintptr_t>(vec + 4);
    const uint32_t slot = at<uint32_t>(closure + kClosureSlot);
    if (!begin || end < begin || slot >= (end - begin) / 4) return 0;
    return at<uintptr_t>(begin + 4 * slot);
}

void on_client_receive(KyubuCpuContext* ctx) {
    if (at<uint32_t>(ctx->ebp + kClientPacketId) != kKyubuPacketId) return;
    at<uint32_t>(ctx->ebp + kClientPacketId) = 0;
    const uintptr_t gc = at<uintptr_t>(ctx->ebx);
    if (!gc || !read_packet(at<uint32_t>(gc + offsetof(cube_GameController, socket)), KY_SIDE_CLIENT, KY_CONN_SERVER))
        at<uint8_t>(ctx->ebp + kClientRecvDisconnected) = 1;
}

void on_client_send(KyubuCpuContext* ctx) {
    if (at<uint8_t>(ctx->ebp + kClientSendFailed)) return;
    const uintptr_t gc = at<uintptr_t>(ctx->edi);
    if (!gc) return;
    const uintptr_t s = at<uint32_t>(gc + offsetof(cube_GameController, socket));
    NetTransport::get().pass(KY_CONN_SERVER, s, [s](const uint8_t* p, size_t n) { return write_all(s, p, n); });
}

void on_server_receive(KyubuCpuContext* ctx) {
    if (at<uint32_t>(ctx->ebp + kServerPacketId) != kKyubuPacketId) return;
    at<uint32_t>(ctx->ebp + kServerPacketId) = 0;
    const uintptr_t c = closure_connection(ctx->edi);
    const uint32_t conn = c ? conn_for_creature(at<int64_t>(c + offsetof(cube_Connection, creature_id))) : 0;
    if (!c || !read_packet(at<uint32_t>(c + offsetof(cube_Connection, socket)), KY_SIDE_SERVER, conn))
        at<uint8_t>(ctx->ebp + kServerRecvDisconnected) = 1;
}

void on_server_send(KyubuCpuContext* ctx) {
    const uintptr_t c = closure_connection(ctx->ebx);
    if (!c) return;
    const uintptr_t s = at<uint32_t>(c + offsetof(cube_Connection, socket));
    const uint32_t conn = conn_for_creature(at<int64_t>(c + offsetof(cube_Connection, creature_id)));
    NetTransport::get().pass(conn, s, [s](const uint8_t* p, size_t n) { return write_all(s, p, n); });
}

}  // namespace

void install_net_transport_hooks() {
    if (g_hooks[0]) return;
    std::string why;
    if (!game::available(game::Prim::net_transport, &why)) {
        KINFO("channel transport: off (channels to a joined game answer KY_E_NO_REMOTE): %s", why.c_str());
        return;
    }
    const bool server = L().host.host == KY_HOST_SERVER;
    const KyubuSide side = server ? KY_SIDE_SERVER : KY_SIDE_CLIENT;
    const char* recv_site = server ? CW_SYM_Net_connectionReceiveNextSite : CW_SYM_Net_clientReceiveNextSite;
    const char* send_site = server ? CW_SYM_connection_send_pass_site : CW_SYM_Net_clientSendPassSite;
    const uintptr_t r = verified_symbol(recv_site, &why);
    const uintptr_t s = r ? verified_symbol(send_site, &why) : 0;
    if (!r || !s ||
        Hooks::get().hook_mid("kyubu", r, side, server ? on_server_receive : on_client_receive, &g_hooks[0], &why) != KY_OK ||
        Hooks::get().hook_mid("kyubu", s, side, server ? on_server_send : on_client_send, &g_hooks[1], &why) != KY_OK) {
        KERROR("channel transport: off: %s", why.c_str());
        remove_net_transport_hooks();
        return;
    }
    NetTransport::get().set_installed(true);
    KINFO("channel transport: installed at %s and %s (Kyubu packet 0x%08X)", recv_site, send_site, (unsigned)kKyubuPacketId);
}

void remove_net_transport_hooks() {
    for (KyubuId& h : g_hooks) {
        if (h) Hooks::get().remove("kyubu", h);
        h = 0;
    }
    NetTransport::get().set_installed(false);
    NetTransport::get().reset();
}

}  // namespace kyubu
