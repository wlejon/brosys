#pragma once

#include "linux/dbus/connection.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bstest {

class FakeBlueZ {
public:
    using Props = std::map<std::string, brosys::dbus::Value>;
    using Object = std::map<std::string, Props>;

    explicit FakeBlueZ(const std::string& bus_address);
    ~FakeBlueZ();

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    void add_adapter(const std::string& path, const Props& props);
    void add_device(const std::string& path, const Props& props);
    void remove_device_by_path(const std::string& path);
    void set_property(const std::string& path, const std::string& iface, const std::string& name,
                      const brosys::dbus::Value& val);

    std::vector<std::string> calls() const;

private:
    std::shared_ptr<brosys::dbus::Interface> make_manager_iface();
    std::shared_ptr<brosys::dbus::Interface> make_adapter_iface(const std::string& path);
    std::shared_ptr<brosys::dbus::Interface> make_device_iface(const std::string& path);
    brosys::dbus::Value managed_objects() const;

    mutable std::mutex mu_;
    std::map<std::string, Object> objects_;
    std::vector<std::string> calls_;
    bool ok_ = false;
    std::string error_;
    std::unique_ptr<brosys::dbus::Connection> conn_;
};

}  // namespace bstest
