#if defined(_WIN32)

#include "power_internal.h"
#include <windows.h>
#include <powrprof.h>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace brosys {

namespace {

bool enable_shutdown_privilege() {
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(hToken);
        return false;
    }

    BOOL res = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
    CloseHandle(hToken);
    return res != FALSE;
}

} // namespace

class WindowsPowerBackend : public IPowerBackend {
public:
    BatteryInfo get_battery_info() override {
        BatteryInfo info{};
        SYSTEM_POWER_STATUS sps{};

        if (!GetSystemPowerStatus(&sps)) {
            return info;
        }

        // Check if a battery is present
        if (sps.BatteryFlag != 128 && sps.BatteryFlag != 255) {
            info.has_battery = true;
        } else if (sps.BatteryFlag == 128) {
            info.has_battery = false;
        } else {
            // 255 unknown status
            info.has_battery = (sps.BatteryLifePercent != 255);
        }

        // Power Source
        if (sps.ACLineStatus == 1) {
            info.source = PowerSource::AC;
        } else if (sps.ACLineStatus == 0) {
            info.source = PowerSource::Battery;
        } else {
            info.source = PowerSource::Unknown;
        }

        // Percentage
        if (sps.BatteryLifePercent != 255 && sps.BatteryLifePercent <= 100) {
            info.percentage = static_cast<float>(sps.BatteryLifePercent);
        } else {
            info.percentage = -1.0f;
        }

        // State
        if (sps.BatteryFlag & 8) {
            info.state = BatteryState::Charging;
        } else if (sps.ACLineStatus == 1) {
            if (info.percentage >= 99.0f) {
                info.state = BatteryState::Full;
            } else {
                info.state = BatteryState::NotCharging;
            }
        } else if (sps.ACLineStatus == 0) {
            info.state = BatteryState::Discharging;
        } else {
            info.state = BatteryState::Unknown;
        }

        // Estimated remaining time
        if (sps.BatteryLifeTime != static_cast<DWORD>(-1)) {
            info.estimated_seconds_remaining = static_cast<int>(sps.BatteryLifeTime);
            if (info.state == BatteryState::Discharging) {
                info.time_to_empty_seconds = info.estimated_seconds_remaining;
            }
        }

        if (sps.BatteryFullLifeTime != static_cast<DWORD>(-1)) {
            // Full life time available
        }

        info.technology = "Lithium";
        return info;
    }

    bool can_suspend() override {
        return IsPwrSuspendAllowed() != FALSE;
    }

    bool can_hibernate() override {
        return IsPwrHibernateAllowed() != FALSE;
    }

    bool can_reboot() override {
        return true;
    }

    bool can_power_off() override {
        return true;
    }

    bool can_lock() override {
        return true;
    }

    bool suspend(bool dry_run) override {
        if (!can_suspend()) return false;
        if (dry_run) return true;
        return SetSuspendState(FALSE, FALSE, FALSE) != FALSE;
    }

    bool hibernate(bool dry_run) override {
        if (!can_hibernate()) return false;
        if (dry_run) return true;
        return SetSuspendState(TRUE, FALSE, FALSE) != FALSE;
    }

    bool reboot(bool dry_run) override {
        if (dry_run) return true;
        if (!enable_shutdown_privilege()) return false;
        return ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG,
                            SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED) != FALSE;
    }

    bool power_off(bool dry_run) override {
        if (dry_run) return true;
        if (!enable_shutdown_privilege()) return false;
        return ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG,
                            SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED) != FALSE;
    }

    bool lock(bool dry_run) override {
        if (dry_run) return true;
        return LockWorkStation() != FALSE;
    }
};

std::unique_ptr<IPowerBackend> create_platform_power_backend() {
    return std::make_unique<WindowsPowerBackend>();
}

} // namespace brosys

#endif // _WIN32
