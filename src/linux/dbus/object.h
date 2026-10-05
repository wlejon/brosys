// Exported D-Bus interfaces: methods, properties and signals described as
// data, dispatched by Connection, introspected from the same description.
#pragma once

#include "linux/dbus/value.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

typedef struct sd_bus_message sd_bus_message;

namespace brosys::dbus {

class Connection;

// A reply that is sent later (after an async call of our own, a user
// decision, ...). Thread-safe: reply()/fail() may be called from any
// thread, once; dropping it unanswered replies with an error.
class DeferredReply {
public:
    DeferredReply(Connection* conn, sd_bus_message* call);
    ~DeferredReply();
    DeferredReply(const DeferredReply&) = delete;
    DeferredReply& operator=(const DeferredReply&) = delete;
    void reply(const Args& out);
    void fail(const std::string& error_name, const std::string& message);

private:
    void send(std::function<int(sd_bus_message*)> build);
    Connection* conn_;
    sd_bus_message* call_;
    bool done_ = false;
};

struct MethodCall {
    std::string sender;
    std::string path;
    std::string interface;
    std::string member;
    Args args;

    // Bus thread only, during the handler: the raw message.
    sd_bus_message* message = nullptr;
    Connection* connection = nullptr;

    // The sender's pid (0 when unknown); bus thread only, during the handler.
    uint32_t sender_pid() const;
    // Take over replying; the handler then returns MethodResult::deferred().
    std::shared_ptr<DeferredReply> defer() const;
};

struct MethodResult {
    enum class Kind { Ok, Error, Deferred } kind = Kind::Ok;
    Args out;
    std::string error_name;
    std::string error_message;

    static MethodResult ok(Args out = Args()) { return {Kind::Ok, std::move(out), {}, {}}; }
    static MethodResult error(std::string name, std::string message) {
        return {Kind::Error, {}, std::move(name), std::move(message)};
    }
    static MethodResult deferred() { return {Kind::Deferred, {}, {}, {}}; }
};

// Standard error names.
inline constexpr const char* kErrorFailed = "org.freedesktop.DBus.Error.Failed";
inline constexpr const char* kErrorInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";
inline constexpr const char* kErrorUnknownMethod = "org.freedesktop.DBus.Error.UnknownMethod";
inline constexpr const char* kErrorUnknownProperty = "org.freedesktop.DBus.Error.UnknownProperty";
inline constexpr const char* kErrorPropertyReadOnly = "org.freedesktop.DBus.Error.PropertyReadOnly";

struct Interface {
    struct Method {
        std::string name;
        std::string in_sig;
        std::string out_sig;
        std::vector<std::string> in_names;   // for introspection (optional)
        std::vector<std::string> out_names;
        // Called with args already checked against in_sig. Out values must
        // match out_sig (checked; a mismatch becomes an error reply).
        std::function<MethodResult(const MethodCall&)> handler;
    };
    struct Property {
        std::string name;
        std::string sig;
        std::function<Value()> get;                         // returns a value of `sig`
        std::function<MethodResult(const Value&)> set;      // empty: read-only
        // "true" | "invalidates" | "const" | "false" (org.freedesktop.DBus.Property.EmitsChangedSignal)
        std::string emits_changed = "true";
    };
    struct Signal {
        std::string name;
        std::string sig;
        std::vector<std::string> arg_names;
    };

    std::string name;
    std::vector<Method> methods;
    std::vector<Property> properties;
    std::vector<Signal> signals;

    const Method* find_method(const std::string& n) const;
    const Property* find_property(const std::string& n) const;
};

// Introspection XML for one path: its interfaces plus the standard ones,
// and <node name="..."/> for each direct child.
std::string introspect_xml(const std::vector<std::shared_ptr<Interface>>& interfaces,
                           const std::vector<std::string>& children);

}  // namespace brosys::dbus
