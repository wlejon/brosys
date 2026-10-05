// Linux PowerService: UPower (devices, on-battery, lid) and logind (CanX,
// actions, inhibitors, PrepareForSleep / PrepareForShutdown), both on one
// system-bus connection. Every piece of service state lives on that
// connection's thread; the host reads mutex-guarded snapshots and drains
// the event queue.
#include "brosys/power.h"
#include "linux/dbus/connection.h"
#include "linux/power/upower_model.h"

#include <mutex>
#include <unistd.h>

namespace brosys {

namespace {

using dbus::Value;
using upower::Props;

constexpr const char* kPropsIface = "org.freedesktop.DBus.Properties";
constexpr int kActionTimeoutMs = 120000;  // interactive polkit prompts take a while

class LogindInhibitor final : public Inhibitor {
public:
    explicit LogindInhibitor(dbus::UnixFd fd) : fd_(std::move(fd)) {}
    // Dropping the last reference to the fd closes it, which ends the inhibition.

private:
    dbus::UnixFd fd_;
};

class LinuxPowerService final : public PowerService {
public:
    ~LinuxPowerService() override { conn_.reset(); }  // joins the bus thread before members go

    bool start(const PowerConfig& config, std::string* error) {
        conn_ = dbus::Connection::open(dbus::BusKind::System, config.system_bus_address, "brosys-power", error);
        if (!conn_) return false;
        bool up = conn_->run_sync([this] {
            install_watches();
            reload_upower();
            refresh_capabilities();
            publish(true);
            return upower_up_ || logind_up_;
        });
        if (!up) {
            // As NetworkService without NetworkManager: no backend, no service.
            if (error)
                *error = "neither UPower (org.freedesktop.UPower) nor logind (org.freedesktop.login1) is running";
            return false;
        }
        // The system bus itself restarting: while it is down every call
        // fails, so a resync reports everything unknown; once the connection
        // is back the same resync reloads both daemons.
        conn_->set_disconnect_handler([this] { resync(); });
        conn_->set_reconnect_handler([this] { resync(); });
        return true;
    }

    PowerEventQueue& events() override { return queue_; }

    PowerState state() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return state_;
    }

    PowerCapabilities capabilities() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return caps_;
    }

    Result request(PowerAction action) override {
        dbus::Reply r;
        if (action == PowerAction::Lock) {
            std::string session = conn_->run_sync([this] { return session_path_; });
            if (session.empty()) return Result::failure("no login session to lock");
            r = conn_->call(upower::kLogindService, session, upower::kLogindSession, "Lock", {}, kActionTimeoutMs);
        } else {
            const char* method = nullptr;
            switch (action) {
                case PowerAction::Suspend: method = "Suspend"; break;
                case PowerAction::Hibernate: method = "Hibernate"; break;
                case PowerAction::HybridSleep: method = "HybridSleep"; break;
                case PowerAction::Reboot: method = "Reboot"; break;
                case PowerAction::PowerOff: method = "PowerOff"; break;
                case PowerAction::Lock: break;
            }
            if (!method) return Result::failure("unknown power action");
            r = conn_->call(upower::kLogindService, upower::kLogindPath, upower::kLogindManager, method,
                            {Value::boolean(true)}, kActionTimeoutMs);
        }
        return r.ok ? Result::success() : Result::failure(r.error());
    }

    std::unique_ptr<Inhibitor> inhibit(const InhibitRequest& req, std::string* error) override {
        std::string what = upower::inhibit_what(req.what);
        if (what.empty()) {
            if (error) *error = "nothing to inhibit (InhibitRequest::what has no known bit)";
            return nullptr;
        }
        dbus::Reply r = conn_->call(upower::kLogindService, upower::kLogindPath, upower::kLogindManager, "Inhibit",
                                    {Value::str(what), Value::str(req.who), Value::str(req.why),
                                     Value::str(req.mode == InhibitMode::Delay ? "delay" : "block")});
        if (!r.ok) {
            if (error) *error = r.error();
            return nullptr;
        }
        if (!r.first() || r.first()->as_fd() < 0) {
            if (error) *error = "logind Inhibit returned no file descriptor";
            return nullptr;
        }
        auto* fd = std::get_if<dbus::UnixFd>(&r.values[0].data);
        if (!fd) {
            if (error) *error = "logind Inhibit returned no file descriptor";
            return nullptr;
        }
        return std::make_unique<LogindInhibitor>(std::move(*fd));
    }

private:
    // ------------------------------------------------------------ bus thread

