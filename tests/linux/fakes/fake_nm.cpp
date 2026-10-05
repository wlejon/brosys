#include "linux/fakes/fake_nm.h"

using namespace brosys::dbus;

namespace bstest {

namespace {
constexpr const char* kNM = "org.freedesktop.NetworkManager";
constexpr const char* kDevice = "org.freedesktop.NetworkManager.Device";
constexpr const char* kWireless = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char* kProps = "org.freedesktop.DBus.Properties";
}  // namespace

FakeNM::FakeNM(const std::string& bus_address) {
    conn_ = Connection::open(BusKind::System, bus_address, "fake-nm", &error_);
    if (!conn_) return;
    auto om = std::make_shared<Interface>();
    om->name = "org.freedesktop.DBus.ObjectManager";
    om->methods.push_back({"GetManagedObjects", "", "a{oa{sa{sv}}}", {}, {"objects"}, [this](const MethodCall&) {
                               return MethodResult::ok({managed_objects()});
                           }});
    om->signals.push_back({"InterfacesAdded", "oa{sa{sv}}", {"path", "interfaces"}});
    om->signals.push_back({"InterfacesRemoved", "oas", {"path", "interfaces"}});
    ok_ = conn_->export_interface("/org/freedesktop", om, &error_);
    if (!ok_) return;

    auto settings = std::make_shared<Interface>();
    settings->name = "org.freedesktop.NetworkManager.Settings";
    settings->methods.push_back({"ListConnections", "", "ao", {}, {"connections"}, [this](const MethodCall&) {
                                     std::lock_guard<std::mutex> lock(mu_);
                                     std::vector<Value> paths;
                                     for (auto& [path, _] : connections_) {
                                         paths.push_back(Value::obj(path));
                                     }
                                     return MethodResult::ok({Value::array("o", paths)});
                                 }});
    ok_ = conn_->export_interface("/org/freedesktop/NetworkManager/Settings", settings, &error_);
}

FakeNM::~FakeNM() { conn_.reset(); }

bool FakeNM::own_name() {
    if (conn_->request_name("org.freedesktop.NetworkManager", 0, &error_) != NameRequest::PrimaryOwner) return false;
    // Survives the bus restarting (re-owns the name), as NetworkManager does.
    conn_->set_reconnect_handler([this] {
        std::string e;
        conn_->request_name("org.freedesktop.NetworkManager", 0, &e);
    });
    return true;
}

Value FakeNM::managed_objects() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<Value, Value>> objs;
    for (auto& [path, object] : objects_) {
        std::vector<std::pair<Value, Value>> ifaces;
        for (auto& [iface, props] : object) {
            std::vector<std::pair<std::string, Value>> p(props.begin(), props.end());
            ifaces.emplace_back(Value::str(iface), Value::vardict(p));
        }
        objs.emplace_back(Value::obj(path), Value::dict("s", "a{sv}", ifaces));
    }
    return Value::dict("o", "a{sa{sv}}", objs);
}

