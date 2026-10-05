#pragma once

#include "brosys/bluetooth.h"
#include "linux/dbus/value.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace brosys::bluez {

using Props = std::map<std::string, dbus::Value>;
using Object = std::map<std::string, Props>;
using Objects = std::map<std::string, Object>;

inline constexpr const char* kService = "org.bluez";
inline constexpr const char* kRoot = "/";
inline constexpr const char* kAdapter = "org.bluez.Adapter1";
inline constexpr const char* kDevice = "org.bluez.Device1";

std::optional<BluetoothAdapter> parse_adapter(const std::string& path, const Props& props);
std::optional<BluetoothDevice> parse_device(const std::string& path, const Props& props);

std::vector<BluetoothAdapter> build_adapters(const Objects& objects);
std::vector<BluetoothDevice> build_devices(const Objects& objects);

void merge_managed_objects(Objects& objects, const dbus::Value& managed);
void merge_interfaces(Objects& objects, const std::string& path, const dbus::Value& ifaces);
bool apply_properties_changed(Objects& objects, const std::string& path, const std::string& iface,
                              const dbus::Value& changed);

}  // namespace brosys::bluez
