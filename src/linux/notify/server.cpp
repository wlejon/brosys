// org.freedesktop.Notifications (Desktop Notifications Specification 1.2).
//
// One sd-bus connection with its own thread. Every state change happens on
// that thread (D-Bus handlers, expiry timers, and the host's commands, which
// are marshalled there with run_sync); the notification table is also read
// by the host (active(), capabilities()), so it sits behind a mutex. Facts
// go to the host as value snapshots through the event queue.
#include "brosys/notifications.h"
#include "linux/dbus/connection.h"
#include "linux/notify/hints.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <unistd.h>

namespace brosys {

namespace {

using dbus::MethodCall;
using dbus::MethodResult;
using dbus::Value;

constexpr const char* kBusName = "org.freedesktop.Notifications";
constexpr const char* kPath = "/org/freedesktop/Notifications";
constexpr const char* kIface = "org.freedesktop.Notifications";

class LinuxNotificationServer final : public NotificationServer {
public:
    explicit LinuxNotificationServer(const NotificationServerConfig& cfg) : cfg_(cfg) {}
    ~LinuxNotificationServer() override {
        stopping_ = true;
        if (conn_) conn_->shutdown();  // joins the bus thread before anything else goes away
    }

    bool start(std::string* error);

    NotificationEventQueue& events() override { return events_; }
    NotificationServerCapabilities capabilities() const override;
    std::vector<Notification> active() const override;
    Result invoke_action(uint32_t id, const std::string& action_key, const std::string& activation_token) override;
    Result close(uint32_t id, CloseReason reason) override;
    uint32_t post(const Notification& notification) override;

private:
    struct Entry {
        Notification n;
        uint64_t timer = 0;       // connection timer id (0: none)
        uint64_t generation = 0;  // which add() armed the timer (a replace re-arms)
    };

    std::shared_ptr<dbus::Interface> make_interface();
    MethodResult on_notify(const MethodCall& c);
    MethodResult on_close(const MethodCall& c);

    // Bus thread only.
    uint32_t add(Notification n, uint32_t replaces_id);
    bool remove(uint32_t id, CloseReason reason);
    void on_expired(uint32_t id, uint64_t generation);
    void set_owned(bool owned, std::string detail, bool force);
    uint32_t allocate_id();

    NotificationServerConfig cfg_;
    NotificationEventQueue events_;

    mutable std::mutex mu_;
    std::map<uint32_t, Entry> entries_;          // guarded by mu_
    bool owned_ = false;                         // guarded by mu_
    std::string owner_detail_;                   // guarded by mu_

    uint32_t next_id_ = 1;       // bus thread
    uint64_t next_generation_ = 1;  // bus thread
    std::atomic<bool> stopping_{false};

