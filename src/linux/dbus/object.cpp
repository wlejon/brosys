// Object export: dispatch of incoming method calls to Interface
// descriptions, the standard Introspectable / Properties interfaces, and
// deferred replies.
#include "linux/dbus/connection.h"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cstring>
#include <set>

namespace brosys::dbus {

namespace {

const char* safe(const char* s) { return s ? s : ""; }

constexpr const char* kIntrospectable = "org.freedesktop.DBus.Introspectable";
constexpr const char* kProperties = "org.freedesktop.DBus.Properties";
constexpr const char* kPeer = "org.freedesktop.DBus.Peer";
constexpr const char* kErrorUnknownInterface = "org.freedesktop.DBus.Error.UnknownInterface";

std::string xml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

void append_args(std::string& xml, const std::string& sig, const std::vector<std::string>& names, const char* dir) {
    auto types = split_signature(sig);
    for (size_t i = 0; i < types.size(); ++i) {
        xml += "      <arg";
        if (i < names.size() && !names[i].empty()) xml += " name=\"" + xml_escape(names[i]) + "\"";
        xml += " type=\"" + xml_escape(types[i]) + "\"";
        if (dir) xml += std::string(" direction=\"") + dir + "\"";
        xml += "/>\n";
    }
}

int reply_error(sd_bus_message* m, const std::string& name, const std::string& message) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    sd_bus_error_set(&e, name.c_str(), message.c_str());
    int r = sd_bus_reply_method_error(m, &e);
    sd_bus_error_free(&e);
    return r < 0 ? r : 1;
}

int reply_values(sd_bus_message* m, const Args& out) {
    if (!sd_bus_message_get_expect_reply(m)) return 1;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r >= 0) r = append_all(reply, out);
    if (r >= 0) r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    if (r < 0) return reply_error(m, kErrorFailed, std::string("reply: ") + std::strerror(-r));
    return 1;
}

}  // namespace

// ---------------------------------------------------------------- Interface

const Interface::Method* Interface::find_method(const std::string& n) const {
    for (auto& m : methods)
        if (m.name == n) return &m;
    return nullptr;
}

const Interface::Property* Interface::find_property(const std::string& n) const {
    for (auto& p : properties)
        if (p.name == n) return &p;
    return nullptr;
}

std::string introspect_xml(const std::vector<std::shared_ptr<Interface>>& interfaces,
                           const std::vector<std::string>& children) {
    std::string xml =
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
        " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n";
    xml +=
        " <interface name=\"org.freedesktop.DBus.Peer\">\n"
        "  <method name=\"Ping\"/>\n"
        "  <method name=\"GetMachineId\">\n      <arg type=\"s\" name=\"machine_uuid\" direction=\"out\"/>\n  </method>\n"
        " </interface>\n"
        " <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
        "  <method name=\"Introspect\">\n      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n  </method>\n"
        " </interface>\n"
        " <interface name=\"org.freedesktop.DBus.Properties\">\n"
        "  <method name=\"Get\">\n      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
        "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
        "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n  </method>\n"
        "  <method name=\"GetAll\">\n      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
        "      <arg name=\"props\" type=\"a{sv}\" direction=\"out\"/>\n  </method>\n"
        "  <method name=\"Set\">\n      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
        "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
        "      <arg name=\"value\" type=\"v\" direction=\"in\"/>\n  </method>\n"
        "  <signal name=\"PropertiesChanged\">\n      <arg type=\"s\" name=\"interface_name\"/>\n"
        "      <arg type=\"a{sv}\" name=\"changed_properties\"/>\n"
        "      <arg type=\"as\" name=\"invalidated_properties\"/>\n  </signal>\n"
        " </interface>\n";
    for (auto& iface : interfaces) {
        xml += " <interface name=\"" + xml_escape(iface->name) + "\">\n";
        for (auto& m : iface->methods) {
            xml += "  <method name=\"" + xml_escape(m.name) + "\">\n";
            append_args(xml, m.in_sig, m.in_names, "in");
            append_args(xml, m.out_sig, m.out_names, "out");
            xml += "  </method>\n";
        }
        for (auto& s : iface->signals) {
            xml += "  <signal name=\"" + xml_escape(s.name) + "\">\n";
            append_args(xml, s.sig, s.arg_names, nullptr);
            xml += "  </signal>\n";
        }
        for (auto& p : iface->properties) {
            const char* access = p.set ? (p.get ? "readwrite" : "write") : "read";
            xml += "  <property name=\"" + xml_escape(p.name) + "\" type=\"" + xml_escape(p.sig) + "\" access=\"" +
                   access + "\"";
            if (p.emits_changed != "true") {
                xml += ">\n   <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"" +
                       xml_escape(p.emits_changed) + "\"/>\n  </property>\n";
            } else {
                xml += "/>\n";
            }
        }
        xml += " </interface>\n";
    }
    for (auto& c : children) xml += " <node name=\"" + xml_escape(c) + "\"/>\n";
    xml += "</node>\n";
    return xml;
}

