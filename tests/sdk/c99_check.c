/* Compiles kyubu_api.h as strict C99 and exercises the entry-point declarations. */
#include "kyubu_api.h"

KYUBU_EXPORT int KYUBU_CALL kyubu_mod_query(KyubuModQuery* q) {
    q->api_version = KYUBU_API_VERSION;
    q->id[0] = 'c'; q->id[1] = '9'; q->id[2] = 0;
    q->sides = KY_SIDE_CLIENT;
    return 0;
}

static void KYUBU_CALL on_frame(KyubuEvent* ev, void* user) { (void)ev; (void)user; }

KYUBU_EXPORT int KYUBU_CALL kyubu_mod_init(const KyubuApi* api, KyubuModHandle self, KyubuSide side) {
    KyubuId id = 0;
    (void)side;
    if (api->size < sizeof(KyubuApi)) return -1;
    return api->subscribe(self, "frame.begin", KY_SIDE_CLIENT, 0, on_frame, 0, &id);
}
