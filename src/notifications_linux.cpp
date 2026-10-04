#if !defined(_WIN32)

#include "notifications_internal.h"
#include "dbus_helper.h"
#include <iostream>

namespace brosys {

class LinuxNotificationBackend : public INotificationBackend {
public:
    LinuxNotificationBackend() = default;

    bool start(NotificationServer* server) override {
        server_ = server;
        bus_ = dbus::DBusConnection::open(dbus::BusType::Session);
        if (bus_) {
            bus_->request_name("org.freedesktop.Notifications");
        }
        return true;
    }

    void stop() override {
        if (bus_) {
            bus_->release_name("org.freedesktop.Notifications");
            bus_->close();
            bus_.reset();
        }
        server_ = nullptr;
    }

    void on_notification_posted(const NotificationItem& /*item*/) override {
        // Broadcast or update internal D-Bus clients if needed
    }

    void on_notification_closed(uint32_t id, CloseReason reason) override {
        if (bus_) {
            std::vector<dbus::DBusVariant> args;
            args.emplace_back(id);
            args.emplace_back(static_cast<uint32_t>(reason));
            bus_->emit_signal("/org/freedesktop/Notifications",
                              "org.freedesktop.Notifications",
                              "NotificationClosed",
                              args);
        }
    }

private:
    NotificationServer* server_ = nullptr;
    std::unique_ptr<dbus::DBusConnection> bus_;
};

std::unique_ptr<INotificationBackend> create_platform_notification_backend() {
    return std::make_unique<LinuxNotificationBackend>();
}

} // namespace brosys

#endif // !_WIN32
