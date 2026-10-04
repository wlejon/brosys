#if !defined(_WIN32)

#include "tray_internal.h"
#include "dbus_helper.h"
#include <iostream>

namespace brosys {

class LinuxTrayBackend : public ITrayBackend {
public:
    LinuxTrayBackend() = default;

    bool start(TrayHost* host) override {
        host_ = host;
        bus_ = dbus::DBusConnection::open(dbus::BusType::Session);
        if (bus_) {
            bus_->request_name("org.kde.StatusNotifierWatcher");
        }
        return true;
    }

    void stop() override {
        if (bus_) {
            bus_->release_name("org.kde.StatusNotifierWatcher");
            bus_->close();
            bus_.reset();
        }
        host_ = nullptr;
    }

    void on_item_registered(const StatusNotifierItem& item) override {
        if (bus_) {
            std::vector<dbus::DBusVariant> args;
            args.emplace_back(item.id);
            bus_->emit_signal("/StatusNotifierWatcher",
                              "org.kde.StatusNotifierWatcher",
                              "StatusNotifierItemRegistered",
                              args);
        }
    }

    void on_item_unregistered(const std::string& id) override {
        if (bus_) {
            std::vector<dbus::DBusVariant> args;
            args.emplace_back(id);
            bus_->emit_signal("/StatusNotifierWatcher",
                              "org.kde.StatusNotifierWatcher",
                              "StatusNotifierItemUnregistered",
                              args);
        }
    }

    bool activate_item(const std::string& id, int x, int y) override {
        if (!bus_) return false;
        std::vector<dbus::DBusVariant> args;
        args.emplace_back(x);
        args.emplace_back(y);
        std::vector<dbus::DBusVariant> out;
        dbus::MethodCall call{
            id,
            "/StatusNotifierItem",
            "org.kde.StatusNotifierItem",
            "Activate"
        };
        return bus_->call_method(call, args, out);
    }

    bool secondary_activate_item(const std::string& id, int x, int y) override {
        if (!bus_) return false;
        std::vector<dbus::DBusVariant> args;
        args.emplace_back(x);
        args.emplace_back(y);
        std::vector<dbus::DBusVariant> out;
        dbus::MethodCall call{
            id,
            "/StatusNotifierItem",
            "org.kde.StatusNotifierItem",
            "SecondaryActivate"
        };
        return bus_->call_method(call, args, out);
    }

    bool context_menu(const std::string& id, int x, int y) override {
        if (!bus_) return false;
        std::vector<dbus::DBusVariant> args;
        args.emplace_back(x);
        args.emplace_back(y);
        std::vector<dbus::DBusVariant> out;
        dbus::MethodCall call{
            id,
            "/StatusNotifierItem",
            "org.kde.StatusNotifierItem",
            "ContextMenu"
        };
        return bus_->call_method(call, args, out);
    }

private:
    TrayHost* host_ = nullptr;
    std::unique_ptr<dbus::DBusConnection> bus_;
};

std::unique_ptr<ITrayBackend> create_platform_tray_backend() {
    return std::make_unique<LinuxTrayBackend>();
}

} // namespace brosys

#endif // !_WIN32
