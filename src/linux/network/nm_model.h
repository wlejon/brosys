// Pure translation of NetworkManager's object tree (as GetManagedObjects /
// PropertiesChanged deliver it) into the public network types.
#pragma once

#include "brosys/network.h"
#include "linux/dbus/value.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace brosys::nm {

using Props = std::map<std::string, dbus::Value>;        // property -> unwrapped value
using Object = std::map<std::string, Props>;             // interface -> properties
using Objects = std::map<std::string, Object>;           // object path -> interfaces

inline constexpr const char* kService = "org.freedesktop.NetworkManager";
inline constexpr const char* kRoot = "/org/freedesktop";  // the ObjectManager
inline constexpr const char* kPath = "/org/freedesktop/NetworkManager";
inline constexpr const char* kIface = "org.freedesktop.NetworkManager";
inline constexpr const char* kDevice = "org.freedesktop.NetworkManager.Device";
inline constexpr const char* kWired = "org.freedesktop.NetworkManager.Device.Wired";
inline constexpr const char* kWireless = "org.freedesktop.NetworkManager.Device.Wireless";
inline constexpr const char* kActive = "org.freedesktop.NetworkManager.Connection.Active";
inline constexpr const char* kIp4 = "org.freedesktop.NetworkManager.IP4Config";
inline constexpr const char* kIp6 = "org.freedesktop.NetworkManager.IP6Config";
inline constexpr const char* kAccessPoint = "org.freedesktop.NetworkManager.AccessPoint";

// The NetworkState the tree describes (devices in Manager.Devices order).
NetworkState build_state(const Objects& objects);

// Access points of one Wi-Fi device (empty when it is not one).
std::vector<WifiAccessPoint> access_points(const Objects& objects, const std::string& device_path);

// Paths of the Wi-Fi devices in Manager.Devices order.
std::vector<std::string> wifi_devices(const Objects& objects);

// Device.Wireless.LastScan (ms, CLOCK_BOOTTIME; -1 never), or nullopt when not Wi-Fi.
std::optional<int64_t> last_scan(const Objects& objects, const std::string& device_path);

Connectivity connectivity_from_nm(uint32_t v);
LinkType link_type_from_device_type(uint32_t t);
LinkType link_type_from_connection_type(const std::string& t);
LinkState link_state_from_device_state(uint32_t s);
LinkState link_state_from_active_state(uint32_t s);
WifiSecurity security_from_flags(uint32_t flags, uint32_t wpa_flags, uint32_t rsn_flags);
uint32_t channel_from_frequency(uint32_t mhz);

// Applies one PropertiesChanged (interface, a{sv}) to the object at `path`
// (created when absent). Returns false when nothing changed.
bool apply_properties_changed(Objects& objects, const std::string& path, const std::string& iface,
                              const dbus::Value& changed);
// Parses a{oa{sa{sv}}} / one InterfacesAdded (o, a{sa{sv}}) into the tree.
void merge_managed_objects(Objects& objects, const dbus::Value& managed);
void merge_interfaces(Objects& objects, const std::string& path, const dbus::Value& ifaces);

}  // namespace brosys::nm
