// A scripted org.freedesktop.NetworkManager on a private bus: an
// ObjectManager at /org/freedesktop (GetManagedObjects, InterfacesAdded /
// InterfacesRemoved), objects whose properties the test edits (each edit
// emits PropertiesChanged the way NM does), and Device.Wireless.RequestScan
// whose outcome the test chooses.
#pragma once

#include "linux/dbus/connection.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bstest {

class FakeNM {
public:
    using Props = std::map<std::string, brosys::dbus::Value>;
    using Object = std::map<std::string, Props>;  // interface -> properties

    enum class ScanMode { Complete, Ignore, Fail };

    explicit FakeNM(const std::string& bus_address);
    ~FakeNM();
    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    // Exports (and announces with InterfacesAdded unless `quiet`).
    void add_object(const std::string& path, const Object& object, bool quiet = false);
    void remove_object(const std::string& path);
    // Changes properties of one interface; one PropertiesChanged signal.
    void set(const std::string& path, const std::string& iface, const Props& changed);
    // PropertiesChanged that only invalidates `names` (the values change silently first).
    void invalidate(const std::string& path, const std::string& iface, const Props& changed);
    // Runs `fn` on the fake's bus thread: signals emitted inside go out back to back.
    void batch(const std::function<void()>& fn);
    // Takes the well-known name (call after the initial objects are added).
    bool own_name();

    // RequestScan behaviour: Complete = after `delay_ms` run the scan hook,
    // then bump LastScan; Ignore = accept and never finish; Fail = error.
    void set_scan_mode(ScanMode mode, int delay_ms = 100);
    void set_scan_hook(std::function<void(const std::string& device)> hook);
    int scan_requests() const;

    struct AddAndActivateCall {
        brosys::dbus::Value connection;
        std::string device;
        std::string specific_object;
    };

    struct ActivateCall {
        std::string connection;
        std::string device;
        std::string specific_object;
    };

    // Configured connection profiles (Settings service)
    void add_connection(const std::string& path, const brosys::dbus::Value& settings);
    void remove_connection(const std::string& path);

    // Call history
    std::vector<AddAndActivateCall> add_and_activate_calls() const;
    std::vector<ActivateCall> activate_calls() const;
    std::vector<std::string> deactivate_calls() const;
    std::vector<std::string> device_disconnect_calls() const;

    // Hooks for testing failure or custom behavior
    void set_add_and_activate_hook(
        std::function<brosys::dbus::MethodResult(const brosys::dbus::Value&, const std::string&, const std::string&)> hook);
    void set_activate_hook(
        std::function<brosys::dbus::MethodResult(const std::string&, const std::string&, const std::string&)> hook);
    void set_deactivate_hook(std::function<brosys::dbus::MethodResult(const std::string&)> hook);
    void set_device_disconnect_hook(std::function<brosys::dbus::MethodResult(const std::string&)> hook);

private:
    std::shared_ptr<brosys::dbus::Interface> make_interface(const std::string& path, const std::string& iface);
    brosys::dbus::Value managed_objects() const;

    mutable std::mutex mu_;
    std::map<std::string, Object> objects_;
    std::map<std::string, brosys::dbus::Value> connections_;
    std::vector<AddAndActivateCall> add_and_activate_calls_;
    std::vector<ActivateCall> activate_calls_;
    std::vector<std::string> deactivate_calls_;
    std::vector<std::string> device_disconnect_calls_;

    std::function<brosys::dbus::MethodResult(const brosys::dbus::Value&, const std::string&, const std::string&)> add_and_activate_hook_;
    std::function<brosys::dbus::MethodResult(const std::string&, const std::string&, const std::string&)> activate_hook_;
    std::function<brosys::dbus::MethodResult(const std::string&)> deactivate_hook_;
    std::function<brosys::dbus::MethodResult(const std::string&)> device_disconnect_hook_;

    ScanMode scan_mode_ = ScanMode::Complete;
    int scan_delay_ms_ = 100;
    int scan_requests_ = 0;
    int64_t clock_ms_ = 1000;
    std::function<void(const std::string&)> scan_hook_;
    bool ok_ = false;
    std::string error_;
    std::unique_ptr<brosys::dbus::Connection> conn_;  // last
};

}  // namespace bstest