    void install_watches() {
        conn_->add_match(
            "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
            "path_namespace='/org/freedesktop/UPower'",
            [this](const dbus::Message& m) { on_properties_changed(m); });
        conn_->add_match("type='signal',interface='org.freedesktop.UPower',path='/org/freedesktop/UPower'",
                         [this](const dbus::Message& m) {
                             if (m.args.empty()) return;
                             std::string path = m.args[0].as_string();
                             if (m.member == "DeviceAdded") {
                                 fetch_device(path);
                             } else if (m.member == "DeviceRemoved") {
                                 devices_.erase(path);
                                 schedule_publish();
                             }
                         });
        conn_->add_match(
            "type='signal',interface='org.freedesktop.login1.Manager',path='/org/freedesktop/login1'",
            [this](const dbus::Message& m) {
                if (m.args.empty()) return;
                bool starting = m.args[0].as_bool();
                if (m.member == "PrepareForSleep") {
                    publish();  // anything pending goes out first
                    queue_.push(SleepPrepare{starting});
                } else if (m.member == "PrepareForShutdown") {
                    publish();
                    queue_.push(ShutdownPrepare{starting});
                }
            });
        conn_->watch_name_owner(upower::kService, [this](const std::string&, const std::string&, const std::string& now) {
            if (now.empty()) {
                upower_up_ = false;
                manager_props_.clear();
                devices_.clear();
            } else {
                reload_upower();
            }
            refresh_capabilities();
            publish();
        });
        conn_->watch_name_owner(upower::kLogindService,
                                [this](const std::string&, const std::string&, const std::string&) {
                                    refresh_capabilities();
                                    publish();
                                });
    }

    void resync() {
        reload_upower();
        refresh_capabilities();
        publish();
    }

    void reload_upower() {
        devices_.clear();
        std::string err;
        manager_props_ = conn_->get_all_properties(upower::kService, upower::kPath, upower::kIface, &err);
        upower_up_ = err.empty();
        if (!upower_up_) return;
        dbus::Reply r = conn_->call(upower::kService, upower::kPath, upower::kIface, "EnumerateDevices");
        if (!r.ok || !r.first()) return;
        for (auto& path : r.first()->as_strings()) {
            std::string derr;
            auto props = conn_->get_all_properties(upower::kService, path, upower::kDeviceIface, &derr);
            if (derr.empty()) devices_[path] = std::move(props);
        }
    }

    void fetch_device(const std::string& path) {
        conn_->get_all_properties_async(upower::kService, path, upower::kDeviceIface,
                                        [this, path](bool ok, std::map<std::string, Value> props, std::string) {
                                            if (!ok) return;
                                            devices_[path] = std::move(props);
                                            schedule_publish();
                                        });
    }

    void on_properties_changed(const dbus::Message& m) {
        if (m.args.size() < 2) return;
        std::string iface = m.args[0].as_string();
        bool invalidated = m.args.size() >= 3 && !m.args[2].items().empty();
        if (iface == upower::kIface && m.path == upower::kPath) {
            upower::merge_changed(manager_props_, m.args[1]);
            if (invalidated) {
                std::string err;
                auto all = conn_->get_all_properties(upower::kService, upower::kPath, upower::kIface, &err);
                if (err.empty()) manager_props_ = std::move(all);
            }
        } else if (iface == upower::kDeviceIface) {
            auto it = devices_.find(m.path);
            if (it == devices_.end()) return;  // the display device, or one we never enumerated
            upower::merge_changed(it->second, m.args[1]);
            if (invalidated) fetch_device(m.path);
        } else {
            return;
        }
        schedule_publish();
    }

