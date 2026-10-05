// Power: batteries / UPS / peripheral batteries, the system power source,
// session actions (suspend, hibernate, reboot, power off, lock) and
// inhibitors.
//
// Linux: UPower (devices, on-battery, lid) and logind (CanX / actions /
// inhibitors / PrepareForSleep / PrepareForShutdown), on the system bus.
// Windows: GetSystemPowerStatus + the battery device class (IOCTL_BATTERY_*),
// powrprof capabilities, power requests, and power broadcasts.
#pragma once

#include "brosys/common.h"
#include "brosys/event_queue.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace brosys {

enum class PowerSource { Unknown, AC, Battery };

// What a power device is. Only devices that power the system
// (`power_supply`) feed PowerState::source; peripherals are informational.
enum class PowerDeviceKind { Battery, Ups, Mouse, Keyboard, Headset, Phone, Tablet, Gamepad, Other };

enum class BatteryState {
    Unknown,
    Charging,
    Discharging,
    Empty,
    FullyCharged,
    PendingCharge,     // on AC, not charging yet (charge threshold, warming up)
    PendingDischarge,
};

enum class BatteryTechnology { Unknown, LithiumIon, LithiumPolymer, LithiumIronPhosphate, LeadAcid, NickelCadmium, NickelMetalHydride };

struct PowerDevice {
    std::string id;                 // stable per boot (UPower object path / battery device path)
    PowerDeviceKind kind = PowerDeviceKind::Battery;
    bool power_supply = false;      // powers the whole system (laptop battery, UPS)
    BatteryState state = BatteryState::Unknown;
    BatteryTechnology technology = BatteryTechnology::Unknown;
    std::optional<double> percent;  // 0..100
    std::optional<int64_t> time_to_empty_s;
    std::optional<int64_t> time_to_full_s;
    std::optional<double> energy_wh;
    std::optional<double> energy_full_wh;
    std::optional<double> energy_full_design_wh;
    std::optional<double> energy_rate_w;  // magnitude of charge / discharge rate
    std::string vendor;
    std::string model;
    std::string serial;

    bool operator==(const PowerDevice&) const = default;
};

struct PowerState {
    PowerSource source = PowerSource::Unknown;
    std::vector<PowerDevice> devices;  // real devices only: no aggregate "display device", no line-power entries
    std::optional<double> percent;     // aggregate over power_supply batteries, when any exist
    std::optional<int64_t> time_to_empty_s;
    std::optional<int64_t> time_to_full_s;
    bool lid_present = false;
    bool lid_closed = false;

    bool has_system_battery() const {
        for (auto& d : devices)
            if (d.power_supply && d.kind == PowerDeviceKind::Battery) return true;
        return false;
    }
    bool operator==(const PowerState&) const = default;
};

enum class PowerAction { Suspend, Hibernate, HybridSleep, Reboot, PowerOff, Lock };

struct PowerCapabilities {
    Availability suspend = Availability::Unknown;
    Availability hibernate = Availability::Unknown;
    Availability hybrid_sleep = Availability::Unknown;
    Availability reboot = Availability::Unknown;
    Availability power_off = Availability::Unknown;
    Availability lock = Availability::Unknown;

    Availability of(PowerAction a) const;
    bool operator==(const PowerCapabilities&) const = default;
};

// Bits for InhibitRequest::what.
namespace inhibit {
inline constexpr uint32_t Sleep = 1u << 0;      // suspend / hibernate
inline constexpr uint32_t Idle = 1u << 1;       // idle actions and display sleep
inline constexpr uint32_t Shutdown = 1u << 2;   // power off / reboot
inline constexpr uint32_t LidSwitch = 1u << 3;  // logind handle-lid-switch (Linux only)
inline constexpr uint32_t PowerKey = 1u << 4;   // logind handle-power-key (Linux only)
}  // namespace inhibit

enum class InhibitMode { Block, Delay };

struct InhibitRequest {
    uint32_t what = inhibit::Idle;
    std::string who;   // application name
    std::string why;   // human-readable reason
    InhibitMode mode = InhibitMode::Block;
};

// Held for as long as it lives; destroying it releases the inhibition.
class Inhibitor {
public:
    virtual ~Inhibitor() = default;
};

// ---------------------------------------------------------------- events

// New snapshot after any change (device added/removed/changed, AC plugged,
// lid). Pushed once at start too.
struct PowerChanged {
    PowerState state;
};

struct PowerCapabilitiesChanged {
    PowerCapabilities capabilities;
};

// The system is about to sleep (`starting` true) or has resumed (false).
// A host holding a Delay inhibitor should finish its work and release it.
struct SleepPrepare {
    bool starting = true;
};

// The system is about to shut down / reboot (`starting` true) or the
// shutdown was cancelled (false).
struct ShutdownPrepare {
    bool starting = true;
};

using PowerEvent = std::variant<PowerChanged, PowerCapabilitiesChanged, SleepPrepare, ShutdownPrepare>;
using PowerEventQueue = MessageQueue<PowerEvent>;

struct PowerConfig {
    // Linux: system-bus address override (tests point it at a private bus
    // running a real upowerd); empty = the default system bus.
    std::string system_bus_address;
    // Polling interval for backends that have no change notification for a
    // value (Windows battery rate / time estimates). 0 disables polling.
    uint32_t poll_interval_ms = 10000;
};

class PowerService {
public:
    // Starts the backend thread; the first PowerChanged is queued before
    // create() returns. nullptr + *error when the platform service is absent.
    static std::unique_ptr<PowerService> create(const PowerConfig& config, std::string* error);
    virtual ~PowerService() = default;

    virtual PowerEventQueue& events() = 0;

    virtual PowerState state() const = 0;                // latest snapshot
    virtual PowerCapabilities capabilities() const = 0;  // latest snapshot

    // Performs the action (never on dry runs: callers check capabilities()).
    // Linux requests are interactive (polkit may prompt).
    virtual Result request(PowerAction action) = 0;

    virtual std::unique_ptr<Inhibitor> inhibit(const InhibitRequest& request, std::string* error) = 0;
};

const char* to_string(PowerSource s);
const char* to_string(PowerDeviceKind k);
const char* to_string(BatteryState s);
const char* to_string(BatteryTechnology t);
const char* to_string(PowerAction a);

}  // namespace brosys