std::shared_ptr<Interface> FakeNM::make_interface(const std::string& path, const std::string& iface) {
    auto i = std::make_shared<Interface>();
    i->name = iface;
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [name, value] : objects_[path][iface]) {
        std::string n = name;
        i->properties.push_back({n, value.sig,
                                 [this, path, iface, n] {
                                     std::lock_guard<std::mutex> l(mu_);
                                     return objects_[path][iface][n];
                                 },
                                 nullptr, "true"});
    }
    if (iface == kWireless) {
        i->methods.push_back({"RequestScan", "a{sv}", "", {"options"}, {}, [this, path](const MethodCall&) {
                                  ScanMode mode;
                                  int delay;
                                  {
                                      std::lock_guard<std::mutex> l(mu_);
                                      ++scan_requests_;
                                      mode = scan_mode_;
                                      delay = scan_delay_ms_;
                                  }
                                  if (mode == ScanMode::Fail)
                                      return MethodResult::error("org.freedesktop.NetworkManager.Device.NotAllowed",
                                                                 "Scanning not allowed while unavailable");
                                  if (mode == ScanMode::Complete)
                                      conn_->add_timer(std::chrono::milliseconds(delay), [this, path] {
                                          std::function<void(const std::string&)> hook;
                                          int64_t now;
                                          {
                                              std::lock_guard<std::mutex> l(mu_);
                                              hook = scan_hook_;
                                              now = (clock_ms_ += 1000);
                                          }
                                          if (hook) hook(path);
                                          set(path, kWireless, {{"LastScan", Value::i64(now)}});
                                      });
                                  return MethodResult::ok();
                              }});
    } else if (iface == kNM) {
        i->methods.push_back({"AddAndActivateConnection", "a{sa{sv}}oo", "oo",
                              {"connection", "device", "specific_object"}, {"path", "active_connection"},
                              [this](const MethodCall& call) {
                                  std::lock_guard<std::mutex> l(mu_);
                                  if (add_and_activate_hook_) {
                                      return add_and_activate_hook_(call.args[0], call.args[1].as_string(),
                                                                    call.args[2].as_string());
                                  }
                                  add_and_activate_calls_.push_back(
                                      {call.args[0], call.args[1].as_string(), call.args[2].as_string()});
                                  std::string c_path =
                                      "/org/freedesktop/NetworkManager/Settings/gen_" + std::to_string(add_and_activate_calls_.size());
                                  std::string a_path =
                                      "/org/freedesktop/NetworkManager/ActiveConnection/gen_" + std::to_string(add_and_activate_calls_.size());
                                  return MethodResult::ok({Value::obj(c_path), Value::obj(a_path)});
                              }});
        i->methods.push_back({"ActivateConnection", "ooo", "o",
                              {"connection", "device", "specific_object"}, {"active_connection"},
                              [this](const MethodCall& call) {
                                  std::lock_guard<std::mutex> l(mu_);
                                  if (activate_hook_) {
                                      return activate_hook_(call.args[0].as_string(), call.args[1].as_string(),
                                                            call.args[2].as_string());
                                  }
                                  activate_calls_.push_back(
                                      {call.args[0].as_string(), call.args[1].as_string(), call.args[2].as_string()});
                                  std::string a_path =
                                      "/org/freedesktop/NetworkManager/ActiveConnection/gen_" + std::to_string(activate_calls_.size());
                                  return MethodResult::ok({Value::obj(a_path)});
                              }});
        i->methods.push_back({"DeactivateConnection", "o", "",
                              {"active_connection"}, {},
                              [this](const MethodCall& call) {
                                  std::lock_guard<std::mutex> l(mu_);
                                  if (deactivate_hook_) {
                                      return deactivate_hook_(call.args[0].as_string());
                                  }
                                  deactivate_calls_.push_back(call.args[0].as_string());
                                  return MethodResult::ok();
                              }});
    } else if (iface == kDevice) {
        i->methods.push_back({"Disconnect", "", "", {}, {},
                              [this, path](const MethodCall&) {
                                  std::lock_guard<std::mutex> l(mu_);
                                  if (device_disconnect_hook_) {
                                      return device_disconnect_hook_(path);
                                  }
                                  device_disconnect_calls_.push_back(path);
                                  return MethodResult::ok();
                              }});
    }
    return i;
}

void FakeNM::add_object(const std::string& path, const Object& object, bool quiet) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        objects_[path] = object;
    }
    std::vector<std::pair<Value, Value>> ifaces;
    for (auto& [iface, props] : object) {
        std::string err;
        conn_->export_interface(path, make_interface(path, iface), &err);
        std::vector<std::pair<std::string, Value>> p(props.begin(), props.end());
        ifaces.emplace_back(Value::str(iface), Value::vardict(p));
    }
    if (!quiet)
        conn_->emit_signal("/org/freedesktop", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                           {Value::obj(path), Value::dict("s", "a{sv}", ifaces)});
}

void FakeNM::remove_object(const std::string& path) {
    std::vector<std::string> ifaces;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [iface, props] : objects_[path]) ifaces.push_back(iface);
        objects_.erase(path);
    }
    conn_->unexport_path(path);
    conn_->emit_signal("/org/freedesktop", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                       {Value::obj(path), Value::strings(ifaces)});
}

