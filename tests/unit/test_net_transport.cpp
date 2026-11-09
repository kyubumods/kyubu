// The channel transport of a joined game (openspec add-skills-unleashed-multiplayer 10.4, design D10): framing, the 64 KiB limit,
// order, delivery at the next update, and no Kyubu packet without the join gate.
#include "doctest.h"

#include "channels.h"
#include "loader.h"
#include "modmanager.h"
#include "net_transport.h"

using namespace kyubu;

namespace {

struct Wire {   // what a send pass wrote, read back as a socket would
    std::vector<uint8_t> bytes;
    size_t at = 0;
    bool read(void* b, uint32_t n) {
        if (bytes.size() - at < n) return false;
        memcpy(b, bytes.data() + at, n);
        at += n;
        return true;
    }
    NetTransport::Writer writer() {
        return [this](const uint8_t* p, size_t n) { bytes.insert(bytes.end(), p, p + n); return true; };
    }
};

// Reads every packet on the wire into `side`, as the receive hook does; returns how many.
int feed(Wire& w, KyubuSide side, uint32_t from) {
    int n = 0;
    while (w.at < w.bytes.size()) {
        uint32_t id = 0;
        REQUIRE(w.read(&id, 4));
        REQUIRE(id == kKyubuPacketId);
        std::string name;
        std::vector<uint8_t> payload;
        REQUIRE(read_frame([&](void* b, uint32_t k) { return w.read(b, k); }, &name, &payload) == FrameRead::ok);
        NetTransport::get().received(side, from, name, payload);
        ++n;
    }
    return n;
}

std::vector<std::string> g_got;
std::vector<uint32_t> g_from;
void KYUBU_CALL on_msg(uint32_t conn, const uint8_t* d, uint32_t n, void*) {
    g_got.emplace_back((const char*)d, n);
    g_from.push_back(conn);
}

struct Setup {
    Mod mod;
    explicit Setup(KyubuHost host, KyubuHostMode mode) {
        L().host.host = host;
        L().mode = mode;
        mod.id = "demo";
        mod.state = ModState::Loaded;
        NetTransport::get().reset();
        NetTransport::get().set_installed(true);
        NetTransport::get().force_gate_for_tests(0x4B4C0001);
        g_got.clear();
        g_from.clear();
    }
    ~Setup() {
        Channels::get().remove_mod(&mod);
        NetTransport::get().reset();
        NetTransport::get().set_installed(false);
        NetTransport::get().force_gate_for_tests(-1);
        L().host.host = KY_HOST_CLIENT;
        L().mode = KY_MODE_MENU_OR_SINGLEPLAYER;
    }
};

}  // namespace

TEST_CASE("net transport: a frame round trip and its layout") {
    const uint8_t data[3] = {1, 2, 3};
    auto f = encode_frame("demo:x", data, 3);
    REQUIRE(f.size() == 4 + 4 + 2 + 6 + 3);
    CHECK(f[0] == 0x55); CHECK(f[1] == 0x42); CHECK(f[2] == 0x59); CHECK(f[3] == 0x4B);   // "UBYK"
    CHECK(f[4] == 11); CHECK(f[5] == 0); CHECK(f[8] == 6); CHECK(f[9] == 0);
    Wire w;
    w.bytes = f;
    w.at = 4;
    std::string name;
    std::vector<uint8_t> payload;
    CHECK(read_frame([&](void* b, uint32_t n) { return w.read(b, n); }, &name, &payload) == FrameRead::ok);
    CHECK(name == "demo:x");
    CHECK(payload == std::vector<uint8_t>{1, 2, 3});
    CHECK(w.at == w.bytes.size());

    auto empty = encode_frame("demo:x", nullptr, 0);   // an empty payload is a message too
    w.bytes = empty;
    w.at = 4;
    CHECK(read_frame([&](void* b, uint32_t n) { return w.read(b, n); }, &name, &payload) == FrameRead::ok);
    CHECK(payload.empty());
}

