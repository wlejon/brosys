#include "dbus_helper.h"
#include <iostream>

namespace brosys::dbus {

#if defined(__linux__)

DBusConnection::~DBusConnection() {
    close();
}

DBusConnection::DBusConnection(DBusConnection&& other) noexcept
    : bus_(other.bus_), connected_(other.connected_) {
    other.bus_ = nullptr;
    other.connected_ = false;
}

DBusConnection& DBusConnection::operator=(DBusConnection&& other) noexcept {
    if (this != &other) {
        close();
        bus_ = other.bus_;
        connected_ = other.connected_;
        other.bus_ = nullptr;
        other.connected_ = false;
    }
    return *this;
}

std::unique_ptr<DBusConnection> DBusConnection::open(BusType type) {
    auto conn = std::make_unique<DBusConnection>();
    int r = 0;
    if (type == BusType::System) {
        r = sd_bus_open_system(&conn->bus_);
    } else {
        r = sd_bus_open_user(&conn->bus_);
    }

    if (r < 0 || !conn->bus_) {
        conn->bus_ = nullptr;
        conn->connected_ = false;
        return nullptr;
    }

    conn->connected_ = true;
    return conn;
}

bool DBusConnection::is_connected() const {
    return connected_ && (bus_ != nullptr);
}

void DBusConnection::close() {
    if (bus_) {
        sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
    }
    connected_ = false;
}

bool DBusConnection::request_name(const std::string& well_known_name) {
    if (!is_connected()) return false;
    int r = sd_bus_request_name(bus_, well_known_name.c_str(), 0);
    return r >= 0;
}

bool DBusConnection::release_name(const std::string& well_known_name) {
    if (!is_connected()) return false;
    int r = sd_bus_release_name(bus_, well_known_name.c_str());
    return r >= 0;
}

bool DBusConnection::call_method(const MethodCall& call,
                                 const std::vector<DBusVariant>& args,
                                 std::vector<DBusVariant>& /*out_results*/) {
    if (!is_connected()) return false;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* m = nullptr;

    int r = sd_bus_message_new_method_call(bus_, &m,
                                           call.destination.c_str(),
                                           call.path.c_str(),
                                           call.interface_name.c_str(),
                                           call.method_name.c_str());
    if (r < 0) {
        return false;
    }

    for (const auto& arg : args) {
        std::visit([&](const auto& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, bool>) {
                int b = val ? 1 : 0;
                sd_bus_message_append(m, "b", b);
            } else if constexpr (std::is_same_v<T, uint8_t>) {
                sd_bus_message_append(m, "y", val);
            } else if constexpr (std::is_same_v<T, int16_t>) {
                sd_bus_message_append(m, "n", val);
            } else if constexpr (std::is_same_v<T, uint16_t>) {
                sd_bus_message_append(m, "q", val);
            } else if constexpr (std::is_same_v<T, int32_t>) {
                sd_bus_message_append(m, "i", val);
            } else if constexpr (std::is_same_v<T, uint32_t>) {
                sd_bus_message_append(m, "u", val);
            } else if constexpr (std::is_same_v<T, int64_t>) {
                sd_bus_message_append(m, "x", val);
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                sd_bus_message_append(m, "t", val);
            } else if constexpr (std::is_same_v<T, double>) {
                sd_bus_message_append(m, "d", val);
            } else if constexpr (std::is_same_v<T, std::string>) {
                sd_bus_message_append(m, "s", val.c_str());
            }
        }, arg);
    }

    sd_bus_message* reply = nullptr;
    r = sd_bus_call(bus_, m, 0, &error, &reply);
    sd_bus_message_unref(m);
    sd_bus_error_free(&error);

    if (r < 0) {
        return false;
    }

    if (reply) {
        sd_bus_message_unref(reply);
    }
    return true;
}

bool DBusConnection::get_property(const std::string& dest,
                                  const std::string& path,
                                  const std::string& iface,
                                  const std::string& prop_name,
                                  DBusVariant& out_value) {
    if (!is_connected()) return false;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    char* str_val = nullptr;
    int r = sd_bus_get_property_string(bus_, dest.c_str(), path.c_str(),
                                      iface.c_str(), prop_name.c_str(),
                                      &error, &str_val);
    if (r >= 0 && str_val) {
        out_value = std::string(str_val);
        free(str_val);
        return true;
    }
    sd_bus_error_free(&error);

    // Try double
    double dbl_val = 0.0;
    r = sd_bus_get_property_trivial(bus_, dest.c_str(), path.c_str(),
                                    iface.c_str(), prop_name.c_str(),
                                    &error, 'd', &dbl_val);
    if (r >= 0) {
        out_value = dbl_val;
        return true;
    }
    sd_bus_error_free(&error);

    // Try uint32
    uint32_t u_val = 0;
    r = sd_bus_get_property_trivial(bus_, dest.c_str(), path.c_str(),
                                    iface.c_str(), prop_name.c_str(),
                                    &error, 'u', &u_val);
    if (r >= 0) {
        out_value = u_val;
        return true;
    }
    sd_bus_error_free(&error);

    // Try int32
    int32_t i_val = 0;
    r = sd_bus_get_property_trivial(bus_, dest.c_str(), path.c_str(),
                                    iface.c_str(), prop_name.c_str(),
                                    &error, 'i', &i_val);
    if (r >= 0) {
        out_value = i_val;
        return true;
    }
    sd_bus_error_free(&error);

    // Try bool
    int b_val = 0;
    r = sd_bus_get_property_trivial(bus_, dest.c_str(), path.c_str(),
                                    iface.c_str(), prop_name.c_str(),
                                    &error, 'b', &b_val);
    if (r >= 0) {
        out_value = (b_val != 0);
        return true;
    }
    sd_bus_error_free(&error);

    return false;
}

