#include "brosys/sys.h"
#include "brosys/version.h"
#include "test_common.h"

#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_smoke] brosys version: " << brosys::version_string() << "\n";
    assert(!brosys::version_string().empty());
    assert(brosys::version_major() == BRO_SYS_VERSION_MAJOR);
    assert(brosys::version_minor() == BRO_SYS_VERSION_MINOR);
    assert(brosys::version_patch() == BRO_SYS_VERSION_PATCH);

    // Test SystemServices instance
    auto& sys = brosys::SystemServices::instance();
    assert(!sys.is_initialized());

    bool init_ok = sys.initialize();
    assert(init_ok);
    assert(sys.is_initialized());

    // Access all managers
    auto& power = sys.power();
    auto& audio = sys.audio();
    auto& network = sys.network();
    auto& notifs = sys.notifications();
    auto& tray = sys.tray();

    (void)power;
    (void)audio;
    (void)network;
    (void)notifs;
    (void)tray;

    sys.shutdown();
    assert(!sys.is_initialized());

    std::cout << "[test_smoke] PASSED\n";
    return 0;
}
