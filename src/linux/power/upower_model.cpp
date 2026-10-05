#include "linux/power/upower_model.h"

#include <algorithm>
#include <cmath>

namespace brosys::upower {

namespace {

const dbus::Value* find(const Props& p, const char* name) {
    auto it = p.find(name);
    return it == p.end() ? nullptr : &it->second;
}

uint64_t get_uint(const Props& p, const char* name, uint64_t fallback = 0) {
    auto* v = find(p, name);
    return v ? v->as_uint(fallback) : fallback;
}

int64_t get_int(const Props& p, const char* name, int64_t fallback = 0) {
    auto* v = find(p, name);
    return v ? v->as_int(fallback) : fallback;
}

double get_double(const Props& p, const char* name, double fallback = 0) {
    auto* v = find(p, name);
    return v ? v->as_double(fallback) : fallback;
}

bool get_bool(const Props& p, const char* name, bool fallback) {
    auto* v = find(p, name);
    return v ? v->as_bool(fallback) : fallback;
}

std::string get_string(const Props& p, const char* name) {
    auto* v = find(p, name);
    return v ? v->as_string() : std::string();
}

}  // namespace

PowerDeviceKind kind_from_type(uint32_t type) {
    switch (type) {
        case 2: return PowerDeviceKind::Battery;
        case 3: return PowerDeviceKind::Ups;
        case 5: return PowerDeviceKind::Mouse;
        case 6: return PowerDeviceKind::Keyboard;
        case 8: return PowerDeviceKind::Phone;
        case 10: return PowerDeviceKind::Tablet;
        case 12: return PowerDeviceKind::Gamepad;    // gaming input
        case 17:                                      // headset
        case 19: return PowerDeviceKind::Headset;     // headphones
        default: return PowerDeviceKind::Other;
    }
}

BatteryState state_from_upower(uint32_t state) {
    switch (state) {
        case 1: return BatteryState::Charging;
        case 2: return BatteryState::Discharging;
        case 3: return BatteryState::Empty;
        case 4: return BatteryState::FullyCharged;
        case 5: return BatteryState::PendingCharge;
        case 6: return BatteryState::PendingDischarge;
        default: return BatteryState::Unknown;
    }
}

BatteryTechnology technology_from_upower(uint32_t tech) {
    switch (tech) {
        case 1: return BatteryTechnology::LithiumIon;
        case 2: return BatteryTechnology::LithiumPolymer;
        case 3: return BatteryTechnology::LithiumIronPhosphate;
        case 4: return BatteryTechnology::LeadAcid;
        case 5: return BatteryTechnology::NickelCadmium;
        case 6: return BatteryTechnology::NickelMetalHydride;
        default: return BatteryTechnology::Unknown;
    }
}

std::optional<PowerDevice> device_from_props(const std::string& path, const Props& p) {
    if (path == kDisplayDevice) return std::nullopt;
    uint32_t type = static_cast<uint32_t>(get_uint(p, "Type"));
    if (type == kTypeLinePower || type == 0) return std::nullopt;  // line power / not yet typed
    if (!get_bool(p, "IsPresent", true)) return std::nullopt;

    PowerDevice d;
    d.id = path;
    d.kind = kind_from_type(type);
    d.power_supply = get_bool(p, "PowerSupply", false);
    d.state = state_from_upower(static_cast<uint32_t>(get_uint(p, "State")));
    d.technology = technology_from_upower(static_cast<uint32_t>(get_uint(p, "Technology")));
    if (find(p, "Percentage")) d.percent = std::clamp(get_double(p, "Percentage"), 0.0, 100.0);
    if (int64_t t = get_int(p, "TimeToEmpty"); t > 0) d.time_to_empty_s = t;
    if (int64_t t = get_int(p, "TimeToFull"); t > 0) d.time_to_full_s = t;
    // Energy figures are meaningful only for batteries that report a capacity
    // (peripherals report just a percentage and zeros here).
    double full = get_double(p, "EnergyFull");
    if (full > 0) {
        d.energy_wh = get_double(p, "Energy");
        d.energy_full_wh = full;
        d.energy_rate_w = std::fabs(get_double(p, "EnergyRate"));
    }
    if (double design = get_double(p, "EnergyFullDesign"); design > 0) d.energy_full_design_wh = design;
    d.vendor = get_string(p, "Vendor");
    d.model = get_string(p, "Model");
    d.serial = get_string(p, "Serial");
    return d;
}

void aggregate(PowerState& s) {
    s.percent.reset();
    s.time_to_empty_s.reset();
    s.time_to_full_s.reset();
    std::vector<const PowerDevice*> bats;
    for (auto& d : s.devices)
        if (d.power_supply && d.kind == PowerDeviceKind::Battery) bats.push_back(&d);
    if (bats.empty()) return;
    if (bats.size() == 1) {
        s.percent = bats[0]->percent;
        s.time_to_empty_s = bats[0]->time_to_empty_s;
        s.time_to_full_s = bats[0]->time_to_full_s;
        return;
    }

    bool energies = true;
    double energy = 0, full = 0, rate = 0, pct_sum = 0;
    int pct_n = 0;
    bool discharging = false, charging = false;
    for (auto* b : bats) {
        if (b->energy_wh && b->energy_full_wh && *b->energy_full_wh > 0) {
            energy += *b->energy_wh;
            full += *b->energy_full_wh;
            rate += b->energy_rate_w.value_or(0);
        } else {
            energies = false;
        }
        if (b->percent) {
            pct_sum += *b->percent;
            ++pct_n;
        }
        discharging |= b->state == BatteryState::Discharging;
        charging |= b->state == BatteryState::Charging;
    }
    if (energies && full > 0)
        s.percent = std::clamp(100.0 * energy / full, 0.0, 100.0);
    else if (pct_n > 0)
        s.percent = pct_sum / pct_n;

    if (energies && rate > 0) {
        if (discharging) s.time_to_empty_s = static_cast<int64_t>(3600.0 * energy / rate);
        else if (charging) s.time_to_full_s = static_cast<int64_t>(3600.0 * (full - energy) / rate);
        return;
    }
    // No usable rate: the longest per-battery estimate is the best we have.
    for (auto* b : bats) {
        if (b->time_to_empty_s && (!s.time_to_empty_s || *b->time_to_empty_s > *s.time_to_empty_s))
            s.time_to_empty_s = b->time_to_empty_s;
        if (b->time_to_full_s && (!s.time_to_full_s || *b->time_to_full_s > *s.time_to_full_s))
            s.time_to_full_s = b->time_to_full_s;
    }
}

Availability availability_from_logind(const std::string& answer) {
    if (answer == "yes") return Availability::Yes;
    if (answer == "challenge") return Availability::NeedsAuth;
    if (answer == "no" || answer == "na") return Availability::No;
    return Availability::Unknown;
}

std::string inhibit_what(uint32_t bits) {
    std::string out;
    auto add = [&](uint32_t bit, const char* name) {
        if (!(bits & bit)) return;
        if (!out.empty()) out += ':';
        out += name;
    };
    add(inhibit::Sleep, "sleep");
    add(inhibit::Idle, "idle");
    add(inhibit::Shutdown, "shutdown");
    add(inhibit::LidSwitch, "handle-lid-switch");
    add(inhibit::PowerKey, "handle-power-key");
    return out;
}

void merge_changed(Props& into, const dbus::Value& changed) {
    for (auto& entry : changed.items()) {
        auto& kv = entry.items();
        if (kv.size() == 2) into[kv[0].as_string()] = kv[1].unwrap();
    }
}

}  // namespace brosys::upower
