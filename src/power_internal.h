#pragma once

#include "brosys/power.h"
#include <memory>

namespace brosys {

class IPowerBackend {
public:
    virtual ~IPowerBackend() = default;
    virtual BatteryInfo get_battery_info() = 0;
    virtual bool can_suspend() = 0;
    virtual bool can_hibernate() = 0;
    virtual bool can_reboot() = 0;
    virtual bool can_power_off() = 0;
    virtual bool can_lock() = 0;
    virtual bool suspend(bool dry_run) = 0;
    virtual bool hibernate(bool dry_run) = 0;
    virtual bool reboot(bool dry_run) = 0;
    virtual bool power_off(bool dry_run) = 0;
    virtual bool lock(bool dry_run) = 0;
};

std::unique_ptr<IPowerBackend> create_platform_power_backend();

} // namespace brosys