    std::unique_ptr<dbus::Connection> conn_;  // last: destroyed first
};

// ---------------------------------------------------------------- setup

std::shared_ptr<dbus::Interface> LinuxNotificationServer::make_interface() {
    auto i = std::make_shared<dbus::Interface>();
    i->name = kIface;
    i->methods.push_back({"Notify", "susssasa{sv}i", "u",
                          {"app_name", "replaces_id", "app_icon", "summary", "body", "actions", "hints", "expire_timeout"},
                          {"id"},
                          [this](const MethodCall& c) { return on_notify(c); }});
    i->methods.push_back({"CloseNotification", "u", "", {"id"}, {},
                          [this](const MethodCall& c) { return on_close(c); }});
    i->methods.push_back({"GetCapabilities", "", "as", {}, {"capabilities"}, [this](const MethodCall&) {
                              return MethodResult::ok({Value::strings(cfg_.capabilities)});
                          }});
    i->methods.push_back({"GetServerInformation", "", "ssss", {}, {"name", "vendor", "version", "spec_version"},
                          [this](const MethodCall&) {
                              return MethodResult::ok({Value::str(cfg_.name), Value::str(cfg_.vendor),
                                                       Value::str(cfg_.version), Value::str("1.2")});
                          }});
    i->signals.push_back({"NotificationClosed", "uu", {"id", "reason"}});
    i->signals.push_back({"ActionInvoked", "us", {"id", "action_key"}});
    i->signals.push_back({"ActivationToken", "us", {"id", "activation_token"}});
    return i;
}

bool LinuxNotificationServer::start(std::string* error) {
    conn_ = dbus::Connection::open(dbus::BusKind::Session, cfg_.session_bus_address, "brosys-notifications", error);
    if (!conn_) return false;
    if (!conn_->export_interface(kPath, make_interface(), error)) return false;
    conn_->set_disconnect_handler([this] {
        if (!stopping_) set_owned(false, "the session bus connection was lost", false);
    });
    conn_->set_name_handler([this](const std::string& name, bool acquired) {
        if (name != kBusName || stopping_) return;
        if (acquired) {
            set_owned(true, std::string("owns ") + kBusName, false);
        } else {
            std::string owner = conn_->get_name_owner(kBusName);
            set_owned(false, std::string("lost ") + kBusName + (owner.empty() ? "" : " to " + owner) +
                                 "; queued to get it back", false);
        }
    });
    // Request and report on the bus thread, so NameAcquired / NameLost are
    // only ever seen after the initial status.
    return conn_->run_sync([&]() -> bool {
        uint32_t flags = dbus::name_flags::AllowReplacement | dbus::name_flags::Queue;
        if (cfg_.replace_existing) flags |= dbus::name_flags::ReplaceExisting;
        std::string err;
        switch (conn_->request_name(kBusName, flags, &err)) {
            case dbus::NameRequest::PrimaryOwner:
            case dbus::NameRequest::AlreadyOwner:
                set_owned(true, std::string("owns ") + kBusName, true);
                return true;
            case dbus::NameRequest::InQueue:
            case dbus::NameRequest::Exists: {
                std::string owner = conn_->get_name_owner(kBusName);
                set_owned(false, std::string("queued for ") + kBusName + (owner.empty() ? "" : " behind " + owner) +
                                     (cfg_.replace_existing ? " (the owner does not allow replacement)" : ""),
                          true);
                return true;
            }
            case dbus::NameRequest::Error: break;
        }
        if (error) *error = std::string("RequestName ") + kBusName + ": " + err;
        return false;
    });
}

void LinuxNotificationServer::set_owned(bool owned, std::string detail, bool force) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!force && owned == owned_) return;
        owned_ = owned;
        owner_detail_ = detail;
    }
    events_.push(NotificationServerStatus{owned, std::move(detail)});
}

// ---------------------------------------------------------------- D-Bus methods

MethodResult LinuxNotificationServer::on_notify(const MethodCall& c) {
    Notification n;
    n.app_name = c.args[0].as_string();
    uint32_t replaces_id = static_cast<uint32_t>(c.args[1].as_uint());
    n.app_icon = c.args[2].as_string();
    n.summary = c.args[3].as_string();
    n.body = c.args[4].as_string();
    auto actions = c.args[5].as_strings();
    for (size_t i = 0; i + 1 < actions.size(); i += 2) n.actions.push_back({actions[i], actions[i + 1]});
    notify::apply_hints(c.args[6], n);
    n.expire_timeout_ms = static_cast<int32_t>(c.args[7].as_int(-1));
    n.sender = c.sender;
    n.sender_pid = c.sender_pid();
    return MethodResult::ok({Value::u32(add(std::move(n), replaces_id))});
}

MethodResult LinuxNotificationServer::on_close(const MethodCall& c) {
    uint32_t id = static_cast<uint32_t>(c.args[0].as_uint());
    if (remove(id, CloseReason::Closed)) return MethodResult::ok();
    // Spec: "If the notification no longer exists, an empty D-BUS Error message is sent back."
    return MethodResult::error(dbus::kErrorFailed, "");
}

// ---------------------------------------------------------------- table (bus thread)

uint32_t LinuxNotificationServer::allocate_id() {
    // Caller holds mu_.
    while (true) {
        uint32_t id = next_id_++;
        if (next_id_ == 0) next_id_ = 1;
        if (id != 0 && !entries_.count(id)) return id;
    }
}

