// The Windows notification server.
//
// Other processes' notifications reach a Windows process only when it is the
// shell: balloons (Shell_NotifyIcon NIF_INFO) arrive at Shell_TrayWnd, which a
// shell-mode TrayHost owns; this server renders them when that host is its
// balloon_source, and reports interaction back to the icon (NIN_BALLOON*).
// Toasts (WinRT) go to Explorer's notification platform and cannot be
// intercepted without package identity, in either mode. Notifications the
// host post()s itself flow through the same queue in every mode.
//
// Threads: balloons arrive on the tray thread; one timer thread closes
// expired notifications; host calls run on the caller's thread. State is a
// mutex-guarded map; no lock of ours is held while calling the balloon hub.
#include "brosys/notifications.h"
#include "win/notifications_balloon.h"
#include "win/tray_host.h"
#include "win/util.h"

#include <shellapi.h>

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace brosys::win::notify {

namespace {

using Clock = std::chrono::steady_clock;
using tray::BalloonGone;
using tray::BalloonHub;
using tray::CallbackTarget;

struct Entry {
    Notification n;
    bool balloon = false;
    std::string item_id;    // balloon: the tray item
    CallbackTarget target;  // balloon: the icon as of the last show
};

class Server final : public NotificationServer, public tray::BalloonSink {
public:
    explicit Server(const NotificationServerConfig& config) : config_(config) {
        markup_ = std::find(config.capabilities.begin(), config.capabilities.end(), "body-markup") !=
                  config.capabilities.end();
    }

    ~Server() override {
        if (hub_) hub_->detach(this);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (timer_.joinable()) timer_.join();
    }

    // Returns false (with `error`) when the balloon source cannot be used.
    bool start(std::string* error) {
        TrayHost* source = config_.balloon_source;
        const std::string toasts =
            "toast (WinRT) notifications go to Explorer's notification platform and cannot be intercepted "
            "without package identity";
        if (!source) {
            caps_ = {false, "local only",
                     "no balloon_source: other processes' notifications reach a Windows process only when it is "
                     "the shell (Shell_NotifyIcon balloons at a shell-mode TrayHost); " + toasts};
        } else {
            auto* host = dynamic_cast<tray::WinTrayHost*>(source);
            hub_ = host ? host->balloon_hub() : nullptr;
            if (!hub_) {
                caps_ = {false, "local only",
                         "balloon_source does not host the tray (" + std::string(to_string(source->status().role)) +
                             ": " + source->status().detail + "); " + toasts};
            } else if (!hub_->attach(this)) {
                if (error) *error = "another NotificationServer already receives this tray host's balloons";
                hub_.reset();
                return false;
            } else {
                caps_ = {true, "Shell_NotifyIcon balloons",
                         "balloons of every process on the tray host's desktop arrive here; " + toasts};
            }
        }
        events_.push(NotificationServerStatus{caps_.receives_foreign, caps_.detail});
        timer_ = std::thread([this] { timer_loop(); });
        // Balloons shown before this server existed (held by the hub).
        if (hub_) hub_->deliver_held(this);
        return true;
    }

    // NotificationServer
    NotificationEventQueue& events() override { return events_; }

    NotificationServerCapabilities capabilities() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return caps_;
    }

    std::vector<Notification> active() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Notification> out;
        for (auto& [id, e] : entries_) out.push_back(e.n);
        return out;
    }

    std::vector<Notification> history() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return history_;
    }

    void clear_history() override {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.clear();
    }

    bool remove_from_history(uint32_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(history_.begin(), history_.end(), [id](const Notification& n) {
            return n.id == id;
        });
        if (it == history_.end()) return false;
        history_.erase(it);
        return true;
    }

    void set_do_not_disturb(bool enabled) override {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (dnd_ != enabled) {
                dnd_ = enabled;
                changed = true;
            }
        }
        if (changed) events_.push(DoNotDisturbChanged{enabled});
    }

    bool is_do_not_disturb() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return dnd_;
    }

    Result invoke_action(uint32_t id, const std::string& key, const std::string&) override {
        Entry e;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = entries_.find(id);
            if (it == entries_.end()) return Result::failure("no notification " + std::to_string(id));
            const auto& acts = it->second.n.actions;
            if (std::none_of(acts.begin(), acts.end(), [&](const NotificationAction& a) { return a.key == key; }))
                return Result::failure("notification " + std::to_string(id) + " has no action '" + key + "'");
            e = it->second;
        }
        if (e.balloon) {
            // The user clicked the balloon: the icon hears NIN_BALLOONUSERCLICK
            // and nothing else (the balloon is gone because of the click).
            if (auto t = hub_->target(e.item_id)) {
                AllowSetForegroundWindow(t->pid);
                tray::post_callback(*t, NIN_BALLOONUSERCLICK, 0, 0);
            }
            remove(id, CloseReason::Dismissed, false, std::nullopt);
            return Result::success();
        }
        if (!e.n.resident) remove(id, CloseReason::Dismissed, false, std::nullopt);
        return Result::success();
    }

    Result close(uint32_t id, CloseReason reason) override {
        if (!remove(id, reason, true, std::nullopt)) return Result::failure("no notification " + std::to_string(id));
        return Result::success();
    }

    uint32_t post(const Notification& notification) override {
        Notification n = notification;
        n.sender = "local";
        n.sender_pid = GetCurrentProcessId();
        bool replaced = false;
        bool dnd = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dnd = dnd_;
            auto it = n.id ? entries_.find(n.id) : entries_.end();
            if (it != entries_.end() && !it->second.balloon) {
                replaced = true;
            } else {
                n.id = allocate_id();
            }
            n.expires_at = expiry(n);
            Entry& e = entries_[n.id];
            e = Entry{};
            e.n = n;

            auto h_it = std::find_if(history_.begin(), history_.end(), [&](const Notification& item) {
                return item.id == n.id;
            });
            if (h_it != history_.end()) *h_it = n;
            else history_.push_back(n);
        }
        cv_.notify_all();
        events_.push(NotificationPosted{n, replaced, dnd});
        return n.id;
    }

    // BalloonSink (tray thread, hub mutex held: never call the hub from here).
    void balloon_shown(const tray::BalloonData& b) override {
        Notification n = balloon_notification(b);
        bool replaced = false;
        bool dnd = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dnd = dnd_;
            auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& kv) {
                return kv.second.balloon && kv.second.item_id == b.item_id;
            });
            if (it != entries_.end()) {
                n.id = it->first;
                replaced = true;
            } else {
                n.id = allocate_id();
            }
            n.expires_at = expiry(n);
            Entry& e = entries_[n.id];
            e.n = n;
            e.balloon = true;
            e.item_id = b.item_id;
            e.target = b.target;

            auto h_it = std::find_if(history_.begin(), history_.end(), [&](const Notification& item) {
                return item.id == n.id;
            });
            if (h_it != history_.end()) *h_it = n;
            else history_.push_back(n);
        }
        cv_.notify_all();
        events_.push(NotificationPosted{n, replaced, dnd});
        tray::post_callback(b.target, NIN_BALLOONSHOW, 0, 0);
    }

    void balloon_gone(const std::string& item_id, BalloonGone why) override {
        std::optional<Entry> e;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->second.balloon && it->second.item_id == item_id) {
                    e = std::move(it->second);
                    entries_.erase(it);
                    break;
                }
            }
        }
        if (!e) return;
        events_.push(NotificationClosed{e->n.id, CloseReason::Closed});
        // Documented: NIN_BALLOONHIDE when the balloon disappears, e.g. because
        // its icon was deleted. A dead owner hears nothing.
        if (why != BalloonGone::OwnerDied) tray::post_callback(e->target, NIN_BALLOONHIDE, 0, 0);
    }

    void source_gone() override {
        std::vector<uint32_t> closed;
        NotificationServerCapabilities caps;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (it->second.balloon) {
                    closed.push_back(it->first);
                    it = entries_.erase(it);
                } else {
                    ++it;
                }
            }
            caps_ = {false, "local only", "the balloon_source tray host was destroyed"};
            caps = caps_;
        }
        for (uint32_t id : closed) events_.push(NotificationClosed{id, CloseReason::Closed});
        events_.push(NotificationServerStatus{false, caps.detail});
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

    Notification balloon_notification(const tray::BalloonData& b) const {
        return make_balloon_notification(b, markup_);
    }

    // Removes `id` and reports it; `tell_icon`: a balloon's icon hears the
    // matching NIN_BALLOON* message. `due`: only if it expires by then.
    bool remove(uint32_t id, CloseReason reason, bool tell_icon, std::optional<Clock::time_point> due) {
        Entry e;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = entries_.find(id);
            if (it == entries_.end()) return false;
            if (due && (!it->second.n.expires_at || *it->second.n.expires_at > *due)) return false;
            e = std::move(it->second);
            entries_.erase(it);
        }
        events_.push(NotificationClosed{id, reason});
        if (e.balloon && tell_icon) {
            if (auto t = hub_->target(e.item_id)) tray::post_callback(*t, balloon_close_message(reason), 0, 0);
        }
        return true;
    }

    void timer_loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            std::optional<Clock::time_point> next;
            for (auto& [id, e] : entries_)
                if (e.n.expires_at && (!next || *e.n.expires_at < *next)) next = e.n.expires_at;
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
            for (auto& [id, e] : entries_)
                if (e.n.expires_at && *e.n.expires_at <= now) due.push_back(id);
            lock.unlock();
            for (uint32_t id : due) remove(id, CloseReason::Expired, true, now);
            lock.lock();
        }
    }

    NotificationServerConfig config_;
    bool markup_ = false;
    NotificationEventQueue events_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<uint32_t, Entry> entries_;
    std::vector<Notification> history_;
    bool dnd_ = false;
    uint32_t next_id_ = 1;
    bool stop_ = false;
    NotificationServerCapabilities caps_;
    std::shared_ptr<BalloonHub> hub_;
    std::thread timer_;
};

}  // namespace

}  // namespace brosys::win::notify

namespace brosys {

std::unique_ptr<NotificationServer> NotificationServer::create(const NotificationServerConfig& config,
                                                               std::string* error) {
    auto server = std::make_unique<win::notify::Server>(config);
    if (!server->start(error)) return nullptr;
    return server;
}

}  // namespace brosys
