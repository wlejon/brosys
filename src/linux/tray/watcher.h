// org.kde.StatusNotifierWatcher at /StatusNotifierWatcher.
//
// Lives on the tray host's connection and runs entirely on its bus thread.
// Items register as a bus name ("org.kde.StatusNotifierItem-1-1", path
// /StatusNotifierItem), an object path (resolved against the sender, as
// libappindicator does), or "name/path"; the registry key is
// "<service><path>". Items and hosts are dropped when their bus name loses
// its owner.
#pragma once

#include "linux/dbus/connection.h"

#include <functional>
#include <string>
#include <vector>

namespace brosys::tray {

inline constexpr const char* kWatcherName = "org.kde.StatusNotifierWatcher";
inline constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
inline constexpr const char* kWatcherIface = "org.kde.StatusNotifierWatcher";

class Watcher {
public:
    struct Callbacks {
        std::function<void(const std::string& id)> item_registered;
        std::function<void(const std::string& id)> item_unregistered;
    };

    // The connection's handlers capture this: destroy the Watcher only after
    // the connection (its thread is joined first).
    Watcher(dbus::Connection* conn, Callbacks callbacks);
    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    // Exports the object and starts tracking owners (bus thread).
    bool start(std::string* error);
    // An in-process host (registered without a D-Bus round trip).
    void add_host(const std::string& bus_name);
    const std::vector<std::string>& items() const { return items_; }

private:
    dbus::MethodResult register_item(const dbus::MethodCall& c);
    dbus::MethodResult register_host(const dbus::MethodCall& c);
    void owner_changed(const std::string& name, const std::string& new_owner);
    void remove_item_at(size_t index);

    dbus::Connection* conn_;
    Callbacks cb_;
    uint64_t owner_match_ = 0;
    std::vector<std::string> items_;          // ids in registration order
    std::vector<std::string> item_services_;  // parallel: the bus name each id depends on
    std::vector<std::string> hosts_;
};

// Splits a registry id "<service><path>" into its bus name and object path.
bool split_item_id(const std::string& id, std::string* service, std::string* path);

}  // namespace brosys::tray