bool DBusConnection::set_property(const std::string& dest,
                                  const std::string& path,
                                  const std::string& iface,
                                  const std::string& prop_name,
                                  const DBusVariant& value) {
    if (!is_connected()) return false;
    sd_bus_error error = SD_BUS_ERROR_NULL;

    int r = -1;
    std::visit([&](const auto& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::string>) {
            r = sd_bus_set_property(bus_, dest.c_str(), path.c_str(),
                                    iface.c_str(), prop_name.c_str(),
                                    &error, "s", val.c_str());
        } else if constexpr (std::is_same_v<T, bool>) {
            int b = val ? 1 : 0;
            r = sd_bus_set_property(bus_, dest.c_str(), path.c_str(),
                                    iface.c_str(), prop_name.c_str(),
                                    &error, "b", b);
        } else if constexpr (std::is_same_v<T, double>) {
            r = sd_bus_set_property(bus_, dest.c_str(), path.c_str(),
                                    iface.c_str(), prop_name.c_str(),
                                    &error, "d", val);
        }
    }, value);

    sd_bus_error_free(&error);
    return r >= 0;
}

bool DBusConnection::emit_signal(const std::string& path,
                                 const std::string& iface,
                                 const std::string& signal_name,
                                 const std::vector<DBusVariant>& args) {
    if (!is_connected()) return false;

    sd_bus_message* m = nullptr;
    int r = sd_bus_message_new_signal(bus_, &m, path.c_str(), iface.c_str(), signal_name.c_str());
    if (r < 0) return false;

    for (const auto& arg : args) {
        std::visit([&](const auto& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::string>) {
                sd_bus_message_append(m, "s", val.c_str());
            } else if constexpr (std::is_same_v<T, uint32_t>) {
                sd_bus_message_append(m, "u", val);
            } else if constexpr (std::is_same_v<T, int32_t>) {
                sd_bus_message_append(m, "i", val);
            } else if constexpr (std::is_same_v<T, bool>) {
                int b = val ? 1 : 0;
                sd_bus_message_append(m, "b", b);
            }
        }, arg);
    }

    r = sd_bus_send(bus_, m, nullptr);
    sd_bus_message_unref(m);
    return r >= 0;
}

int DBusConnection::process() {
    if (!is_connected()) return -1;
    int r = sd_bus_process(bus_, nullptr);
    return r;
}

bool DBusConnection::wait(uint64_t timeout_usec) {
    if (!is_connected()) return false;
    int r = sd_bus_wait(bus_, timeout_usec);
    return r >= 0;
}

#else

// Fallback non-Linux stubs
DBusConnection::~DBusConnection() = default;
DBusConnection::DBusConnection(DBusConnection&&) noexcept = default;
DBusConnection& DBusConnection::operator=(DBusConnection&&) noexcept = default;

std::unique_ptr<DBusConnection> DBusConnection::open(BusType /*type*/) {
    return nullptr;
}

bool DBusConnection::is_connected() const {
    return false;
}

void DBusConnection::close() {}

bool DBusConnection::request_name(const std::string& /*name*/) {
    return false;
}

bool DBusConnection::release_name(const std::string& /*name*/) {
    return false;
}

bool DBusConnection::call_method(const MethodCall& /*call*/,
                                 const std::vector<DBusVariant>& /*args*/,
                                 std::vector<DBusVariant>& /*out_results*/) {
    return false;
}

bool DBusConnection::get_property(const std::string& /*dest*/,
                                  const std::string& /*path*/,
                                  const std::string& /*iface*/,
                                  const std::string& /*prop_name*/,
                                  DBusVariant& /*out_value*/) {
    return false;
}

bool DBusConnection::set_property(const std::string& /*dest*/,
                                  const std::string& /*path*/,
                                  const std::string& /*iface*/,
                                  const std::string& /*prop_name*/,
                                  const DBusVariant& /*value*/) {
    return false;
}

bool DBusConnection::emit_signal(const std::string& /*path*/,
                                 const std::string& /*iface*/,
                                 const std::string& /*signal_name*/,
                                 const std::vector<DBusVariant>& /*args*/) {
    return false;
}

int DBusConnection::process() {
    return 0;
}

bool DBusConnection::wait(uint64_t /*timeout_usec*/) {
    return false;
}

#endif

} // namespace brosys::dbus
