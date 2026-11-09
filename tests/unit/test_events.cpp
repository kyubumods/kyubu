#include "doctest.h"

#include "events.h"
#include "loader.h"

using namespace kyubu;

TEST_CASE("game-events: priority ordering and cancellation") {
    // covers: game-events/Priority ordering (bus part)
    auto& ev = Events::get();
    std::string order;
    ev.subscribe_internal("test.order", KY_SIDE_CLIENT, 0, [&](KyubuEvent*) { order += "a"; });
    ev.subscribe_internal("test.order", KY_SIDE_CLIENT, 10, [&](KyubuEvent*) { order += "b"; });
    ev.subscribe_internal("test.order", KY_SIDE_SERVER, 99, [&](KyubuEvent*) { order += "s"; });
    ev.emit("test.order", KY_SIDE_CLIENT);
    CHECK(order == "ba");

    std::string seen;
    ev.subscribe_internal("test.cancel", KY_SIDE_CLIENT, 5, [&](KyubuEvent* e) { seen += "1"; e->cancelled = 1; });
    ev.subscribe_internal("test.cancel", KY_SIDE_CLIENT, 0, [&](KyubuEvent*) { seen += "2"; });
    CHECK(ev.emit("test.cancel", KY_SIDE_CLIENT, nullptr, 0, true));
    CHECK(seen == "1");
    // a non-cancellable event ignores cancel requests
    seen.clear();
    CHECK_FALSE(ev.emit("test.cancel", KY_SIDE_CLIENT, nullptr, 0, false));
    CHECK(seen == "12");
}

TEST_CASE("game-events: faulting handler does not stop the bus") {
    auto& ev = Events::get();
    int after = 0;
    ev.subscribe_internal("test.fault", KY_SIDE_CLIENT, 10, [](KyubuEvent*) { *(volatile int*)0 = 1; });
    ev.subscribe_internal("test.fault", KY_SIDE_CLIENT, 0, [&](KyubuEvent*) { ++after; });
    ev.emit("test.fault", KY_SIDE_CLIENT);
    CHECK(after == 1);
}

TEST_CASE("game-events: unavailable events refuse subscriptions") {
    auto& ev = Events::get();
    ev.set_unavailable("test.tick", "not on this build");
    KyubuId id = 0;
    CHECK(ev.subscribe(nullptr, "test.tick", KY_SIDE_SERVER, 0, [](KyubuEvent*, void*) {}, nullptr, &id) == KY_E_UNAVAILABLE);
}

TEST_CASE("sides-and-messaging: side activity per host mode") {
    Loader& l = L();
    auto saved = l.host.host;
    auto mode = l.mode;
    l.host.host = KY_HOST_CLIENT;
    l.mode = KY_MODE_MENU_OR_SINGLEPLAYER;
    CHECK(side_active(KY_SIDE_CLIENT));
    CHECK(side_active(KY_SIDE_SERVER));
    l.mode = KY_MODE_REMOTE_CLIENT;
    CHECK(side_active(KY_SIDE_CLIENT));
    CHECK_FALSE(side_active(KY_SIDE_SERVER));
    l.host.host = KY_HOST_SERVER;
    l.mode = KY_MODE_DEDICATED;
    CHECK_FALSE(side_active(KY_SIDE_CLIENT));
    CHECK(side_active(KY_SIDE_SERVER));
    l.host.host = saved;
    l.mode = mode;
}
