// macOS power state: the IOPS power-source list (what pmset -g batt
// prints), the AppleSmartBattery registry entry for energy figures, and the
// root domain's AppleClamshellState for the lid.
#include "mac/cf.h"
#include "mac/power_mac.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>

#include <algorithm>
#include <cmath>

namespace brosys::mac {

namespace {

CFRef<CFMutableDictionaryRef> registry_properties(const char* service_class) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(service_class));
    if (!s) return {};
    CFMutableDictionaryRef props = nullptr;
    IORegistryEntryCreateCFProperties(s, &props, kCFAllocatorDefault, 0);
    IOObjectRelease(s);
    return CFRef<CFMutableDictionaryRef>(props);
}

// mAh at the pack's present voltage (mV) -> Wh.
std::optional<double> watt_hours(std::optional<int64_t> mah, std::optional<int64_t> mv) {
    if (!mah || !mv || *mah < 0 || *mv <= 0) return std::nullopt;
    return static_cast<double>(*mah) * static_cast<double>(*mv) / 1e6;
}

PowerDevice from_source(CFDictionaryRef d, CFDictionaryRef smart) {
    PowerDevice dev;
    std::string type = dict_string(d, CFSTR(kIOPSTypeKey)).value_or("");
    if (type == kIOPSInternalBatteryType) {
        dev.kind = PowerDeviceKind::Battery;
        dev.power_supply = true;
    } else if (type == kIOPSUPSType) {
        dev.kind = PowerDeviceKind::Ups;
        dev.power_supply = true;
    } else {
        dev.kind = PowerDeviceKind::Other;
    }
    if (auto sid = dict_int(d, CFSTR(kIOPSPowerSourceIDKey))) dev.id = "iops:" + std::to_string(*sid);
    else dev.id = "iops:" + dict_string(d, CFSTR(kIOPSNameKey)).value_or("?");

    auto cur = dict_int(d, CFSTR(kIOPSCurrentCapacityKey));
    auto max = dict_int(d, CFSTR(kIOPSMaxCapacityKey));
    if (cur && max && *max > 0) dev.percent = std::clamp(100.0 * static_cast<double>(*cur) / static_cast<double>(*max), 0.0, 100.0);

    const bool charging = dict_bool(d, CFSTR(kIOPSIsChargingKey)).value_or(false);
    const bool charged = dict_bool(d, CFSTR(kIOPSIsChargedKey)).value_or(false);
    const std::string src = dict_string(d, CFSTR(kIOPSPowerSourceStateKey)).value_or("");
    if (charged) dev.state = BatteryState::FullyCharged;
    else if (charging) dev.state = BatteryState::Charging;
    else if (src == kIOPSBatteryPowerValue) dev.state = (cur && *cur == 0) ? BatteryState::Empty : BatteryState::Discharging;
    else if (src == kIOPSACPowerValue) dev.state = BatteryState::PendingCharge;  // on AC, held (optimized charging)

    // Minutes; -1 while the estimate is being calculated.
    if (auto t = dict_int(d, CFSTR(kIOPSTimeToEmptyKey)); t && *t >= 0 && dev.state == BatteryState::Discharging)
        dev.time_to_empty_s = *t * 60;
    if (auto t = dict_int(d, CFSTR(kIOPSTimeToFullChargeKey)); t && *t >= 0 && dev.state == BatteryState::Charging)
        dev.time_to_full_s = *t * 60;

    dev.serial = dict_string(d, CFSTR(kIOPSHardwareSerialNumberKey)).value_or("");
    if (dev.kind == PowerDeviceKind::Battery && smart) {
        dev.model = dict_string(smart, CFSTR("DeviceName")).value_or("");
        dev.vendor = dict_string(smart, CFSTR("Manufacturer")).value_or("");
        if (dev.serial.empty()) dev.serial = dict_string(smart, CFSTR("Serial")).value_or("");
        auto mv = dict_int(smart, CFSTR("Voltage"));
        dev.energy_wh = watt_hours(dict_int(smart, CFSTR("AppleRawCurrentCapacity")), mv);
        dev.energy_full_wh = watt_hours(dict_int(smart, CFSTR("AppleRawMaxCapacity")), mv);
        dev.energy_full_design_wh = watt_hours(dict_int(smart, CFSTR("DesignCapacity")), mv);
        if (auto ma = dict_int(smart, CFSTR("Amperage")); ma && mv && *mv > 0)
            dev.energy_rate_w = std::fabs(static_cast<double>(*ma)) * static_cast<double>(*mv) / 1e6;
    } else {
        dev.model = dict_string(d, CFSTR(kIOPSNameKey)).value_or("");
        dev.vendor = dict_string(d, CFSTR("Manufacturer")).value_or("");
    }
    return dev;
}

void read_lid(PowerState& s) {
    io_service_t root = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
    if (!root) return;
    CFRef<CFTypeRef> v(IORegistryEntryCreateCFProperty(root, CFSTR("AppleClamshellState"), kCFAllocatorDefault, 0));
    IOObjectRelease(root);
    if (!v || CFGetTypeID(v.get()) != CFBooleanGetTypeID()) return;  // no lid (desktop Macs)
    s.lid_present = true;
    s.lid_closed = CFBooleanGetValue(static_cast<CFBooleanRef>(v.get())) != 0;
}

}  // namespace

PowerState read_power_state() {
    PowerState s;
    CFRef<CFTypeRef> info(IOPSCopyPowerSourcesInfo());
    if (info) {
        std::string providing = to_utf8(IOPSGetProvidingPowerSourceType(info.get()));
        if (providing == kIOPMACPowerKey) s.source = PowerSource::AC;
        else if (providing == kIOPMBatteryPowerKey || providing == kIOPMUPSPowerKey) s.source = PowerSource::Battery;

        CFRef<CFArrayRef> list(IOPSCopyPowerSourcesList(info.get()));
        auto smart = registry_properties("AppleSmartBattery");
        for (CFIndex i = 0, n = list ? CFArrayGetCount(list.get()) : 0; i < n; ++i) {
            CFDictionaryRef d = IOPSGetPowerSourceDescription(info.get(), CFArrayGetValueAtIndex(list.get(), i));
            if (!d || !dict_bool(d, CFSTR(kIOPSIsPresentKey)).value_or(true)) continue;
            s.devices.push_back(from_source(d, smart.get()));
        }
    }
    double sum = 0;
    int n = 0;
    for (auto& d : s.devices) {
        if (!d.power_supply) continue;
        if (d.percent) {
            sum += *d.percent;
            ++n;
        }
        if (d.time_to_full_s) s.time_to_full_s = std::max(s.time_to_full_s.value_or(0), *d.time_to_full_s);
    }
    if (n) s.percent = sum / n;
    if (n && s.source == PowerSource::Battery) {
        CFTimeInterval t = IOPSGetTimeRemainingEstimate();  // seconds, or Unknown (-1) / Unlimited (-2)
        if (t >= 0) s.time_to_empty_s = static_cast<int64_t>(t);
    }
    read_lid(s);
    return s;
}

}  // namespace brosys::mac
