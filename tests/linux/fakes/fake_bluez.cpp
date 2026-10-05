#include "linux/fakes/fake_bluez.h"

namespace bstest {

using namespace brosys::dbus;

FakeBlueZ::FakeBlueZ(const std::string& bus_address) {
    conn_ = Connection::open(BusKind::System, bus_address, "fake-bluez", &error_);
    if (!conn_) return;

    if (!conn_->export_interface("/", make_manager_iface(), &error_)) return;

    if (conn_->request_name("org.bluez", name_flags::AllowReplacement | name_flags::ReplaceExisting,
                            &error_) != NameRequest::PrimaryOwner)
        return;

    ok_ = true;
}

FakeBlueZ::~FakeBlueZ() {
    if (conn_) conn_->shutdown();
}

Value FakeBlueZ::managed_objects() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<Value, Value>> objs;
    for (auto& [path, ifaces] : objects_) {
        std::vector<std::pair<Value, Value>> iface_entries;
        for (auto& [iface, props] : ifaces) {
            std::vector<std::pair<std::string, Value>> p_list;
            for (auto& [k, v] : props) p_list.emplace_back(k, v);
            iface_entries.emplace_back(Value::str(iface), Value::vardict(p_list));
        }
        objs.emplace_back(Value::obj(path), Value::dict("s", "a{sv}", iface_entries));
    }
    return Value::dict("o", "a{sa{sv}}", objs);
}

std::shared_ptr<Interface> FakeBlueZ::make_manager_iface() {
    auto i = std::make_shared<Interface>();
    i->name = "org.freedesktop.DBus.ObjectManager";
    i->methods.push_back({"GetManagedObjects", "", "a{oa{sa{sv}}}", {}, {"objects"},
                          [this](const MethodCall&) {
                              return MethodResult::ok({managed_objects()});
                          }});
    i->signals.push_back({"InterfacesAdded", "oa{sa{sv}}", {"object_path", "interfaces_and_properties"}});
    i->signals.push_back({"InterfacesRemoved", "oas", {"object_path", "interfaces"}});
    return i;
}

std::shared_ptr<Interface> FakeBlueZ::make_adapter_iface(const std::string& path) {
    auto i = std::make_shared<Interface>();
    i->name = "org.bluez.Adapter1";
    i->methods.push_back({"StartDiscovery", "", "", {}, {}, [this, path](const MethodCall&) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            calls_.push_back("StartDiscovery");
            objects_[path]["org.bluez.Adapter1"]["Discovering"] = Value::boolean(true);
        }
        conn_->emit_properties_changed(path, "org.bluez.Adapter1", {"Discovering"});
        return MethodResult::ok();
    }});
    i->methods.push_back({"StopDiscovery", "", "", {}, {}, [this, path](const MethodCall&) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            calls_.push_back("StopDiscovery");
            objects_[path]["org.bluez.Adapter1"]["Discovering"] = Value::boolean(false);
        }
        conn_->emit_properties_changed(path, "org.bluez.Adapter1", {"Discovering"});
        return MethodResult::ok();
    }});
    i->methods.push_back({"RemoveDevice", "o", "", {"device"}, {}, [this, path](const MethodCall& c) {
        std::string dev_path = c.args[0].as_string();
        {
            std::lock_guard<std::mutex> lock(mu_);
            calls_.push_back("RemoveDevice(" + dev_path + ")");
        }
        remove_device_by_path(dev_path);
        return MethodResult::ok();
    }});

    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [name, val] : objects_[path]["org.bluez.Adapter1"]) {
        std::string prop_name = name;
        if (prop_name == "Powered") {
            i->properties.push_back({prop_name, "b",
                [this, path, prop_name] {
                    std::lock_guard<std::mutex> l(mu_);
                    return objects_[path]["org.bluez.Adapter1"][prop_name];
                },
                [this, path](const Value& v) -> MethodResult {
                    {
                        std::lock_guard<std::mutex> l(mu_);
                        objects_[path]["org.bluez.Adapter1"]["Powered"] = v;
                        calls_.push_back(std::string("SetPowered(") + (v.as_bool() ? "true" : "false") + ")");
                    }
                    conn_->emit_properties_changed(path, "org.bluez.Adapter1", {"Powered"});
                    return MethodResult::ok();
                },
                "true"});
        } else {
            i->properties.push_back({prop_name, val.sig,
                [this, path, prop_name] {
                    std::lock_guard<std::mutex> l(mu_);
                    return objects_[path]["org.bluez.Adapter1"][prop_name];
                },
                std::function<MethodResult(const Value&)>(), "true"});
        }
    }
    return i;
}

