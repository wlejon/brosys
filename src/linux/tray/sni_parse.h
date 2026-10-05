// StatusNotifierItem properties and com.canonical.dbusmenu layouts -> the
// public value types. Pure functions (no bus), shared by the host and tests.
#pragma once

#include "brosys/tray.h"
#include "linux/dbus/value.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace brosys::tray {

// a(iiay): ARGB32 in network byte order -> straight RGBA. Malformed
// entries (non-positive size, too little data) are skipped.
std::vector<Image> pixmaps_from(const dbus::Value& v);

// ToolTip (sa(iiay)ss).
TrayToolTip tooltip_from(const dbus::Value& v, const std::string& theme_path);

struct ItemSnapshot {
    TrayItem item;          // id and pid are left for the caller
    std::string menu_path;  // "" when the item has no dbusmenu
};

// From an org.kde.StatusNotifierItem GetAll map (values unwrapped).
ItemSnapshot item_from_properties(const std::map<std::string, dbus::Value>& props);

// tray_change bits between two snapshots of one item.
uint32_t item_changes(const ItemSnapshot& before, const ItemSnapshot& after);

// One dbusmenu layout node (ia{sv}av), recursively; nullopt when malformed.
std::optional<MenuItem> menu_from_layout(const dbus::Value& node);

TrayItemStatus status_from(const std::string& s);
TrayCategory category_from(const std::string& s);

}  // namespace brosys::tray
