#include "linux/fakes/fake_udisks.h"

namespace bstest {

using namespace brosys::dbus;

namespace {
constexpr const char* kRoot = "/org/freedesktop/UDisks2";
constexpr const char* kOM = "org.freedesktop.DBus.ObjectManager";
constexpr const char* kFilesystem = "org.freedesktop.UDisks2.Filesystem";
constexpr const char* kDrive = "org.freedesktop.UDisks2.Drive";
constexpr const char* kBlock = "org.freedesktop.UDisks2.Block";

std::string option_flag(const MethodCall& c, const char* key) {
    if (c.args.empty()) return {};
    const Value* v = c.args[0].lookup(key);
    return v && v->as_bool() ? std::string(",") + key : std::string();
}
}  // namespace

Value FakeUDisks::bytestring(const std::string& s) {
    Value::Bytes b(s.begin(), s.end());
    b.push_back(0);
    return Value::bytes(std::move(b));
}

Value FakeUDisks::bytestrings(const std::vector<std::string>& v) {
    Value::Items items;
    for (auto& s : v) items.push_back(bytestring(s));
    return Value::array("ay", std::move(items));
}

FakeUDisks::FakeUDisks(const std::string& bus_address) {
    conn_ = Connection::open(BusKind::System, bus_address, "fake-udisks", &error_);
    if (!conn_) return;
    auto om = std::make_shared<Interface>();
    om->name = kOM;
    om->methods.push_back({"GetManagedObjects", "", "a{oa{sa{sv}}}", {}, {"objects"},
                           [this](const MethodCall&) { return MethodResult::ok({managed_objects()}); }});
    om->signals.push_back({"InterfacesAdded", "oa{sa{sv}}", {"object_path", "interfaces_and_properties"}});
    om->signals.push_back({"InterfacesRemoved", "oas", {"object_path", "interfaces"}});
    if (!conn_->export_interface(kRoot, om, &error_)) return;
    if (conn_->request_name("org.freedesktop.UDisks2", name_flags::AllowReplacement | name_flags::ReplaceExisting,
                            &error_) != NameRequest::PrimaryOwner)
        return;
    ok_ = true;
}

FakeUDisks::~FakeUDisks() {
    if (conn_) conn_->shutdown();
}

Value FakeUDisks::managed_objects() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<Value, Value>> objs;
    for (auto& [path, ifaces] : objects_) {
        std::vector<std::pair<Value, Value>> entries;
        for (auto& [iface, props] : ifaces) {
            std::vector<std::pair<std::string, Value>> p(props.begin(), props.end());
            entries.emplace_back(Value::str(iface), Value::vardict(p));
        }
        objs.emplace_back(Value::obj(path), Value::dict("s", "a{sv}", entries));
    }
    return Value::dict("o", "a{sa{sv}}", objs);
}

MethodResult FakeUDisks::record(const std::string& method, const std::string& call) {
    std::lock_guard<std::mutex> lock(mu_);
    calls_.push_back(call);
    auto it = failures_.find(method);
    if (it == failures_.end()) return MethodResult::ok();
    auto err = it->second;
    failures_.erase(it);
    return MethodResult::error(err.first, err.second);
}

