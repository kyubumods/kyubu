#include "doctest.h"

#include "channels.h"
#include "loader.h"
#include "modmanager.h"

using namespace kyubu;

namespace {
std::string g_client_got, g_server_got;
bool g_flag = false;

void KYUBU_CALL on_server(uint32_t conn, const uint8_t* d, uint32_t n, void*) {
    g_server_got.assign((const char*)d, n);
    g_flag = true;
    Channels::get().send(nullptr, KY_SIDE_SERVER, "demo:echo", conn, "pong", 4);
}
void KYUBU_CALL on_client(uint32_t, const uint8_t* d, uint32_t n, void*) { g_client_got.assign((const char*)d, n); }
}  // namespace

TEST_CASE("sides-and-messaging: loopback channels") {
    // covers: sides-and-messaging/Round trip
    // covers: sides-and-messaging/No synchronous delivery
    Loader& l = L();
    l.host.host = KY_HOST_CLIENT;
    l.mode = KY_MODE_MENU_OR_SINGLEPLAYER;
    Mod demo;
    demo.id = "demo";
    demo.state = ModState::Loaded;
    auto& ch = Channels::get();
    CHECK(ch.reg(&demo, KY_SIDE_SERVER, "demo:echo", on_server, nullptr) == KY_OK);
    CHECK(ch.reg(&demo, KY_SIDE_CLIENT, "demo:echo", on_client, nullptr) == KY_OK);
    CHECK(ch.reg(&demo, KY_SIDE_CLIENT, "other:echo", on_client, nullptr) == KY_E_NAMESPACE);

    char buf[5] = "ping";
    CHECK(ch.send(&demo, KY_SIDE_CLIENT, "demo:echo", KY_CONN_SERVER, buf, 4) == KY_OK);
    memcpy(buf, "XXXX", 4);          // the sender reusing its buffer must not change the message
    CHECK_FALSE(g_flag);              // not delivered inside send()
    ch.pump(KY_SIDE_SERVER);
    CHECK(g_flag);
    CHECK(g_server_got == "ping");
    CHECK(g_client_got.empty());      // the reply waits for the client's next update
    ch.pump(KY_SIDE_CLIENT);
    CHECK(g_client_got == "pong");

    std::vector<uint8_t> big(KY_CHANNEL_MAX_PAYLOAD + 1);
    CHECK(ch.send(&demo, KY_SIDE_CLIENT, "demo:echo", KY_CONN_SERVER, big.data(), (uint32_t)big.size()) == KY_E_TOO_LARGE);
    // covers: sides-and-messaging/Client code tries an authoritative change (channel direction rules)
    CHECK(ch.send(&demo, KY_SIDE_CLIENT, "demo:echo", KY_CONN_ALL_CLIENTS, "x", 1) == KY_E_SIDE);
    CHECK(ch.send(&demo, KY_SIDE_SERVER, "demo:echo", KY_CONN_SERVER, "x", 1) == KY_E_SIDE);

    // remote client mode: no Kyubu transport to a vanilla server
    l.mode = KY_MODE_REMOTE_CLIENT;
    CHECK(ch.send(&demo, KY_SIDE_CLIENT, "demo:echo", KY_CONN_SERVER, "x", 1) == KY_E_NO_REMOTE);
    l.mode = KY_MODE_MENU_OR_SINGLEPLAYER;
    ch.remove_mod(&demo);
}
