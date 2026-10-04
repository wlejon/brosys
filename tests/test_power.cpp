#include "brosys/power.h"
#include "test_common.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    init_test();
    std::cout << "[test_power] Testing power subsystem...\n";

    // 1. Query live system battery info
    auto info = brosys::power::get_battery_info();
    std::cout << "  Battery present: " << (info.has_battery ? "Yes" : "No") << "\n";
    std::cout << "  Percentage: " << info.percentage << "%\n";
    std::cout << "  Power source: " << static_cast<int>(info.source) << "\n";
    std::cout << "  Battery state: " << static_cast<int>(info.state) << "\n";

    // 2. Query capabilities
    bool can_susp = brosys::power::can_suspend();
    bool can_hib = brosys::power::can_hibernate();
    bool can_reb = brosys::power::can_reboot();
    bool can_pwr = brosys::power::can_power_off();
    bool can_lck = brosys::power::can_lock();
    std::cout << "  Capabilities - Suspend: " << can_susp
              << ", Hibernate: " << can_hib
              << ", Reboot: " << can_reb
              << ", PowerOff: " << can_pwr
              << ", Lock: " << can_lck << "\n";

    // 3. Test dry-run actions (must NOT reboot or sleep system!)
    assert(brosys::power::reboot(true) == true);
    assert(brosys::power::power_off(true) == true);
    assert(brosys::power::lock(true) == true);
    if (can_susp) {
        assert(brosys::power::suspend(true) == true);
    }
    if (can_hib) {
        assert(brosys::power::hibernate(true) == true);
    }

    // 4. Test PowerManager instance & Mocking
    brosys::PowerManager mgr;
    assert(!mgr.is_mocked());

    brosys::BatteryInfo mock_info;
    mock_info.has_battery = true;
    mock_info.percentage = 85.5f;
    mock_info.source = brosys::PowerSource::Battery;
    mock_info.state = brosys::BatteryState::Discharging;
    mock_info.estimated_seconds_remaining = 7200;
    mock_info.time_to_empty_seconds = 7200;

    mgr.set_mock_battery(mock_info);
    assert(mgr.is_mocked());

    auto queried = mgr.get_battery_info();
    assert(queried.has_battery == true);
    assert(queried.percentage == 85.5f);
    assert(queried.source == brosys::PowerSource::Battery);
    assert(queried.state == brosys::BatteryState::Discharging);
    assert(queried.estimated_seconds_remaining == 7200);

    // 5. Test monitoring callbacks
    std::atomic<bool> callback_called = false;
    mgr.register_battery_callback([&](const brosys::BatteryInfo& b) {
        if (b.percentage == 85.5f) {
            callback_called.store(true);
        }
    });

    mgr.start_monitoring(50);
    assert(mgr.is_monitoring());

    // Wait briefly for monitor thread to poll
    for (int i = 0; i < 20; ++i) {
        if (callback_called.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(callback_called.load());

    mgr.stop_monitoring();
    assert(!mgr.is_monitoring());

    mgr.clear_mock_battery();
    assert(!mgr.is_mocked());

    std::cout << "[test_power] PASSED\n";
    return 0;
}
