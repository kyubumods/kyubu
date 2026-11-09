#include "kyubu.hpp"
struct Fault {
    explicit Fault(kyubu::Mod& m) {
        m.on("frame.begin", [](KyubuEvent&) { *(volatile int*)0 = 42; });
    }
};
KYUBU_MOD("fx_fault_frame", KY_SIDE_CLIENT, Fault)
