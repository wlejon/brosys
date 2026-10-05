// The Linux TrayHost: a StatusNotifierHost that is also the
// StatusNotifierWatcher when it can own the name (role Watcher), or a
// registered host of another process's watcher (role WatcherClient).
//
// Threads: one sd-bus connection with its own thread. All tracking (watcher
// registry, item properties, dbusmenu layouts) happens there; item fetches
// are asynchronous so a slow item never stalls the others. The host's
// interaction calls are blocking D-Bus calls marshalled onto that thread.
// The item table is mutated only on the bus thread, under mu_, so the host
// can read snapshots (items(), menu(), status()) from its own thread.
#pragma once

#include "brosys/tray.h"
#include "linux/dbus/connection.h"
#include "linux/tray/sni_parse.h"
#include "linux/tray/watcher.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace brosys::tray {

inline constexpr const char* kItemIface = "org.kde.StatusNotifierItem";
inline constexpr const char* kMenuIface = "com.canonical.dbusmenu";

class LinuxTrayHost final : public TrayHost {
public:
    explicit LinuxTrayHost(const TrayConfig& cfg) : cfg_(cfg) {}
    ~LinuxTrayHost() override;
    bool start(std::string* error);

    TrayEventQueue& events() override { return events_; }
    TrayHostStatus status() const override;
    std::vector<TrayItem> items() const override;

    Result activate(const std::string& item_id, int32_t x, int32_t y) override;
    Result secondary_activate(const std::string& item_id, int32_t x, int32_t y) override;
    Result context_menu(const std::string& item_id, int32_t x, int32_t y) override;
    Result scroll(const std::string& item_id, int32_t delta, ScrollOrientation orientation) override;

    std::optional<MenuItem> menu(const std::string& item_id) const override;
    Result menu_about_to_show(const std::string& item_id, int32_t menu_item_id) override;
    Result menu_event(const std::string& item_id, int32_t menu_item_id, MenuEventType type) override;
    Result set_item_rect(const std::string& item_id, const Rect32& rect) override;

private:
    struct ItemState {
        uint64_t epoch = 0;      // identifies this registration in async replies
        std::string service;     // as registered (unique or well-known name)
        std::string path;
        std::string owner;       // unique name (signal matches)
        uint32_t pid = 0;
        ItemSnapshot snap;
        bool announced = false;  // TrayItemAdded was pushed
        bool fetching = false, refetch = false;
        int retries = 0;
        uint64_t item_match = 0;
        // dbusmenu
        std::string menu_path;
        uint64_t menu_match = 0;
        bool menu_fetching = false, menu_refetch = false;
        std::optional<MenuItem> menu;
    };

    // ---- roles (host.cpp, bus thread)
    bool claim_host_name(std::string* error);
    bool become_watcher(std::string* error);
    void enter_client(const std::string& owner);
    void watcher_owner_changed(const std::string& new_owner);
    void on_watcher_signal(const dbus::Message& m);
    void set_status(TrayRole role, std::string detail);

    // ---- items and menus (items.cpp, bus thread)
    void item_appeared(const std::string& id);
    void item_vanished(const std::string& id);
    void clear_items();
    ItemState* find_item(const std::string& id, uint64_t epoch);
    void fetch_item(const std::string& id);
    void apply_item(const std::string& id, uint64_t epoch, bool ok, const std::map<std::string, dbus::Value>& props);
    void set_menu_path(const std::string& id, const std::string& path);
    void fetch_menu(const std::string& id);
    void store_menu(const std::string& id, uint64_t epoch, const std::string& menu_path, const dbus::Reply& r);

    // ---- host-thread helpers (items.cpp)
    struct Target {
        std::string service, path, menu_path;
        uint64_t epoch = 0;
    };
    bool locate(const std::string& id, Target* t, std::string* why) const;
    Result call_item(const std::string& id, const char* method, const dbus::Args& args);

    TrayConfig cfg_;
    TrayEventQueue events_;

    mutable std::mutex mu_;
    TrayHostStatus status_;                   // guarded by mu_
    std::map<std::string, ItemState> items_;  // written on the bus thread under mu_

    // Bus thread.
    std::string host_name_;
    std::string watcher_owner_;  // unique name of the watcher we follow ("" none; ours in Watcher role)
    uint64_t next_epoch_ = 1;
    std::atomic<bool> stopping_{false};

    std::unique_ptr<Watcher> watcher_;
    std::unique_ptr<dbus::Connection> conn_;  // last: destroyed (thread joined) first
};

}  // namespace brosys::tray
