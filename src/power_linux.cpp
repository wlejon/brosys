#if !defined(_WIN32)

#include "power_internal.h"
#include "dbus_helper.h"
#include <fstream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace brosys {

namespace {

BatteryInfo read_sysfs_battery() {
    BatteryInfo info{};
    std::string base_path;

    for (int i = 0; i < 4; ++i) {
        std::string p = "/sys/class/power_supply/BAT" + std::to_string(i);
        if (fs::exists(p)) {
            base_path = p;
            info.has_battery = true;
            break;
        }
    }

    if (!info.has_battery) {
        // Check for AC
        for (int i = 0; i < 2; ++i) {
            std::string ac = "/sys/class/power_supply/AC" + std::to_string(i);
            if (fs::exists(ac)) {
                std::ifstream f(ac + "/online");
                int online = 0;
                if (f >> online && online == 1) {
                    info.source = PowerSource::AC;
                }
                break;
            }
        }
        return info;
    }

    // Read capacity (percentage)
    {
        std::ifstream f(base_path + "/capacity");
        int cap = 0;
        if (f >> cap) {
            info.percentage = static_cast<float>(cap);
        }
    }

    // Read status
    {
        std::ifstream f(base_path + "/status");
        std::string status;
        if (f >> status) {
            if (status == "Charging") {
                info.state = BatteryState::Charging;
                info.source = PowerSource::AC;
            } else if (status == "Discharging") {
                info.state = BatteryState::Discharging;
                info.source = PowerSource::Battery;
            } else if (status == "Full") {
                info.state = BatteryState::Full;
                info.source = PowerSource::AC;
            } else if (status == "Not charging") {
                info.state = BatteryState::NotCharging;
                info.source = PowerSource::AC;
            }
        }
    }

    // Read model & technology
    {
        std::ifstream f(base_path + "/model_name");
        std::getline(f, info.model);
    }
    {
        std::ifstream f(base_path + "/technology");
        std::getline(f, info.technology);
    }

    return info;
}

} // namespace

class LinuxPowerBackend : public IPowerBackend {
public:
    BatteryInfo get_battery_info() override {
        // Try D-Bus UPower first
        auto bus = dbus::DBusConnection::open(dbus::BusType::System);
        if (bus) {
            dbus::DBusVariant val;
            if (bus->get_property("org.freedesktop.UPower",
                                  "/org/freedesktop/UPower/devices/DisplayDevice",
                                  "org.freedesktop.UPower.Device",
                                  "Percentage", val)) {
                if (std::holds_alternative<double>(val)) {
                    BatteryInfo info{};
                    info.has_battery = true;
                    info.percentage = static_cast<float>(std::get<double>(val));

                    dbus::DBusVariant state_val;
                    if (bus->get_property("org.freedesktop.UPower",
                                          "/org/freedesktop/UPower/devices/DisplayDevice",
                                          "org.freedesktop.UPower.Device",
                                          "State", state_val)) {
                        uint32_t s = 0;
                        if (std::holds_alternative<uint32_t>(state_val)) {
                            s = std::get<uint32_t>(state_val);
                        }
                        switch (s) {
                            case 1: info.state = BatteryState::Charging; info.source = PowerSource::AC; break;
                            case 2: info.state = BatteryState::Discharging; info.source = PowerSource::Battery; break;
                            case 4: info.state = BatteryState::Full; info.source = PowerSource::AC; break;
                            default: info.state = BatteryState::Unknown; break;
                        }
                    }
                    return info;
                }
            }
        }

        // Fallback to sysfs
        return read_sysfs_battery();
    }

    bool can_suspend() override {
        return check_logind_can("CanSuspend");
    }

    bool can_hibernate() override {
        return check_logind_can("CanHibernate");
    }

    bool can_reboot() override {
        return check_logind_can("CanReboot");
    }

    bool can_power_off() override {
        return check_logind_can("CanPowerOff");
    }

    bool can_lock() override {
        return true;
    }

    bool suspend(bool dry_run) override {
        if (dry_run) return can_suspend();
        return call_logind_action("Suspend");
    }

    bool hibernate(bool dry_run) override {
        if (dry_run) return can_hibernate();
        return call_logind_action("Hibernate");
    }

    bool reboot(bool dry_run) override {
        if (dry_run) return can_reboot();
        return call_logind_action("Reboot");
    }

    bool power_off(bool dry_run) override {
        if (dry_run) return can_power_off();
        return call_logind_action("PowerOff");
    }

    bool lock(bool dry_run) override {
        if (dry_run) return true;
        return call_logind_action("LockSession");
    }

private:
    bool check_logind_can(const std::string& method) {
        auto bus = dbus::DBusConnection::open(dbus::BusType::System);
        if (!bus) return false;
        std::vector<dbus::DBusVariant> results;
        dbus::MethodCall call{
            "org.freedesktop.login1",
            "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager",
            method
        };
        return bus->call_method(call, {}, results);
    }

    bool call_logind_action(const std::string& action) {
        auto bus = dbus::DBusConnection::open(dbus::BusType::System);
        if (!bus) return false;
        std::vector<dbus::DBusVariant> results;
        std::vector<dbus::DBusVariant> args;
        args.emplace_back(true); // interactive = true
        dbus::MethodCall call{
            "org.freedesktop.login1",
            "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager",
            action
        };
        return bus->call_method(call, args, results);
    }
};

std::unique_ptr<IPowerBackend> create_platform_power_backend() {
    return std::make_unique<LinuxPowerBackend>();
}

} // namespace brosys

#endif // !_WIN32
