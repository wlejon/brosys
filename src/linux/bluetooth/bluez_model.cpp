#include "linux/bluetooth/bluez_model.h"

namespace brosys::bluez {

namespace {

const dbus::Value* prop(const Props* p, const char* name) {
    if (!p) return nullptr;
    auto it = p->find(name);
    return it == p->end() ? nullptr : &it->second;
}

std::string str_prop(const Props* p, const char* name) {
    auto* v = prop(p, name);
    return v ? v->as_string() : std::string();
}

bool bool_prop(const Props* p, const char* name, bool fallback) {
    auto* v = prop(p, name);
    return v ? v->as_bool(fallback) : fallback;
}

}  // namespace

std::optional<BluetoothAdapter> parse_adapter(const std::string& path, const Props& props) {
    BluetoothAdapter a;
    a.id = path;
    a.address = str_prop(&props, "Address");
    a.name = str_prop(&props, "Name");
    a.alias = str_prop(&props, "Alias");
    if (a.alias.empty()) a.alias = a.name;
    a.powered = bool_prop(&props, "Powered", false);
    a.discovering = bool_prop(&props, "Discovering", false);
    a.pairable = bool_prop(&props, "Pairable", true);
    a.discoverable = bool_prop(&props, "Discoverable", false);
    return a;
}

std::optional<BluetoothDevice> parse_device(const std::string& path, const Props& props) {
    BluetoothDevice d;
    d.id = path;
    d.mac = str_prop(&props, "Address");
    if (d.mac.empty()) return std::nullopt;
    d.adapter_id = str_prop(&props, "Adapter");
    d.name = str_prop(&props, "Name");
    d.alias = str_prop(&props, "Alias");
    if (d.alias.empty()) d.alias = d.name.empty() ? d.mac : d.name;
    d.icon = str_prop(&props, "Icon");
    d.paired = bool_prop(&props, "Paired", false);
    d.connected = bool_prop(&props, "Connected", false);
    d.trusted = bool_prop(&props, "Trusted", false);
    d.blocked = bool_prop(&props, "Blocked", false);
    if (auto* v = prop(&props, "RSSI")) {
        if (auto i = v->to_int()) {
            d.rssi = static_cast<int16_t>(*i);
        }
    }
    return d;
}

std::vector<BluetoothAdapter> build_adapters(const Objects& objects) {
    std::vector<BluetoothAdapter> list;
    for (auto& [path, ifaces] : objects) {
        auto it = ifaces.find(kAdapter);
        if (it != ifaces.end()) {
            if (auto a = parse_adapter(path, it->second)) {
                list.push_back(std::move(*a));
            }
        }
    }
    return list;
}

std::vector<BluetoothDevice> build_devices(const Objects& objects) {
    std::vector<BluetoothDevice> list;
    for (auto& [path, ifaces] : objects) {
        auto it = ifaces.find(kDevice);
        if (it != ifaces.end()) {
            if (auto d = parse_device(path, it->second)) {
                list.push_back(std::move(*d));
            }
        }
    }
    return list;
}

bool apply_properties_changed(Objects& objects, const std::string& path, const std::string& iface,
                              const dbus::Value& changed) {
    bool any = false;
    Props& p = objects[path][iface];
    for (auto& entry : changed.items()) {
        auto& kv = entry.items();
        if (kv.size() != 2) continue;
        auto& slot = p[kv[0].as_string()];
        const dbus::Value& v = kv[1].unwrap();
        if (!(slot == v)) {
            slot = v;
            any = true;
        }
    }
    return any;
}

void merge_interfaces(Objects& objects, const std::string& path, const dbus::Value& ifaces) {
    Object& o = objects[path];
    for (auto& entry : ifaces.items()) {
        auto& kv = entry.items();
        if (kv.size() != 2) continue;
        Props& p = o[kv[0].as_string()];
        for (auto& pe : kv[1].items()) {
            auto& pkv = pe.items();
            if (pkv.size() == 2) p[pkv[0].as_string()] = pkv[1].unwrap();
        }
    }
}

void merge_managed_objects(Objects& objects, const dbus::Value& managed) {
    for (auto& entry : managed.items()) {
        auto& kv = entry.items();
        if (kv.size() == 2) merge_interfaces(objects, kv[0].as_string(), kv[1]);
    }
}

}  // namespace brosys::bluez