uint32_t LinuxNotificationServer::add(Notification n, uint32_t replaces_id) {
    int32_t timeout = n.expire_timeout_ms;
    if (timeout < 0) timeout = n.urgency == Urgency::Critical ? 0 : std::max<int32_t>(cfg_.default_timeout_ms, 0);
    n.expires_at.reset();
    if (timeout > 0) n.expires_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);

    bool replaced = false;
    uint64_t old_timer = 0;
    uint64_t generation = next_generation_++;
    Notification snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = replaces_id ? entries_.find(replaces_id) : entries_.end();
        if (it != entries_.end()) {
            replaced = true;
            n.id = replaces_id;
            old_timer = it->second.timer;
        } else {
            n.id = allocate_id();
        }
        Entry& e = entries_[n.id];
        e.n = std::move(n);
        e.timer = 0;
        e.generation = generation;
        snapshot = e.n;
    }
    if (old_timer) conn_->cancel_timer(old_timer);
    if (timeout > 0) {
        uint32_t id = snapshot.id;
        uint64_t timer = conn_->add_timer(std::chrono::milliseconds(timeout),
                                          [this, id, generation] { on_expired(id, generation); });
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(id);
        if (it != entries_.end()) it->second.timer = timer;
    }
    uint32_t id = snapshot.id;
    events_.push(NotificationPosted{std::move(snapshot), replaced});
    return id;
}

void LinuxNotificationServer::on_expired(uint32_t id, uint64_t generation) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(id);
        if (it == entries_.end() || it->second.generation != generation) return;  // replaced or gone since
    }
    remove(id, CloseReason::Expired);
}

bool LinuxNotificationServer::remove(uint32_t id, CloseReason reason) {
    uint64_t timer = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(id);
        if (it == entries_.end()) return false;
        timer = it->second.timer;
        entries_.erase(it);
    }
    if (timer) conn_->cancel_timer(timer);
    conn_->emit_signal(kPath, kIface, "NotificationClosed", {Value::u32(id), Value::u32(static_cast<uint32_t>(reason))});
    events_.push(NotificationClosed{id, reason});
    return true;
}

// ---------------------------------------------------------------- host API

NotificationServerCapabilities LinuxNotificationServer::capabilities() const {
    NotificationServerCapabilities c;
    c.source = kBusName;
    std::lock_guard<std::mutex> lock(mu_);
    c.receives_foreign = owned_ && conn_ && conn_->connected();
    if (!c.receives_foreign) c.detail = owner_detail_;
    return c;
}

std::vector<Notification> LinuxNotificationServer::active() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Notification> out;
    out.reserve(entries_.size());
    for (auto& [id, e] : entries_) out.push_back(e.n);
    return out;
}

Result LinuxNotificationServer::invoke_action(uint32_t id, const std::string& action_key,
                                              const std::string& activation_token) {
    return conn_->run_sync([&]() -> Result {
        bool resident = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = entries_.find(id);
            if (it == entries_.end()) return Result::failure("no notification " + std::to_string(id));
            auto& acts = it->second.n.actions;
            bool known = std::any_of(acts.begin(), acts.end(), [&](const NotificationAction& a) { return a.key == action_key; });
            if (!known) return Result::failure("notification " + std::to_string(id) + " has no action '" + action_key + "'");
            resident = it->second.n.resident;
        }
        if (!activation_token.empty())
            conn_->emit_signal(kPath, kIface, "ActivationToken", {Value::u32(id), Value::str(activation_token)});
        if (!conn_->emit_signal(kPath, kIface, "ActionInvoked", {Value::u32(id), Value::str(action_key)}))
            return Result::failure("not connected to the session bus");
        if (!resident) remove(id, CloseReason::Dismissed);
        return Result::success();
    });
}

Result LinuxNotificationServer::close(uint32_t id, CloseReason reason) {
    return conn_->run_sync([&]() -> Result {
        if (remove(id, reason)) return Result::success();
        return Result::failure("no notification " + std::to_string(id));
    });
}

uint32_t LinuxNotificationServer::post(const Notification& notification) {
    Notification n = notification;
    n.sender = "local";
    n.sender_pid = static_cast<uint32_t>(getpid());
    return conn_->run_sync([&]() -> uint32_t {
        uint32_t replaces = n.id;
        return add(std::move(n), replaces);
    });
}

}  // namespace

std::unique_ptr<NotificationServer> NotificationServer::create(const NotificationServerConfig& config,
                                                               std::string* error) {
    auto s = std::make_unique<LinuxNotificationServer>(config);
    if (!s->start(error)) return nullptr;
    return s;
}

}  // namespace brosys
