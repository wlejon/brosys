// macOS power internals shared by power.cpp, power_snapshot.cpp,
// power_actions.cpp and workspace.mm.
#pragma once

#include "brosys/power.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace brosys::mac {

// IOPS power sources + the AppleSmartBattery registry entry + the clamshell.
PowerState read_power_state();

// What this process may ask for right now (console session, sleep policy,
// AppleEvent consent towards loginwindow).
PowerCapabilities read_power_capabilities();

// Performs an action whose availability is not No.
Result perform_power_action(PowerAction action);

// NSWorkspaceWillPowerOffNotification (AppKit delivers it to GUI
// applications only). `fn` runs on a private queue until the returned
// handle is destroyed (which waits for a callback in flight).
std::shared_ptr<void> observe_power_off(std::function<void()> fn);

}  // namespace brosys::mac

// Test seam (not public API): drive the IOKit system-power handler with
// synthetic notifications. Synthetic will-sleep notifications are
// "acknowledged" into a list instead of IOAllowPowerChange.
namespace brosys::mac::power_testing {
void deliver_system_power(PowerService& service, uint32_t message, intptr_t notification_id);
std::vector<intptr_t> acknowledged(PowerService& service);
}  // namespace brosys::mac::power_testing