TEST_CASE("net transport: 64 KiB is the limit, both ways") {
    std::vector<uint8_t> big(KY_CHANNEL_MAX_PAYLOAD);
    CHECK_FALSE(encode_frame("demo:x", big.data(), (uint32_t)big.size()).empty());
    CHECK(encode_frame("demo:x", big.data(), (uint32_t)big.size() + 0).size() == 8 + 2 + 6 + KY_CHANNEL_MAX_PAYLOAD);
    big.push_back(0);
    CHECK(encode_frame("demo:x", big.data(), (uint32_t)big.size()).empty());
    CHECK(encode_frame(std::string(kMaxChannelName + 1, 'a'), nullptr, 0).empty());
    CHECK(encode_frame("", nullptr, 0).empty());

    // a length above the largest valid frame, a name that does not fit, an empty name, a name without ':'
    auto bad = [](std::vector<uint8_t> after_id) {
        size_t at = 0;
        std::string n;
        std::vector<uint8_t> p;
        return read_frame([&](void* b, uint32_t k) {
            if (after_id.size() - at < k) return false;
            memcpy(b, after_id.data() + at, k);
            at += k;
            return true;
        }, &n, &p);
    };
    const uint32_t over = kMaxFrameLength + 1;
    CHECK(bad({(uint8_t)over, (uint8_t)(over >> 8), (uint8_t)(over >> 16), 0}) == FrameRead::malformed);
    CHECK(bad({4, 0, 0, 0, 5, 0, 'a', 'b'}) == FrameRead::malformed);
    CHECK(bad({4, 0, 0, 0, 0, 0, 'a', 'b'}) == FrameRead::malformed);
    CHECK(bad({4, 0, 0, 0, 2, 0, 'a', 'b'}) == FrameRead::malformed);
    CHECK(bad({4, 0, 0, 0, 2, 0, 'a'}) == FrameRead::io_error);   // the connection closed in the middle

    Setup s(KY_HOST_CLIENT, KY_MODE_REMOTE_CLIENT);
    std::vector<uint8_t> over_limit(KY_CHANNEL_MAX_PAYLOAD + 1);
    CHECK(Channels::get().send(&s.mod, KY_SIDE_CLIENT, "demo:x", KY_CONN_SERVER, over_limit.data(),
                               (uint32_t)over_limit.size()) == KY_E_TOO_LARGE);
    CHECK(NetTransport::get().queued(KY_CONN_SERVER) == 0);
}

TEST_CASE("net transport: client to server and back, in order, at the next update") {
    auto& ch = Channels::get();
    Wire up;
    {
        Setup s(KY_HOST_CLIENT, KY_MODE_REMOTE_CLIENT);
        CHECK(ch.send(&s.mod, KY_SIDE_CLIENT, "demo:x", KY_CONN_SERVER, "one", 3) == KY_OK);
        CHECK(ch.send(&s.mod, KY_SIDE_CLIENT, "demo:x", KY_CONN_SERVER, "two", 3) == KY_OK);
        CHECK(ch.send(&s.mod, KY_SIDE_CLIENT, "demo:x", KY_CONN_ALL_CLIENTS, "no", 2) == KY_E_SIDE);
        // the server VM of a joined Cube.exe has no remote end
        CHECK(ch.send(&s.mod, KY_SIDE_SERVER, "demo:x", KY_CONN_ALL_CLIENTS, "no", 2) == KY_E_NO_REMOTE);
        CHECK(NetTransport::get().queued(KY_CONN_SERVER) == 2);
        NetTransport::get().pass(KY_CONN_SERVER, 77, up.writer());   // the client's send thread
        CHECK(NetTransport::get().queued(KY_CONN_SERVER) == 0);
        CHECK(NetTransport::get().frames_sent() >= 2);
    }
    Setup s(KY_HOST_SERVER, KY_MODE_DEDICATED);
    REQUIRE(ch.reg(&s.mod, KY_SIDE_SERVER, "demo:x", on_msg, nullptr) == KY_OK);
    CHECK(feed(up, KY_SIDE_SERVER, conn_for_creature(3)) == 2);   // the receive thread of player 3
    CHECK(g_got.empty());                                          // never on the network thread
    ch.pump(KY_SIDE_SERVER);
    REQUIRE(g_got.size() == 2);
    CHECK(g_got[0] == "one");
    CHECK(g_got[1] == "two");
    CHECK(g_from[0] == 4);

    // the answer goes to that connection only once its send thread has run
    CHECK(ch.send(&s.mod, KY_SIDE_SERVER, "demo:x", 4, "back", 4) == KY_E_NOT_FOUND);
    Wire down3, down5;
    NetTransport::get().pass(4, 300, down3.writer());
    NetTransport::get().pass(6, 500, down5.writer());
    CHECK(ch.send(&s.mod, KY_SIDE_SERVER, "demo:x", 4, "back", 4) == KY_OK);
    CHECK(ch.send(&s.mod, KY_SIDE_SERVER, "demo:x", KY_CONN_ALL_CLIENTS, "all", 3) == KY_OK);
    CHECK(ch.send(&s.mod, KY_SIDE_SERVER, "demo:x", KY_CONN_LOCAL_CLIENT, "no", 2) == KY_E_NOT_FOUND);
    NetTransport::get().pass(4, 300, down3.writer());
    NetTransport::get().pass(6, 500, down5.writer());

    Channels::get().remove_mod(&s.mod);
    L().host.host = KY_HOST_CLIENT;
    L().mode = KY_MODE_REMOTE_CLIENT;
    REQUIRE(ch.reg(&s.mod, KY_SIDE_CLIENT, "demo:x", on_msg, nullptr) == KY_OK);
    g_got.clear();
    g_from.clear();
    CHECK(feed(down3, KY_SIDE_CLIENT, KY_CONN_SERVER) == 2);
    CHECK(feed(down5, KY_SIDE_CLIENT, KY_CONN_SERVER) == 1);
    CHECK(g_got.empty());
    ch.pump(KY_SIDE_CLIENT);
    REQUIRE(g_got.size() == 3);
    CHECK(g_got[0] == "back");
    CHECK(g_got[1] == "all");
    CHECK(g_got[2] == "all");
    CHECK(g_from[0] == KY_CONN_SERVER);
}

