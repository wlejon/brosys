#include "brosys/notifications.h"
#include "notifications_internal.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace brosys {

struct NotificationServer::Impl {
    std::unique_ptr<INotificationBackend> backend;
    std::atomic<bool> running{false};
    std::atomic<uint32_t> next_id{1};

    std::unordered_map<uint32_t, NotificationItem> active_notifications;
    mutable std::mutex mutex;

    NotificationReceivedCallback received_cb;
    ActionInvokedCallback action_cb;
    NotificationClosedCallback closed_cb;

    std::thread timer_thread;
    std::atomic<bool> timer_running{false};

    Impl() : backend(create_platform_notification_backend()) {}

    ~Impl() {
        stop();
    }

    void stop() {
        if (timer_running.exchange(false)) {
            if (timer_thread.joinable()) {
                timer_thread.join();
            }
        }
        if (backend && running.load()) {
            backend->stop();
        }
        running.store(false);
    }
};

NotificationServer::NotificationServer() : impl_(std::make_unique<Impl>()) {}
NotificationServer::~NotificationServer() = default;

NotificationServer::NotificationServer(NotificationServer&&) noexcept = default;
NotificationServer& NotificationServer::operator=(NotificationServer&&) noexcept = default;

bool NotificationServer::start() {
    if (impl_->running.exchange(true)) {
        return true;
    }

    if (impl_->backend) {
        impl_->backend->start(this);
    }

    // Start expiration timer worker
    impl_->timer_running.store(true);
    impl_->timer_thread = std::thread([this]() {
        while (impl_->timer_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (!impl_->timer_running.load()) break;

            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            std::vector<uint32_t> expired;
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                for (const auto& [id, item] : impl_->active_notifications) {
                    if (item.timeout_ms > 0) {
                        if (now >= static_cast<int64_t>(item.timestamp_ms + item.timeout_ms)) {
                            expired.push_back(id);
                        }
                    }
                }
            }

            for (uint32_t id : expired) {
                close_notification(id, CloseReason::Expired);
            }
        }
    });

    return true;
}

void NotificationServer::stop() {
    impl_->stop();
}

bool NotificationServer::is_running() const {
    return impl_->running.load();
}

uint32_t NotificationServer::post_notification(const NotificationItem& item) {
    NotificationItem copy = item;
    if (copy.id == 0) {
        copy.id = impl_->next_id.fetch_add(1);
    }
    if (copy.timestamp_ms == 0) {
        copy.timestamp_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    NotificationReceivedCallback cb;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->active_notifications[copy.id] = copy;
        cb = impl_->received_cb;
    }

    if (impl_->backend && impl_->running.load()) {
        impl_->backend->on_notification_posted(copy);
    }

    if (cb) {
        cb(copy);
    }

    return copy.id;
}

bool NotificationServer::close_notification(uint32_t id, CloseReason reason) {
    NotificationClosedCallback cb;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->active_notifications.find(id);
        if (it != impl_->active_notifications.end()) {
            impl_->active_notifications.erase(it);
            found = true;
            cb = impl_->closed_cb;
        }
    }

    if (found) {
        if (impl_->backend && impl_->running.load()) {
            impl_->backend->on_notification_closed(id, reason);
        }
        if (cb) {
            cb(id, reason);
        }
        return true;
    }
    return false;
}

bool NotificationServer::invoke_action(uint32_t id, const std::string& action_key) {
    ActionInvokedCallback cb;
    bool exists = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->active_notifications.find(id);
        if (it != impl_->active_notifications.end()) {
            exists = true;
            cb = impl_->action_cb;
        }
    }

    if (exists && cb) {
        cb(id, action_key);
        return true;
    }
    return exists;
}

std::vector<NotificationItem> NotificationServer::get_active_notifications() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<NotificationItem> result;
    result.reserve(impl_->active_notifications.size());
    for (const auto& [_, item] : impl_->active_notifications) {
        result.push_back(item);
    }
    return result;
}

std::optional<NotificationItem> NotificationServer::get_notification(uint32_t id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->active_notifications.find(id);
    if (it != impl_->active_notifications.end()) {
        return it->second;
    }
    return std::nullopt;
}

size_t NotificationServer::active_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->active_notifications.size();
}

void NotificationServer::clear_all() {
    std::vector<uint32_t> ids;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& [id, _] : impl_->active_notifications) {
            ids.push_back(id);
        }
    }
    for (uint32_t id : ids) {
        close_notification(id, CloseReason::DismissedByUser);
    }
}

std::vector<std::string> NotificationServer::get_capabilities() const {
    return {
        "action-icons",
        "actions",
        "body",
        "body-hyperlinks",
        "body-images",
        "body-markup",
        "icon-multi",
        "icon-static",
        "persistence"
    };
}

ServerInfo NotificationServer::get_server_info() const {
    return ServerInfo{
        .name = "brosys",
        .vendor = "Bro Project",
        .version = "0.1.0",
        .spec_version = "1.2"
    };
}

void NotificationServer::set_notification_received_callback(NotificationReceivedCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->received_cb = std::move(cb);
}

void NotificationServer::set_action_invoked_callback(ActionInvokedCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->action_cb = std::move(cb);
}

void NotificationServer::set_notification_closed_callback(NotificationClosedCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->closed_cb = std::move(cb);
}

} // namespace brosys
