// UDisks2's object tree (ObjectManager GetManagedObjects / InterfacesAdded /
// PropertiesChanged values) translated into the public disk types.
#pragma once

#include "brosys/disks.h"
#include "linux/dbus/value.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace brosys::udisks {

using Props = std::map<std::string, dbus::Value>;
using Object = std::map<std::string, Props>;  // interface -> properties
using Objects = std::map<std::string, Object>;  // object path -> interfaces

inline constexpr const char* kService = "org.freedesktop.UDisks2";
inline constexpr const char* kRoot = "/org/freedesktop/UDisks2";
inline constexpr const char* kDrive = "org.freedesktop.UDisks2.Drive";
inline constexpr const char* kDriveAta = "org.freedesktop.UDisks2.Drive.Ata";
inline constexpr const char* kNVMeController = "org.freedesktop.UDisks2.NVMe.Controller";
inline constexpr const char* kBlock = "org.freedesktop.UDisks2.Block";
inline constexpr const char* kPartition = "org.freedesktop.UDisks2.Partition";
inline constexpr const char* kPartitionTable = "org.freedesktop.UDisks2.PartitionTable";
inline constexpr const char* kFilesystem = "org.freedesktop.UDisks2.Filesystem";
inline constexpr const char* kLoop = "org.freedesktop.UDisks2.Loop";

std::optional<DiskDrive> parse_drive(const std::string& path, const Object& object);
std::optional<DiskBlock> parse_block(const std::string& path, const Object& object);
std::vector<DiskDrive> build_drives(const Objects& objects);
std::vector<DiskBlock> build_blocks(const Objects& objects);

// SMART from the drive's Ata / NVMe.Controller properties plus the reply of
// its SmartGetAttributes (Ata: a(ysqiiixia{sv}); NVMe: a{sv}; null = none).
DiskSmart parse_smart(const std::string& path, const Object& object, const dbus::Value* attributes);

// A NUL-terminated byte string ('ay') as text.
std::string bytestring(const dbus::Value& v);

void merge_managed_objects(Objects& objects, const dbus::Value& managed);
void merge_interfaces(Objects& objects, const std::string& path, const dbus::Value& ifaces);
// Removes the named interfaces (and the object when none are left).
void remove_interfaces(Objects& objects, const std::string& path, const std::vector<std::string>& ifaces);
// Applies a PropertiesChanged (changed a{sv} + invalidated names); true when anything changed.
bool apply_properties_changed(Objects& objects, const std::string& path, const std::string& iface,
                              const dbus::Value& changed);

}  // namespace brosys::udisks
