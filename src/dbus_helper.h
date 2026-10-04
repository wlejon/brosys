#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <variant>
#include <unordered_map>
#include <cstdint>

#if defined(__linux__)
#include <systemd/sd-bus.h>
#endif

namespace brosys::dbus {

enum class BusType {
    System,
    Session
};

using DBusVariant = std::variant<
    std::monostate,
    bool,
    uint8_t,
    int16_t,
    uint16_t,
    int32_t,
    uint32_t,
    int64_t,
    uint64_t,
    double,
    std::string,
    std::vector<std::string>,
    std::vector<uint8_t>
>;

struct MethodCall {
    std::string destination;
    std::string path;
    std::string interface_name;
    std::string method_name;
};

struct SignalMessage {
    std::string path;
    std::string interface_name;
    std::string member;
    std::string sender;
};

class DBusConnection {
public:
    DBusConnection() = default;
    ~DBusConnection();

    DBusConnection(const DBusConnection&) = delete;
    DBusConnection& operator=(const DBusConnection&) = delete;
    DBusConnection(DBusConnection&& other) noexcept;
    DBusConnection& operator=(DBusConnection&& other) noexcept;

    static std::unique_ptr<DBusConnection> open(BusType type);

    [[nodiscard]] bool is_connected() const;
    void close();

    bool request_name(const std::string& well_known_name);
    bool release_name(const std::string& well_known_name);

    // Call remote method with simple string/variant arguments
    bool call_method(const MethodCall& call,
                     const std::vector<DBusVariant>& args,
                     std::vector<DBusVariant>& out_results);

    // Get a property from an object
    bool get_property(const std::string& dest,
                      const std::string& path,
                      const std::string& iface,
                      const std::string& prop_name,
                      DBusVariant& out_value);

    // Set a property on an object
    bool set_property(const std::string& dest,
                      const std::string& path,
                      const std::string& iface,
                      const std::string& prop_name,
                      const DBusVariant& value);

    // Emit a signal
    bool emit_signal(const std::string& path,
                     const std::string& iface,
                     const std::string& signal_name,
                     const std::vector<DBusVariant>& args);

    // Poll / process incoming messages (returns number of messages processed)
    int process();
    bool wait(uint64_t timeout_usec = 100000);

#if defined(__linux__)
    sd_bus* raw_bus() const { return bus_; }
#endif

private:
#if defined(__linux__)
    sd_bus* bus_ = nullptr;
#else
    void* bus_ = nullptr;
#endif
    bool connected_ = false;
};

} // namespace brosys::dbus
