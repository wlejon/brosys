// A scripted UDisks2 on a private bus: an ObjectManager at
// /org/freedesktop/UDisks2 over objects the test adds, with the methods
// DiskService calls (Filesystem.Mount / Unmount, Drive.Eject / PowerOff,
// Drive.Ata and NVMe.Controller SmartGetAttributes) acting on that state and
// signalling as UDisks does.
#pragma once

#include "linux/dbus/connection.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bstest {

class FakeUDisks {
public:
    using Value = brosys::dbus::Value;
    using Props = std::map<std::string, Value>;
    using Object = std::map<std::string, Props>;  // interface -> properties

    explicit FakeUDisks(const std::string& bus_address);
    ~FakeUDisks();

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    // Exports the object and signals InterfacesAdded.
    void add_object(const std::string& path, const Object& object);
    // Unexports it and signals InterfacesRemoved.
    void remove_object(const std::string& path);
    void set_property(const std::string& path, const std::string& iface, const std::string& name, const Value& v);

    // The reply of SmartGetAttributes on that drive (Ata: a(ysqiiixia{sv}); NVMe: a{sv}).
    void set_smart_attributes(const std::string& path, const Value& reply);
    // The next call of `method` fails with this D-Bus error.
    void fail_next(const std::string& method, const std::string& error_name, const std::string& message);

    // "Mount(/org/.../sdb1)", "Unmount(...,force)", "Eject(...)", "PowerOff(...)", "SmartGetAttributes(...)".
    std::vector<std::string> calls() const;

    static Value bytestring(const std::string& s);
    static Value bytestrings(const std::vector<std::string>& v);

private:
    std::shared_ptr<brosys::dbus::Interface> make_interface(const std::string& path, const std::string& iface);
    brosys::dbus::MethodResult record(const std::string& method, const std::string& call);
    Value managed_objects() const;

    mutable std::mutex mu_;
    std::map<std::string, Object> objects_;
    std::map<std::string, Value> smart_;
    std::map<std::string, std::pair<std::string, std::string>> failures_;
    std::vector<std::string> calls_;
    bool ok_ = false;
    std::string error_;
    std::unique_ptr<brosys::dbus::Connection> conn_;
};

}  // namespace bstest
