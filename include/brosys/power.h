#pragma once

#include "brosys/export.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace brosys {

enum class PowerSource {
    Unknown = 0,
    Battery = 1,
    AC = 2
};

enum class BatteryState {
    Unknown = 0,
    Charging = 1,
    Discharging = 2,
    NotCharging = 3,
    Full = 4
};

struct BatteryInfo {
    bool has_battery = false;
    PowerSource source = PowerSource::Unknown;
    BatteryState state = BatteryState::Unknown;
    float percentage = -1.0f; // 0.0f - 100.0f, or -1.0f if unknown
    int estimated_seconds_remaining = -1; // -1 if unknown
    int time_to_empty_seconds = -1;
    int time_to_full_seconds = -1;
    float energy_rate_watts = 0.0f;
    std::string model;
    std::string technology;

    bool operator==(const BatteryInfo& other) const = default;
};

class BROSYS_API PowerManager {
public:
    using BatteryCallback = std::function<void(const BatteryInfo&)>;

    PowerManager();
    ~PowerManager();

    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;
    PowerManager(PowerManager&&) noexcept;
    PowerManager& operator=(PowerManager&&) noexcept;

    [[nodiscard]] BatteryInfo get_battery_info() const;

    [[nodiscard]] bool can_suspend() const;
    [[nodiscard]] bool can_hibernate() const;
    [[nodiscard]] bool can_reboot() const;
    [[nodiscard]] bool can_power_off() const;
    [[nodiscard]] bool can_lock() const;

    bool suspend(bool dry_run = false);
    bool hibernate(bool dry_run = false);
    bool reboot(bool dry_run = false);
    bool power_off(bool dry_run = false);
    bool lock(bool dry_run = false);

    void register_battery_callback(BatteryCallback cb);
    void start_monitoring(int interval_ms = 2000);
    void stop_monitoring();
    [[nodiscard]] bool is_monitoring() const;

    // Testing / simulation support
    void set_mock_battery(const BatteryInfo& info);
    void clear_mock_battery();
    [[nodiscard]] bool is_mocked() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Convenience free functions
namespace power {
    BROSYS_API BatteryInfo get_battery_info();
    BROSYS_API bool can_suspend();
    BROSYS_API bool can_hibernate();
    BROSYS_API bool can_reboot();
    BROSYS_API bool can_power_off();
    BROSYS_API bool can_lock();
    BROSYS_API bool suspend(bool dry_run = false);
    BROSYS_API bool hibernate(bool dry_run = false);
    BROSYS_API bool reboot(bool dry_run = false);
    BROSYS_API bool power_off(bool dry_run = false);
    BROSYS_API bool lock(bool dry_run = false);
} // namespace power

} // namespace brosys
