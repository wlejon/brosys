// Pure translation of UPower / logind values into the public power types.
// No bus access: the service feeds property maps in, tests feed them too.
#pragma once

#include "brosys/power.h"
#include "linux/dbus/value.h"

#include <map>
#include <optional>
#include <string>

namespace brosys::upower {

using Props = std::map<std::string, dbus::Value>;  // unwrapped property values

inline constexpr const char* kService = "org.freedesktop.UPower";
inline constexpr const char* kPath = "/org/freedesktop/UPower";
inline constexpr const char* kIface = "org.freedesktop.UPower";
inline constexpr const char* kDeviceIface = "org.freedesktop.UPower.Device";
inline constexpr const char* kDisplayDevice = "/org/freedesktop/UPower/devices/DisplayDevice";

inline constexpr const char* kLogindService = "org.freedesktop.login1";
inline constexpr const char* kLogindPath = "/org/freedesktop/login1";
inline constexpr const char* kLogindManager = "org.freedesktop.login1.Manager";
inline constexpr const char* kLogindSession = "org.freedesktop.login1.Session";
inline constexpr const char* kLogindUser = "org.freedesktop.login1.User";

// UPower device Type values (up-types.h).
inline constexpr uint32_t kTypeLinePower = 1;

// A UPower device's properties -> PowerDevice; nullopt when the device must
// not be listed (line power, not present, the aggregate display device).
std::optional<PowerDevice> device_from_props(const std::string& path, const Props& p);

PowerDeviceKind kind_from_type(uint32_t type);
BatteryState state_from_upower(uint32_t state);
BatteryTechnology technology_from_upower(uint32_t tech);

// Fills percent / time_to_empty_s / time_to_full_s from the power-supply
// batteries in `s.devices` (cleared when there are none).
void aggregate(PowerState& s);

// logind CanX answers: "yes", "no", "challenge", "na" (and anything else).
Availability availability_from_logind(const std::string& answer);

// inhibit:: bits -> logind "what" ("sleep:idle"); "" when no known bit is set.
std::string inhibit_what(uint32_t bits);

// Merges a PropertiesChanged a{sv} (values wrapped in variants) into `into`.
void merge_changed(Props& into, const dbus::Value& changed);

}  // namespace brosys::upower