TEST_CASE("net transport: no Kyubu packet without the gate, the hooks or a joined game") {
    auto& ch = Channels::get();
    Setup s(KY_HOST_CLIENT, KY_MODE_REMOTE_CLIENT);
    NetTransport::get().force_gate_for_tests(0);   // no gate mods: the server may be vanilla
    CHECK(ch.send(&s.mod, KY_SIDE_CLIENT, "demo:x", KY_CONN_SERVER, "x", 1) == KY_E_NO_REMOTE);
    NetTransport::get().force_gate_for_tests(0x4B4C0001);
    NetTransport::get().set_installed(false);      // the sites are not verified on this build
    CHECK(ch.send(&s.mod, KY_SIDE_CLIENT, "demo:x", KY_CONN_SERVER, "x", 1) == KY_E_NO_REMOTE);
    NetTransport::get().set_installed(true);
    Wire w;
    NetTransport::get().pass(KY_CONN_SERVER, 1, w.writer());
    CHECK(w.bytes.empty());

    L().host.host = KY_HOST_SERVER;
    L().mode = KY_MODE_DEDICATED;
    NetTransport::get().pass(4, 300, w.writer());
    NetTransport::get().force_gate_for_tests(0);
    CHECK(ch.send(&s.mod, KY_SIDE_SERVER, "demo:x", 4, "x", 1) == KY_E_NO_REMOTE);
    CHECK(ch.send(&s.mod, KY_SIDE_SERVER, "demo:x", KY_CONN_ALL_CLIENTS, "x", 1) == KY_E_NO_REMOTE);
    NetTransport::get().pass(4, 300, w.writer());
    CHECK(w.bytes.empty());
}

TEST_CASE("net transport: a player who leaves takes the queue along") {
    auto& ch = Channels::get();
    Setup s(KY_HOST_SERVER, KY_MODE_DEDICATED);
    Wire w;
    NetTransport::get().pass(2, 100, w.writer());
    CHECK(ch.send(&s.mod, KY_SIDE_SERVER, "demo:x", 2, "old", 3) == KY_OK);
    NetTransport::get().pass(2, 200, w.writer());   // creature 1 again, another socket: the old player's frame is dropped
    CHECK(w.bytes.empty());
    CHECK(ch.send(&s.mod, KY_SIDE_SERVER, "demo:x", 2, "new", 3) == KY_OK);
    NetTransport::get().forget_socket(200);          // the socket closed
    CHECK(NetTransport::get().connections().empty());
    CHECK(ch.send(&s.mod, KY_SIDE_SERVER, "demo:x", 2, "gone", 4) == KY_E_NOT_FOUND);
}