// ---------------------------------------------------------------- MethodCall

uint32_t MethodCall::sender_pid() const {
    if (!message) return 0;
    sd_bus_creds* creds = nullptr;
    pid_t pid = 0;
    if (sd_bus_query_sender_creds(message, SD_BUS_CREDS_PID, &creds) >= 0) {
        if (sd_bus_creds_get_pid(creds, &pid) < 0) pid = 0;
        sd_bus_creds_unref(creds);
    }
    if (pid <= 0 && connection && !sender.empty()) {
        Reply r = connection->call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                   "GetConnectionUnixProcessID", {Value::str(sender)});
        if (r.ok && !r.values.empty()) pid = static_cast<pid_t>(r.values[0].as_uint());
    }
    return pid > 0 ? static_cast<uint32_t>(pid) : 0;
}

std::shared_ptr<DeferredReply> MethodCall::defer() const {
    return std::make_shared<DeferredReply>(connection, message);
}

// ---------------------------------------------------------------- DeferredReply

DeferredReply::DeferredReply(Connection* conn, sd_bus_message* call) : conn_(conn), call_(sd_bus_message_ref(call)) {}

DeferredReply::~DeferredReply() {
    if (!done_) fail(kErrorFailed, "the service dropped the request");
}

void DeferredReply::send(std::function<int(sd_bus_message*)> build) {
    if (done_) return;
    done_ = true;
    sd_bus_message* call = call_;
    call_ = nullptr;
    conn_->post([call, build = std::move(build)] {
        if (call && build) build(call);
        sd_bus_message_unref(call);
    });
}

void DeferredReply::reply(const Args& out) {
    send([out](sd_bus_message* m) { return reply_values(m, out); });
}

void DeferredReply::fail(const std::string& error_name, const std::string& message) {
    send([error_name, message](sd_bus_message* m) { return reply_error(m, error_name, message); });
}

// ---------------------------------------------------------------- dispatch

std::vector<std::string> Connection::child_names(const std::string& path) const {
    std::set<std::string> names;
    std::string prefix = path == "/" ? "/" : path + "/";
    for (auto& [p, node] : nodes_) {
        if (p.size() <= prefix.size() || p.compare(0, prefix.size(), prefix) != 0) continue;
        std::string rest = p.substr(prefix.size());
        names.insert(rest.substr(0, rest.find('/')));
    }
    return {names.begin(), names.end()};
}