std::shared_ptr<Interface> FakeBlueZ::make_device_iface(const std::string& path) {
    auto i = std::make_shared<Interface>();
    i->name = "org.bluez.Device1";
    i->methods.push_back({"Connect", "", "", {}, {}, [this, path](const MethodCall&) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            calls_.push_back("Connect(" + path + ")");
            objects_[path]["org.bluez.Device1"]["Connected"] = Value::boolean(true);
        }
        conn_->emit_properties_changed(path, "org.bluez.Device1", {"Connected"});
        return MethodResult::ok();
    }});
    i->methods.push_back({"Disconnect", "", "", {}, {}, [this, path](const MethodCall&) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            calls_.push_back("Disconnect(" + path + ")");
            objects_[path]["org.bluez.Device1"]["Connected"] = Value::boolean(false);
        }
        conn_->emit_properties_changed(path, "org.bluez.Device1", {"Connected"});
        return MethodResult::ok();
    }});
    i->methods.push_back({"Pair", "", "", {}, {}, [this, path](const MethodCall&) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            calls_.push_back("Pair(" + path + ")");
            objects_[path]["org.bluez.Device1"]["Paired"] = Value::boolean(true);
        }
        conn_->emit_properties_changed(path, "org.bluez.Device1", {"Paired"});
        return MethodResult::ok();
    }});

    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [name, val] : objects_[path]["org.bluez.Device1"]) {
        std::string prop_name = name;
        i->properties.push_back({prop_name, val.sig,
            [this, path, prop_name] {
                std::lock_guard<std::mutex> l(mu_);
                return objects_[path]["org.bluez.Device1"][prop_name];
            },
            std::function<MethodResult(const Value&)>(), "true"});
    }
    return i;
}

void FakeBlueZ::add_adapter(const std::string& path, const Props& props) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        objects_[path]["org.bluez.Adapter1"] = props;
    }
    conn_->export_interface(path, make_adapter_iface(path), nullptr);

    std::vector<std::pair<std::string, Value>> p_list(props.begin(), props.end());
    std::vector<std::pair<Value, Value>> iface_list{
        {Value::str("org.bluez.Adapter1"), Value::vardict(p_list)}};
    conn_->emit_signal("/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                       {Value::obj(path), Value::dict("s", "a{sv}", iface_list)});
}

void FakeBlueZ::add_device(const std::string& path, const Props& props) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        objects_[path]["org.bluez.Device1"] = props;
    }
    conn_->export_interface(path, make_device_iface(path), nullptr);

    std::vector<std::pair<std::string, Value>> p_list(props.begin(), props.end());
    std::vector<std::pair<Value, Value>> iface_list{
        {Value::str("org.bluez.Device1"), Value::vardict(p_list)}};
    conn_->emit_signal("/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                       {Value::obj(path), Value::dict("s", "a{sv}", iface_list)});
}

void FakeBlueZ::remove_device_by_path(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        objects_.erase(path);
    }
    conn_->unexport_path(path);
    conn_->emit_signal("/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                       {Value::obj(path), Value::strings({"org.bluez.Device1"})});
}

void FakeBlueZ::set_property(const std::string& path, const std::string& iface, const std::string& name,
                             const Value& val) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        objects_[path][iface][name] = val;
    }
    conn_->emit_properties_changed(path, iface, {name});
}

std::vector<std::string> FakeBlueZ::calls() const {
    std::lock_guard<std::mutex> lock(mu_);
    return calls_;
}

}  // namespace bstest
