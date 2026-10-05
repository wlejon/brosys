// Battery devices through the battery device class (IOCTL_BATTERY_*): the
// per-battery data GetSystemPowerStatus aggregates away (chemistry,
// capacities, rate, names, UPS vs system battery).
#pragma once

#include "brosys/power.h"

#include <vector>

namespace brosys::win {

std::vector<PowerDevice> enumerate_batteries();

}  // namespace brosys::win