void FakeNM::set(const std::string& path, const std::string& iface, const Props& changed) {
    std::vector<std::pair<std::string, Value>> p;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [name, value] : changed) {
            objects_[path][iface][name] = value;
            p.emplace_back(name, value);
        }
    }
    conn_->emit_signal(path, kProps, "PropertiesChanged",
                       {Value::str(iface), Value::vardict(p), Value::strings({})});
}

void FakeNM::invalidate(const std::string& path, const std::string& iface, const Props& changed) {
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [name, value] : changed) {
            objects_[path][iface][name] = value;
            names.push_back(name);
        }
    }
    conn_->emit_signal(path, kProps, "PropertiesChanged", {Value::str(iface), Value::vardict({}), Value::strings(names)});
}

void FakeNM::batch(const std::function<void()>& fn) { conn_->run_sync(fn); }

void FakeNM::set_scan_mode(ScanMode mode, int delay_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    scan_mode_ = mode;
    scan_delay_ms_ = delay_ms;
}

void FakeNM::set_scan_hook(std::function<void(const std::string&)> hook) {
    std::lock_guard<std::mutex> lock(mu_);
    scan_hook_ = std::move(hook);
}

int FakeNM::scan_requests() const {
    std::lock_guard<std::mutex> lock(mu_);
    return scan_requests_;
}

void FakeNM::add_connection(const std::string& path, const Value& settings) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        connections_[path] = settings;
    }
    auto iface = std::make_shared<Interface>();
    iface->name = "org.freedesktop.NetworkManager.Settings.Connection";
    iface->methods.push_back({"GetSettings", "", "a{sa{sv}}", {}, {"settings"}, [this, path](const MethodCall&) {
                                  std::lock_guard<std::mutex> l(mu_);
                                  auto it = connections_.find(path);
                                  if (it != connections_.end()) {
                                      return MethodResult::ok({it->second});
                                  }
                                  return MethodResult::error("org.freedesktop.NetworkManager.Settings.InvalidConnection",
                                                             "Unknown connection");
                              }});
    std::string err;
    conn_->export_interface(path, iface, &err);
}

void FakeNM::remove_connection(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        connections_.erase(path);
    }
    conn_->unexport_path(path);
}

std::vector<FakeNM::AddAndActivateCall> FakeNM::add_and_activate_calls() const {
    std::lock_guard<std::mutex> lock(mu_);
    return add_and_activate_calls_;
}

std::vector<FakeNM::ActivateCall> FakeNM::activate_calls() const {
    std::lock_guard<std::mutex> lock(mu_);
    return activate_calls_;
}

std::vector<std::string> FakeNM::deactivate_calls() const {
    std::lock_guard<std::mutex> lock(mu_);
    return deactivate_calls_;
}

std::vector<std::string> FakeNM::device_disconnect_calls() const {
    std::lock_guard<std::mutex> lock(mu_);
    return device_disconnect_calls_;
}

void FakeNM::set_add_and_activate_hook(
    std::function<brosys::dbus::MethodResult(const brosys::dbus::Value&, const std::string&, const std::string&)> hook) {
    std::lock_guard<std::mutex> lock(mu_);
    add_and_activate_hook_ = std::move(hook);
}

void FakeNM::set_activate_hook(
    std::function<brosys::dbus::MethodResult(const std::string&, const std::string&, const std::string&)> hook) {
    std::lock_guard<std::mutex> lock(mu_);
    activate_hook_ = std::move(hook);
}

void FakeNM::set_deactivate_hook(std::function<brosys::dbus::MethodResult(const std::string&)> hook) {
    std::lock_guard<std::mutex> lock(mu_);
    deactivate_hook_ = std::move(hook);
}

void FakeNM::set_device_disconnect_hook(std::function<brosys::dbus::MethodResult(const std::string&)> hook) {
    std::lock_guard<std::mutex> lock(mu_);
    device_disconnect_hook_ = std::move(hook);
}

}  // namespace bstest