    void refresh_capabilities() {
        PowerCapabilities c;
        auto can = [&](const char* method) {
            dbus::Reply r = conn_->call(upower::kLogindService, upower::kLogindPath, upower::kLogindManager, method);
            return r.ok && r.first() ? upower::availability_from_logind(r.first()->as_string()) : Availability::Unknown;
        };
        c.suspend = can("CanSuspend");
        c.hibernate = can("CanHibernate");
        c.hybrid_sleep = can("CanHybridSleep");
        c.reboot = can("CanReboot");
        c.power_off = can("CanPowerOff");
        logind_up_ = !conn_->get_name_owner(upower::kLogindService).empty();
        session_path_ = logind_up_ ? find_session() : std::string();
        c.lock = !logind_up_ ? Availability::Unknown : session_path_.empty() ? Availability::No : Availability::Yes;
        caps_pending_ = c;
    }

    // This process's session, else the user's display session ("" when neither exists).
    std::string find_session() {
        dbus::Reply r = conn_->call(upower::kLogindService, upower::kLogindPath, upower::kLogindManager,
                                    "GetSessionByPID", {Value::u32(static_cast<uint32_t>(getpid()))});
        if (r.ok && r.first() && !r.first()->as_string().empty()) return r.first()->as_string();
        r = conn_->call(upower::kLogindService, upower::kLogindPath, upower::kLogindManager, "GetUser",
                        {Value::u32(static_cast<uint32_t>(getuid()))});
        if (!r.ok || !r.first()) return {};
        dbus::Reply d = conn_->get_property(upower::kLogindService, r.first()->as_string(), upower::kLogindUser, "Display");
        if (!d.ok || !d.first()) return {};
        auto& fields = d.first()->items();  // (so): session id, object path
        if (fields.size() != 2) return {};
        std::string path = fields[1].as_string();
        return path == "/" ? std::string() : path;
    }

    PowerState build() const {
        PowerState s;
        if (upower_up_) {
            auto get = [&](const char* name) {
                auto it = manager_props_.find(name);
                return it != manager_props_.end() && it->second.as_bool();
            };
            s.source = get("OnBattery") ? PowerSource::Battery : PowerSource::AC;
            s.lid_present = get("LidIsPresent");
            s.lid_closed = s.lid_present && get("LidIsClosed");
        }
        for (auto& [path, props] : devices_)
            if (auto d = upower::device_from_props(path, props)) s.devices.push_back(std::move(*d));
        upower::aggregate(s);
        return s;
    }

    // Coalesces a burst of signals (one sd_bus_process round) into one snapshot.
    void schedule_publish() {
        if (publish_scheduled_) return;
        publish_scheduled_ = true;
        conn_->add_timer(std::chrono::milliseconds(0), [this] { publish(); });
    }

    void publish(bool initial = false) {
        publish_scheduled_ = false;
        PowerState s = build();
        bool push_state = false, push_caps = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (initial || !(s == state_)) {
                state_ = s;
                push_state = true;
            }
            if (initial || !(caps_pending_ == caps_)) {
                caps_ = caps_pending_;
                push_caps = true;
            }
        }
        if (push_state) queue_.push(PowerChanged{std::move(s)});
        if (push_caps) queue_.push(PowerCapabilitiesChanged{caps_pending_});
    }

    PowerEventQueue queue_;
    mutable std::mutex mu_;
    PowerState state_;       // published, guarded by mu_
    PowerCapabilities caps_;  // published, guarded by mu_

    // Bus-thread state.
    bool upower_up_ = false;
    bool logind_up_ = false;
    Props manager_props_;
    std::map<std::string, Props> devices_;  // every enumerated device (filtering happens in build())
    PowerCapabilities caps_pending_;
    std::string session_path_;
    bool publish_scheduled_ = false;

    std::unique_ptr<dbus::Connection> conn_;  // last: destroyed (thread joined) first
};

}  // namespace

std::unique_ptr<PowerService> PowerService::create(const PowerConfig& config, std::string* error) {
    auto s = std::make_unique<LinuxPowerService>();
    if (!s->start(config, error)) return nullptr;
    return s;
}

}  // namespace brosys
