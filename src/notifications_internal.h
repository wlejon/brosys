#pragma once

#include "brosys/notifications.h"
#include <memory>

namespace brosys {

class INotificationBackend {
public:
    virtual ~INotificationBackend() = default;
    virtual bool start(NotificationServer* server) = 0;
    virtual void stop() = 0;
    virtual void on_notification_posted(const NotificationItem& item) = 0;
    virtual void on_notification_closed(uint32_t id, CloseReason reason) = 0;
};

std::unique_ptr<INotificationBackend> create_platform_notification_backend();

} // namespace brosys
