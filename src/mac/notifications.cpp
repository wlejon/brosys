// The macOS notification server: local only.
//
// Other applications' notifications go to Notification Center (usernoted)
// through UNUserNotificationCenter; macOS has no notification-server role and
// no public API to receive, read or intercept them, so capabilities() says
// receives_foreign = false. What works is the host's own notifications
// (post()), which flow through the same queue, expiry and interaction model as
// on the other platforms.
//
// Deliberately not done: forwarding post() to Notification Center. The host
// renders its own notifications (that is what this server is for); a
// UNUserNotificationCenter client needs an app bundle and the user's
// permission, and would show every notification twice.
#include "brosys/notifications.h"

#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace brosys {

namespace {

using Clock = std::chrono::steady_clock;

class MacNotificationServer final : public NotificationServer {
public:
    explicit MacNotificationServer(const NotificationServerConfig& config) : config_(config) {
        caps_ = {false, "local only",
                 "macOS delivers other applications' notifications only to Notification Center (usernoted); there is "
                 "no notification-server role and no public API to receive them"};
        events_.push(NotificationServerStatus{false, caps_.detail});
        timer_ = std::thread([this] { timer_loop(); });
    }

    ~MacNotificationServer() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        timer_.join();
    }

    NotificationEventQueue& events() override { return events_; }
    NotificationServerCapabilities capabilities() const override { return caps_; }

    std::vector<Notification> active() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Notification> out;
        for (auto& [id, n] : entries_) out.push_back(n);
        return out;
    }

    Result invoke_action(uint32_t id, const std::string& key, const std::string&) override {
        bool resident = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = entries_.find(id);
            if (it == entries_.end()) return Result::failure("no notification " + std::to_string(id));
            const auto& acts = it->second.actions;
            if (std::none_of(acts.begin(), acts.end(), [&](const NotificationAction& a) { return a.key == key; }))
                return Result::failure("notification " + std::to_string(id) + " has no action '" + key + "'");
            resident = it->second.resident;
        }
        if (!resident) remove(id, CloseReason::Dismissed, std::nullopt);
        return Result::success();
    }

    Result close(uint32_t id, CloseReason reason) override {
        if (!remove(id, reason, std::nullopt)) return Result::failure("no notification " + std::to_string(id));
        return Result::success();
    }

    uint32_t post(const Notification& notification) override {
        Notification n = notification;
        n.sender = "local";
        n.sender_pid = static_cast<uint32_t>(getpid());
        bool replaced = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (n.id && entries_.count(n.id)) replaced = true;
            else n.id = allocate_id();
            n.expires_at = expiry(n);
            entries_[n.id] = n;
        }
        cv_.notify_all();
        events_.push(NotificationPosted{n, replaced});
        return n.id;
    }

private:
    uint32_t allocate_id() {
        for (;;) {
            uint32_t id = next_id_++;
            if (id != 0 && !entries_.count(id)) return id;
        }
    }

    std::optional<Clock::time_point> expiry(const Notification& n) const {
        int32_t ms = n.expire_timeout_ms;
        if (ms < 0) {
            if (n.urgency == Urgency::Critical) return std::nullopt;
            ms = config_.default_timeout_ms;
        }
        if (ms <= 0) return std::nullopt;
        return Clock::now() + std::chrono::milliseconds(ms);
    }

    // `due`: only if it expires by then.
    bool remove(uint32_t id, CloseReason reason, std::optional<Clock::time_point> due) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = entries_.find(id);
            if (it == entries_.end()) return false;
            if (due && (!it->second.expires_at || *it->second.expires_at > *due)) return false;
            entries_.erase(it);
        }
        events_.push(NotificationClosed{id, reason});
        return true;
    }

    void timer_loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            std::optional<Clock::time_point> next;
            for (auto& [id, n] : entries_)
                if (n.expires_at && (!next || *n.expires_at < *next)) next = n.expires_at;
            if (!next) {
                cv_.wait(lock);
                continue;
            }
            if (Clock::now() < *next) {
                cv_.wait_until(lock, *next);
                continue;
            }
            const auto now = Clock::now();
            std::vector<uint32_t> due;
            for (auto& [id, n] : entries_)
                if (n.expires_at && *n.expires_at <= now) due.push_back(id);
            lock.unlock();
            for (uint32_t id : due) remove(id, CloseReason::Expired, now);
            lock.lock();
        }
    }

    NotificationServerConfig config_;
    NotificationServerCapabilities caps_;
    NotificationEventQueue events_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<uint32_t, Notification> entries_;
    uint32_t next_id_ = 1;
    bool stop_ = false;
    std::thread timer_;
};

}  // namespace

std::unique_ptr<NotificationServer> NotificationServer::create(const NotificationServerConfig& config, std::string*) {
    return std::make_unique<MacNotificationServer>(config);
}

}  // namespace brosys