std::shared_ptr<Interface> FakeUDisks::make_interface(const std::string& path, const std::string& iface) {
    auto i = std::make_shared<Interface>();
    i->name = iface;
    if (iface == kFilesystem) {
        i->methods.push_back({"Mount", "a{sv}", "s", {"options"}, {"mount_path"}, [this, path](const MethodCall& c) {
            std::string fstype, opts;
            if (!c.args.empty()) {
                if (const Value* v = c.args[0].lookup("fstype")) fstype = v->as_string();
                if (const Value* v = c.args[0].lookup("options")) opts = v->as_string();
            }
            MethodResult r = record("Mount", "Mount(" + path + (fstype.empty() ? "" : "," + fstype) +
                                                 (opts.empty() ? "" : "," + opts) + ")");
            if (r.kind == MethodResult::Kind::Error) return r;
            std::string where;
            {
                std::lock_guard<std::mutex> lock(mu_);
                Props& fs = objects_[path][kFilesystem];
                if (!fs["MountPoints"].items().empty())
                    return MethodResult::error("org.freedesktop.UDisks2.Error.AlreadyMounted", "already mounted");
                std::string label = objects_[path][kBlock]["IdLabel"].as_string();
                if (label.empty()) label = path.substr(path.rfind('/') + 1);
                where = "/run/media/fake/" + label;
                fs["MountPoints"] = bytestrings({where});
            }
            conn_->emit_properties_changed(path, kFilesystem, {"MountPoints"});
            return MethodResult::ok({Value::str(where)});
        }});
        i->methods.push_back({"Unmount", "a{sv}", "", {"options"}, {}, [this, path](const MethodCall& c) {
            MethodResult r = record("Unmount", "Unmount(" + path + option_flag(c, "force") + ")");
            if (r.kind == MethodResult::Kind::Error) return r;
            {
                std::lock_guard<std::mutex> lock(mu_);
                Props& fs = objects_[path][kFilesystem];
                if (fs["MountPoints"].items().empty())
                    return MethodResult::error("org.freedesktop.UDisks2.Error.NotMounted", "not mounted");
                fs["MountPoints"] = bytestrings({});
            }
            conn_->emit_properties_changed(path, kFilesystem, {"MountPoints"});
            return MethodResult::ok();
        }});
    } else if (iface == kDrive) {
        i->methods.push_back({"Eject", "a{sv}", "", {"options"}, {}, [this, path](const MethodCall&) {
            MethodResult r = record("Eject", "Eject(" + path + ")");
            if (r.kind == MethodResult::Kind::Error) return r;
            {
                std::lock_guard<std::mutex> lock(mu_);
                objects_[path][kDrive]["MediaAvailable"] = Value::boolean(false);
            }
            conn_->emit_properties_changed(path, kDrive, {"MediaAvailable"});
            return MethodResult::ok();
        }});
        i->methods.push_back({"PowerOff", "a{sv}", "", {"options"}, {}, [this, path](const MethodCall&) {
            MethodResult r = record("PowerOff", "PowerOff(" + path + ")");
            if (r.kind == MethodResult::Kind::Error) return r;
            // The drive and its block devices go away, after the reply.
            conn_->add_timer(std::chrono::milliseconds(1), [this, path] {
                std::vector<std::string> gone{path};
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    for (auto& [p, o] : objects_) {
                        auto b = o.find(kBlock);
                        if (b != o.end() && b->second["Drive"].as_string() == path) gone.push_back(p);
                    }
                }
                for (auto& p : gone) remove_object(p);
            });
            return MethodResult::ok();
        }});
    } else if (iface == "org.freedesktop.UDisks2.Drive.Ata" || iface == "org.freedesktop.UDisks2.NVMe.Controller") {
        const bool nvme = iface != "org.freedesktop.UDisks2.Drive.Ata";
        i->methods.push_back({"SmartGetAttributes", "a{sv}", nvme ? "a{sv}" : "a(ysqiiixia{sv})", {"options"},
                              {"attributes"}, [this, path, nvme](const MethodCall&) {
            MethodResult r = record("SmartGetAttributes", "SmartGetAttributes(" + path + ")");
            if (r.kind == MethodResult::Kind::Error) return r;
            std::lock_guard<std::mutex> lock(mu_);
            auto it = smart_.find(path);
            if (it == smart_.end())
                return nvme ? MethodResult::ok({Value::vardict({})})
                            : MethodResult::ok({Value::array("(ysqiiixia{sv})", {})});
            return MethodResult::ok({it->second});
        }});
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [name, val] : objects_[path][iface]) {
        const std::string prop_name = name;
        i->properties.push_back({prop_name, val.sig,
                                 [this, path, iface, prop_name] {
                                     std::lock_guard<std::mutex> l(mu_);
                                     return objects_[path][iface][prop_name];
                                 },
                                 std::function<MethodResult(const Value&)>(), "true"});
    }
    return i;
}

void FakeUDisks::add_object(const std::string& path, const Object& object) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        objects_[path] = object;
    }
    std::vector<std::pair<Value, Value>> entries;
    for (auto& [iface, props] : object) {
        conn_->export_interface(path, make_interface(path, iface), nullptr);
        std::vector<std::pair<std::string, Value>> p(props.begin(), props.end());
        entries.emplace_back(Value::str(iface), Value::vardict(p));
    }
    conn_->emit_signal(kRoot, kOM, "InterfacesAdded", {Value::obj(path), Value::dict("s", "a{sv}", entries)});
}

void FakeUDisks::remove_object(const std::string& path) {
    std::vector<std::string> ifaces;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = objects_.find(path);
        if (it == objects_.end()) return;
        for (auto& [iface, props] : it->second) ifaces.push_back(iface);
        objects_.erase(it);
    }
    conn_->unexport_path(path);
    conn_->emit_signal(kRoot, kOM, "InterfacesRemoved", {Value::obj(path), Value::strings(ifaces)});
}

void FakeUDisks::set_property(const std::string& path, const std::string& iface, const std::string& name,
                              const Value& v) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        objects_[path][iface][name] = v;
    }
    conn_->emit_properties_changed(path, iface, {name});
}

void FakeUDisks::set_smart_attributes(const std::string& path, const Value& reply) {
    std::lock_guard<std::mutex> lock(mu_);
    smart_[path] = reply;
}

void FakeUDisks::fail_next(const std::string& method, const std::string& error_name, const std::string& message) {
    std::lock_guard<std::mutex> lock(mu_);
    failures_[method] = {error_name, message};
}

std::vector<std::string> FakeUDisks::calls() const {
    std::lock_guard<std::mutex> lock(mu_);
    return calls_;
}

}  // namespace bstest
