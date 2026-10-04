#if defined(_WIN32)

#include "notifications_internal.h"
#include <windows.h>
#include <iostream>

namespace brosys {

class WindowsNotificationBackend : public INotificationBackend {
public:
    WindowsNotificationBackend() = default;

    bool start(NotificationServer* server) override {
        server_ = server;
        return true;
    }

    void stop() override {
        server_ = nullptr;
    }

    void on_notification_posted(const NotificationItem& /*item*/) override {
        // Dispatched to desktop or toast notification system
    }

    void on_notification_closed(uint32_t /*id*/, CloseReason /*reason*/) override {
        // Closed notification hook
    }

private:
    NotificationServer* server_ = nullptr;
};

std::unique_ptr<INotificationBackend> create_platform_notification_backend() {
    return std::make_unique<WindowsNotificationBackend>();
}

} // namespace brosys

#endif // _WIN32
