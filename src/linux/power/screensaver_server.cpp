// Linux ScreenSaver / Idle Inhibit Provider.
//
// Exports org.freedesktop.ScreenSaver on the session bus (at /org/freedesktop/ScreenSaver
// and /ScreenSaver) and maps client Inhibit / Throttle calls to logind's idle inhibitor
// on the system bus. When a client process exits without calling UnInhibit, its
// inhibition is cleaned up automatically via NameOwnerChanged.
#include "linux/power/screensaver_server.h"

namespace brosys {

namespace {

using dbus::MethodCall;
using dbus::MethodResult;
using dbus::Value;

constexpr const char* kBusName = "org.freedesktop.ScreenSaver";
constexpr const char* kPath = "/org/freedesktop/ScreenSaver";
constexpr const char* kAltPath = "/ScreenSaver";
constexpr const char* kIface = "org.freedesktop.ScreenSaver";

constexpr const char* kLogindService = "org.freedesktop.login1";
constexpr const char* kLogindPath = "/org/freedesktop/login1";
constexpr const char* kLogindManager = "org.freedesktop.login1.Manager";

}  // namespace

LinuxScreenSaverServer::LinuxScreenSaverServer(const ScreenSaverConfig& config)
    : config_(config) {}

LinuxScreenSaverServer::~LinuxScreenSaverServer() {
    if (session_conn_) session_conn_->shutdown();
    if (system_conn_) system_conn_->shutdown();
}

std::shared_ptr<dbus::Interface> LinuxScreenSaverServer::make_interface() {
    auto i = std::make_shared<dbus::Interface>();
    i->name = kIface;
    i->methods.push_back({"Inhibit", "ss", "u", {"application_name", "reason_for_inhibit"}, {"cookie"},
                          [this](const MethodCall& c) { return on_inhibit(c, false); }});
    i->methods.push_back({"UnInhibit", "u", "", {"cookie"}, {},
                          [this](const MethodCall& c) { return on_uninhibit(c); }});
    i->methods.push_back({"GetActive", "", "b", {}, {"active"},
                          [this](const MethodCall&) {
                              return MethodResult::ok({Value::boolean(is_active())});
                          }});
    i->methods.push_back({"Throttle", "ss", "u", {"application_name", "reason_for_inhibit"}, {"cookie"},
                          [this](const MethodCall& c) { return on_inhibit(c, true); }});
    i->methods.push_back({"UnThrottle", "u", "", {"cookie"}, {},
                          [this](const MethodCall& c) { return on_uninhibit(c); }});
    i->methods.push_back({"SimulateUserActivity", "", "", {}, {},
                          [](const MethodCall&) { return MethodResult::ok(); }});
    i->signals.push_back({"ActiveChanged", "b", {"active"}});
    return i;
}

bool LinuxScreenSaverServer::start(std::string* error) {
    session_conn_ = dbus::Connection::open(dbus::BusKind::Session, config_.session_bus_address,
                                           "brosys-screensaver-session", error);
    if (!session_conn_) return false;

    // Connect to system bus for logind mapping (failure is non-fatal: provider still works)
    std::string sys_err;
    system_conn_ = dbus::Connection::open(dbus::BusKind::System, config_.system_bus_address,
                                          "brosys-screensaver-system", &sys_err);

    auto iface1 = make_interface();
    auto iface2 = make_interface();
    if (!session_conn_->export_interface(kPath, iface1, error)) return false;
    session_conn_->export_interface(kAltPath, iface2, nullptr);

    uint32_t flags = dbus::name_flags::AllowReplacement | dbus::name_flags::Queue;
    if (config_.replace_existing) flags |= dbus::name_flags::ReplaceExisting;
    session_conn_->request_name(kBusName, flags, nullptr);

    session_conn_->set_reconnect_handler([this, flags] {
        session_conn_->request_name(kBusName, flags, nullptr);
    });

    return true;
}

uint32_t LinuxScreenSaverServer::allocate_cookie() {
    while (true) {
        uint32_t c = next_cookie_++;
        if (next_cookie_ == 0) next_cookie_ = 1;
        if (c != 0 && !entries_.count(c)) return c;
    }
}

MethodResult LinuxScreenSaverServer::on_inhibit(const MethodCall& c, bool is_throttle) {
    std::string app = c.args.size() > 0 ? c.args[0].as_string() : "";
    std::string reason = c.args.size() > 1 ? c.args[1].as_string() : "";

    InhibitEntry entry;
    entry.app_name = app;
    entry.reason = reason;
    entry.is_throttle = is_throttle;
    entry.sender = c.sender;

    // Map to logind idle inhibit if system bus is available
    if (system_conn_) {
        dbus::Reply r = system_conn_->call(kLogindService, kLogindPath, kLogindManager, "Inhibit",
                                           {Value::str("idle"), Value::str(app), Value::str(reason), Value::str("block")});
        if (r.ok && !r.values.empty()) {
            if (auto* fd = std::get_if<dbus::UnixFd>(&r.values[0].data)) {
                entry.logind_fd = std::move(*fd);
            }
        }
    }

    uint32_t cookie = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cookie = allocate_cookie();
        entry.cookie = cookie;
        entries_[cookie] = std::move(entry);
    }

    // Watch sender so client death automatically releases the inhibition
    if (!c.sender.empty()) {
        uint64_t w = session_conn_->watch_name_owner(
            c.sender, [this, cookie](const std::string&, const std::string&, const std::string& new_owner) {
                if (new_owner.empty()) release_cookie(cookie);
            });
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(cookie);
        if (it != entries_.end()) it->second.watch_id = w;
    }

    return MethodResult::ok({Value::u32(cookie)});
}

MethodResult LinuxScreenSaverServer::on_uninhibit(const MethodCall& c) {
    uint32_t cookie = c.args.size() > 0 ? static_cast<uint32_t>(c.args[0].as_uint()) : 0;
    release_cookie(cookie);
    return MethodResult::ok();
}

void LinuxScreenSaverServer::release_cookie(uint32_t cookie) {
    uint64_t watch_id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(cookie);
        if (it == entries_.end()) return;
        watch_id = it->second.watch_id;
        // Dropping entry closes logind_fd
        entries_.erase(it);
    }
    if (watch_id && session_conn_) {
        session_conn_->post([this, watch_id] {
            session_conn_->remove_match(watch_id);
        });
    }
}

uint32_t LinuxScreenSaverServer::active_inhibitions() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<uint32_t>(entries_.size());
}

bool LinuxScreenSaverServer::is_active() const {
    std::lock_guard<std::mutex> lock(mu_);
    return active_;
}

void LinuxScreenSaverServer::set_active(bool active) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (active_ == active) return;
        active_ = active;
    }
    if (session_conn_) {
        session_conn_->emit_signal(kPath, kIface, "ActiveChanged", {Value::boolean(active)});
        session_conn_->emit_signal(kAltPath, kIface, "ActiveChanged", {Value::boolean(active)});
    }
}

Result LinuxScreenSaverServer::simulate_user_activity() {
    return Result::success();
}

std::unique_ptr<ScreenSaverServer> ScreenSaverServer::create(const ScreenSaverConfig& config,
                                                             std::string* error) {
    auto s = std::make_unique<LinuxScreenSaverServer>(config);
    if (!s->start(error)) return nullptr;
    return s;
}

}  // namespace brosys