int Connection::reply_properties(sd_bus_message* m, Node& node, const std::string& member) {
    Args args;
    std::string err;
    if (!read_all(m, args, &err)) return reply_error(m, kErrorInvalidArgs, err);
    std::string sig = safe(sd_bus_message_get_signature(m, true));
    auto find_iface = [&](const std::string& name) -> std::shared_ptr<Interface> {
        for (auto& i : node.interfaces)
            if (i->name == name) return i;
        return nullptr;
    };
    if (member == "Get") {
        if (sig != "ss") return reply_error(m, kErrorInvalidArgs, "expected (ss)");
        auto iface = find_iface(args[0].as_string());
        if (!iface) return reply_error(m, kErrorUnknownInterface, "no interface " + args[0].as_string());
        auto* p = iface->find_property(args[1].as_string());
        if (!p || !p->get) return reply_error(m, kErrorUnknownProperty, "no property " + args[1].as_string());
        return reply_values(m, {Value::variant(p->get())});
    }
    if (member == "GetAll") {
        if (sig != "s") return reply_error(m, kErrorInvalidArgs, "expected (s)");
        std::vector<std::pair<std::string, Value>> all;
        if (auto iface = find_iface(args[0].as_string())) {
            for (auto& p : iface->properties)
                if (p.get) all.emplace_back(p.name, p.get());
        } else if (!args[0].as_string().empty() && args[0].as_string() != kPeer &&
                   args[0].as_string() != kIntrospectable && args[0].as_string() != kProperties) {
            return reply_error(m, kErrorUnknownInterface, "no interface " + args[0].as_string());
        }
        return reply_values(m, {Value::vardict(std::move(all))});
    }
    if (member == "Set") {
        if (sig != "ssv") return reply_error(m, kErrorInvalidArgs, "expected (ssv)");
        auto iface = find_iface(args[0].as_string());
        if (!iface) return reply_error(m, kErrorUnknownInterface, "no interface " + args[0].as_string());
        auto* p = iface->find_property(args[1].as_string());
        if (!p) return reply_error(m, kErrorUnknownProperty, "no property " + args[1].as_string());
        if (!p->set) return reply_error(m, kErrorPropertyReadOnly, "property " + p->name + " is read-only");
        const Value& v = args[2].unwrap();
        if (v.sig != p->sig) return reply_error(m, kErrorInvalidArgs, "property " + p->name + " has type " + p->sig);
        std::string path = node.path, iname = iface->name, pname = p->name;
        MethodResult res = p->set(v);
        if (res.kind == MethodResult::Kind::Error) return reply_error(m, res.error_name, res.error_message);
        int r = reply_values(m, {});
        emit_properties_changed(path, iname, {pname});
        return r;
    }
    return reply_error(m, kErrorUnknownMethod, "no method " + member + " in " + kProperties);
}

int Connection::dispatch(sd_bus_message* m, Node& node) {
    if (!sd_bus_message_is_method_call(m, nullptr, nullptr)) return 0;
    std::string iface_name = safe(sd_bus_message_get_interface(m));
    std::string member = safe(sd_bus_message_get_member(m));

    if ((iface_name == kIntrospectable || iface_name.empty()) && member == "Introspect")
        return reply_values(m, {Value::str(introspect_xml(node.interfaces, child_names(node.path)))});
    if (iface_name == kProperties) return reply_properties(m, node, member);
    if (iface_name == kPeer) return 0;  // sd-bus answers Ping / GetMachineId

    std::shared_ptr<Interface> iface;
    const Interface::Method* method = nullptr;
    bool iface_known = iface_name.empty();
    for (auto& i : node.interfaces) {
        if (!iface_name.empty() && i->name != iface_name) continue;
        iface_known = true;
        if (auto* mm = i->find_method(member)) {
            iface = i;
            method = mm;
            break;
        }
    }
    if (!method) {
        if (!iface_known) return reply_error(m, kErrorUnknownInterface, "no interface " + iface_name + " at " + node.path);
        return reply_error(m, kErrorUnknownMethod, "no method " + member + " at " + node.path);
    }
    std::string sig = safe(sd_bus_message_get_signature(m, true));
    if (sig != method->in_sig)
        return reply_error(m, kErrorInvalidArgs,
                           "method " + member + " takes (" + method->in_sig + "), got (" + sig + ")");

    MethodCall call;
    call.sender = safe(sd_bus_message_get_sender(m));
    call.path = node.path;
    call.interface = iface->name;
    call.member = member;
    call.message = m;
    call.connection = this;
    std::string err;
    if (!read_all(m, call.args, &err)) return reply_error(m, kErrorInvalidArgs, err);

    std::string out_sig = method->out_sig;
    auto handler = method->handler;  // the handler may re-export and invalidate `method`
    MethodResult res = handler ? handler(call) : MethodResult::error(kErrorFailed, "not implemented");
    switch (res.kind) {
        case MethodResult::Kind::Deferred: return 1;
        case MethodResult::Kind::Error: return reply_error(m, res.error_name, res.error_message);
        case MethodResult::Kind::Ok: break;
    }
    if (signature_of(res.out) != out_sig)
        return reply_error(m, kErrorFailed, "service bug: " + member + " returned (" + signature_of(res.out) +
                                                "), declared (" + out_sig + ")");
    return reply_values(m, res.out);
}

}  // namespace brosys::dbus
