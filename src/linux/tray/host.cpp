// LinuxTrayHost: lifecycle, the Watcher / WatcherClient roles, and the
// host-thread API that does not touch items (see items.cpp for those).
#include "linux/tray/host.h"

#include <unistd.h>

namespace brosys {

std::unique_ptr<TrayHost> TrayHost::create(const TrayConfig& config, std::string* error) {
    auto h = std::make_unique<tray::LinuxTrayHost>(config);
    if (!h->start(error)) return nullptr;
    return h;
}

namespace tray {

namespace {
std::atomic<int> g_instances{0};  // several hosts in one process get distinct host names
}  // namespace

LinuxTrayHost::~LinuxTrayHost() {
    stopping_ = true;
    if (conn_) conn_->shutdown();  // joins the bus thread; the watcher and item table go after
}

bool LinuxTrayHost::start(std::string* error) {
    conn_ = dbus::Connection::open(dbus::BusKind::Session, cfg_.session_bus_address, "brosys-tray", error);
    if (!conn_) return false;
    conn_->set_disconnect_handler([this] { on_disconnected(); });
    conn_->set_reconnect_handler([this] { on_reconnected(); });
    return conn_->run_sync([&]() -> bool {
        if (!claim_host_name(error)) return false;
        if (!conn_->add_match(std::string("type='signal',path='") + kWatcherPath + "',interface='" + kWatcherIface + "'",
                              [this](const dbus::Message& m) { on_watcher_signal(m); }) ||
            !conn_->watch_name_owner(kWatcherName, [this](const std::string&, const std::string&,
                                                          const std::string& now) { watcher_owner_changed(now); })) {
            if (error) *error = "cannot subscribe to the StatusNotifierWatcher signals";
            return false;
        }
        return enter_role(error);
    });
}

// The bus daemon went away: every item and registration with it.
void LinuxTrayHost::on_disconnected() {
    if (stopping_) return;
    clear_items("the session bus connection was lost");
    watcher_owner_.clear();
    if (watcher_) watcher_->reset();
    set_status(TrayRole::None, "the session bus connection was lost; reconnecting");
}

// The bus daemon is back (a new one at the same address): our matches and
// the watcher object are installed again by the connection; names and the
// role are ours to take up again. Items re-register by themselves when the
// watcher name gets an owner.
void LinuxTrayHost::on_reconnected() {
    if (stopping_) return;
    std::string err;
    if (!claim_host_name(&err) || !enter_role(&err)) set_status(TrayRole::None, "reconnected, but " + err);
}

// Watcher (owning the name, or queued for it) or WatcherClient (bus thread).
bool LinuxTrayHost::enter_role(std::string* error) {
    if (cfg_.become_watcher) {
        std::string err;
        switch (conn_->request_name(kWatcherName, dbus::name_flags::Queue, &err)) {
            case dbus::NameRequest::PrimaryOwner:
            case dbus::NameRequest::AlreadyOwner:
                return become_watcher(error);
            case dbus::NameRequest::InQueue:
            case dbus::NameRequest::Exists: {
                std::string owner = conn_->get_name_owner(kWatcherName);
                if (owner.empty())
                    set_status(TrayRole::None, "queued for org.kde.StatusNotifierWatcher");
                else
                    enter_client(owner);
                return true;
            }
            case dbus::NameRequest::Error: break;
        }
        if (error) *error = std::string("RequestName ") + kWatcherName + ": " + err;
        return false;
    }
    std::string owner = conn_->get_name_owner(kWatcherName);
    if (owner.empty())
        set_status(TrayRole::None, "no StatusNotifierWatcher on the bus (become_watcher is off); waiting for one");
    else
        enter_client(owner);
    return true;
}

bool LinuxTrayHost::claim_host_name(std::string* error) {
    if (!host_name_.empty()) {  // reconnected: the name we had, if it is free
        std::string err;
        auto r = conn_->request_name(host_name_, 0, &err);
        if (r == dbus::NameRequest::PrimaryOwner || r == dbus::NameRequest::AlreadyOwner) return true;
    }
    for (int attempt = 0; attempt < 64; ++attempt) {
        int n = g_instances++;
        host_name_ = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
        if (n) host_name_ += "-" + std::to_string(n);
        std::string err;
        switch (conn_->request_name(host_name_, 0, &err)) {
            case dbus::NameRequest::PrimaryOwner:
            case dbus::NameRequest::AlreadyOwner: return true;
            case dbus::NameRequest::Exists:
            case dbus::NameRequest::InQueue: continue;
            case dbus::NameRequest::Error:
                if (error) *error = "RequestName " + host_name_ + ": " + err;
                return false;
        }
    }
    if (error) *error = "no free org.kde.StatusNotifierHost-<pid> name";
    return false;
}

void LinuxTrayHost::set_status(TrayRole role, std::string detail) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_.role = role;
        status_.detail = detail;
    }
    events_.push(TrayHostStatus{role, std::move(detail)});
}

