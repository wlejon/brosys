#include "linux/tray/watcher.h"

#include <systemd/sd-bus.h>

#include <algorithm>

namespace brosys::tray {

using dbus::MethodCall;
using dbus::MethodResult;
using dbus::Value;

bool split_item_id(const std::string& id, std::string* service, std::string* path) {
    size_t slash = id.find('/');
    if (slash == 0 || slash == std::string::npos) return false;
    *service = id.substr(0, slash);
    *path = id.substr(slash);
    return true;
}

Watcher::Watcher(dbus::Connection* conn, Callbacks callbacks) : conn_(conn), cb_(std::move(callbacks)) {}

bool Watcher::start(std::string* error) {
    owner_match_ = conn_->watch_name_owner("", [this](const std::string& name, const std::string&,
                                                      const std::string& now) { owner_changed(name, now); });
    if (!owner_match_) {
        if (error) *error = "cannot watch NameOwnerChanged";
        return false;
    }
    for (ItemFlavor flavor : {ItemFlavor::Kde, ItemFlavor::Freedesktop}) {
        auto i = std::make_shared<dbus::Interface>();
        i->name = flavor == ItemFlavor::Kde ? kWatcherIface : kFdoWatcherIface;
        i->methods.push_back({"RegisterStatusNotifierItem", "s", "", {"service"}, {},
                              [this, flavor](const MethodCall& c) { return register_item(c, flavor); }});
        i->methods.push_back({"RegisterStatusNotifierHost", "s", "", {"service"}, {},
                              [this](const MethodCall& c) { return register_host(c); }});
        i->properties.push_back({"RegisteredStatusNotifierItems", "as", [this] { return Value::strings(items_); },
                                 nullptr, "true"});
        i->properties.push_back({"IsStatusNotifierHostRegistered", "b",
                                 [this] { return Value::boolean(!hosts_.empty()); }, nullptr, "true"});
        i->properties.push_back({"ProtocolVersion", "i", [] { return Value::i32(0); }, nullptr, "const"});
        i->signals.push_back({"StatusNotifierItemRegistered", "s", {"service"}});
        i->signals.push_back({"StatusNotifierItemUnregistered", "s", {"service"}});
        i->signals.push_back({"StatusNotifierHostRegistered", "", {}});
        i->signals.push_back({"StatusNotifierHostUnregistered", "", {}});
        if (!conn_->export_interface(kWatcherPath, i, error)) return false;
    }
    return true;
}

void Watcher::emit(const char* member, const dbus::Args& args) {
    conn_->emit_signal(kWatcherPath, kWatcherIface, member, args);
    conn_->emit_signal(kWatcherPath, kFdoWatcherIface, member, args);
}

void Watcher::emit_changed(const char* property) {
    conn_->emit_properties_changed(kWatcherPath, kWatcherIface, {property});
    conn_->emit_properties_changed(kWatcherPath, kFdoWatcherIface, {property});
}

void Watcher::reset() {
    items_.clear();
    item_services_.clear();
    hosts_.clear();
}

void Watcher::add_host(const std::string& bus_name) {
    if (std::find(hosts_.begin(), hosts_.end(), bus_name) != hosts_.end()) return;
    bool first = hosts_.empty();
    hosts_.push_back(bus_name);
    emit("StatusNotifierHostRegistered", {});
    if (first) emit_changed("IsStatusNotifierHostRegistered");
}

MethodResult Watcher::register_item(const MethodCall& c, ItemFlavor flavor) {
    const std::string arg = c.args[0].as_string();
    std::string service, path;
    if (!arg.empty() && arg[0] == '/') {
        service = c.sender;  // libappindicator & co. register their object path
        path = arg;
    } else if (arg.find('/') != std::string::npos) {
        split_item_id(arg, &service, &path);
    } else {
        service = arg;
        path = "/StatusNotifierItem";
    }
    if (service.empty() || !sd_bus_service_name_is_valid(service.c_str()))
        return MethodResult::error(dbus::kErrorInvalidArgs, "'" + arg + "' is not a bus name or object path");
    if (!sd_bus_object_path_is_valid(path.c_str()))
        return MethodResult::error(dbus::kErrorInvalidArgs, "'" + path + "' is not an object path");
    if (conn_->get_name_owner(service).empty())
        return MethodResult::error("org.freedesktop.DBus.Error.NameHasNoOwner", service + " has no owner");
    std::string id = service + path;
    if (std::find(items_.begin(), items_.end(), id) != items_.end()) return MethodResult::ok();
    items_.push_back(id);
    item_services_.push_back(service);
    emit("StatusNotifierItemRegistered", {Value::str(id)});
    emit_changed("RegisteredStatusNotifierItems");
    if (cb_.item_registered) cb_.item_registered(id, flavor);
    return MethodResult::ok();
}

MethodResult Watcher::register_host(const MethodCall& c) {
    std::string service = c.args[0].as_string();
    if (!service.empty() && service[0] == '/') service = c.sender;
    if (service.empty() || !sd_bus_service_name_is_valid(service.c_str()))
        return MethodResult::error(dbus::kErrorInvalidArgs, "'" + service + "' is not a bus name");
    if (conn_->get_name_owner(service).empty())
        return MethodResult::error("org.freedesktop.DBus.Error.NameHasNoOwner", service + " has no owner");
    add_host(service);
    return MethodResult::ok();
}

void Watcher::remove_item_at(size_t index) {
    std::string id = items_[index];
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
    item_services_.erase(item_services_.begin() + static_cast<std::ptrdiff_t>(index));
    emit("StatusNotifierItemUnregistered", {Value::str(id)});
    emit_changed("RegisteredStatusNotifierItems");
    if (cb_.item_unregistered) cb_.item_unregistered(id);
}

void Watcher::owner_changed(const std::string& name, const std::string& new_owner) {
    if (!new_owner.empty()) return;
    for (size_t i = items_.size(); i-- > 0;)
        if (item_services_[i] == name) remove_item_at(i);
    auto h = std::find(hosts_.begin(), hosts_.end(), name);
    if (h != hosts_.end()) {
        hosts_.erase(h);
        emit("StatusNotifierHostUnregistered", {});
        if (hosts_.empty()) emit_changed("IsStatusNotifierHostRegistered");
    }
}

}  // namespace brosys::tray