bool LinuxTrayHost::become_watcher(std::string* error) {
    if (!watcher_) {
        Watcher::Callbacks cb;
        cb.item_registered = [this](const std::string& id, ItemFlavor flavor) { item_appeared(id, flavor); };
        cb.item_unregistered = [this](const std::string& id) { item_vanished(id, "it was unregistered before it answered"); };
        auto w = std::make_unique<Watcher>(conn_.get(), std::move(cb));
        std::string err;
        if (!w->start(&err)) {
            if (error) *error = "StatusNotifierWatcher: " + err;
            set_status(TrayRole::None, "owns org.kde.StatusNotifierWatcher but cannot serve it: " + err);
            return false;
        }
        watcher_ = std::move(w);
    }
    watcher_owner_ = conn_->unique_name();
    watcher_->add_host(host_name_);
    // The same registry under the freedesktop name, when nobody else serves it
    // (queued otherwise: it becomes ours if its owner leaves).
    std::string err, also;
    switch (conn_->request_name(kFdoWatcherName, dbus::name_flags::Queue, &err)) {
        case dbus::NameRequest::PrimaryOwner:
        case dbus::NameRequest::AlreadyOwner: also = std::string(" and ") + kFdoWatcherName; break;
        case dbus::NameRequest::InQueue:
        case dbus::NameRequest::Exists:
            also = std::string(" (queued for ") + kFdoWatcherName + ", owned by " +
                   conn_->get_name_owner(kFdoWatcherName) + ")";
            break;
        case dbus::NameRequest::Error: also = std::string(" (") + kFdoWatcherName + ": " + err + ")"; break;
    }
    set_status(TrayRole::Watcher, std::string("owns ") + kWatcherName + also + " (host " + host_name_ + ")");
    return true;
}

void LinuxTrayHost::enter_client(const std::string& owner) {
    watcher_owner_ = owner;
    set_status(TrayRole::WatcherClient, "registered as " + host_name_ + " with the StatusNotifierWatcher at " + owner);
    conn_->call_async(owner, kWatcherPath, kWatcherIface, "RegisterStatusNotifierHost", {dbus::Value::str(host_name_)},
                      [this, owner](dbus::Reply r) {
                          if (stopping_ || r.ok || watcher_owner_ != owner) return;
                          set_status(TrayRole::WatcherClient,
                                     "the StatusNotifierWatcher at " + owner + " refused the host: " + r.error());
                      });
    conn_->call_async(owner, kWatcherPath, "org.freedesktop.DBus.Properties", "Get",
                      {dbus::Value::str(kWatcherIface), dbus::Value::str("RegisteredStatusNotifierItems")},
                      [this, owner](dbus::Reply r) {
                          if (stopping_ || !r.ok || watcher_owner_ != owner || !r.first()) return;
                          for (auto& id : r.first()->as_strings()) item_appeared(id);
                      });
}

void LinuxTrayHost::watcher_owner_changed(const std::string& now) {
    if (stopping_ || now == watcher_owner_) return;
    // A different watcher knows nothing of the old one's items; they
    // re-register with the new watcher themselves.
    clear_items("the StatusNotifierWatcher it registered with went away");
    watcher_owner_.clear();
    if (now.empty()) {
        set_status(TrayRole::None, cfg_.become_watcher
                                       ? "the StatusNotifierWatcher went away; queued for the name"
                                       : "the StatusNotifierWatcher went away; waiting for a new one");
    } else if (now == conn_->unique_name()) {
        std::string err;
        become_watcher(&err);
    } else {
        enter_client(now);
    }
}

void LinuxTrayHost::on_watcher_signal(const dbus::Message& m) {
    if (stopping_ || watcher_owner_.empty() || m.sender != watcher_owner_) return;
    if (watcher_owner_ == conn_->unique_name()) return;  // our own watcher reports directly
    if (m.args.empty()) return;
    if (m.member == "StatusNotifierItemRegistered")
        item_appeared(m.args[0].as_string());
    else if (m.member == "StatusNotifierItemUnregistered")
        item_vanished(m.args[0].as_string());
}

TrayHostStatus LinuxTrayHost::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

Result LinuxTrayHost::set_item_rect(const std::string& item_id, const Rect32&) {
    // StatusNotifierItem has no geometry query; the coordinates travel with
    // each Activate / ContextMenu call instead.
    std::lock_guard<std::mutex> lock(mu_);
    if (!items_.count(item_id)) return Result::failure("no tray item " + item_id);
    return Result::success();
}

}  // namespace tray
}  // namespace brosys
