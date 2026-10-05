#include "linux/network/nm_model.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>

namespace brosys::nm {

namespace {

const Props* iface_of(const Objects& objects, const std::string& path, const char* iface) {
    auto o = objects.find(path);
    if (o == objects.end()) return nullptr;
    auto i = o->second.find(iface);
    return i == o->second.end() ? nullptr : &i->second;
}

const dbus::Value* prop(const Props* p, const char* name) {
    if (!p) return nullptr;
    auto it = p->find(name);
    return it == p->end() ? nullptr : &it->second;
}

std::string str_prop(const Props* p, const char* name) {
    auto* v = prop(p, name);
    return v ? v->as_string() : std::string();
}

uint64_t uint_prop(const Props* p, const char* name, uint64_t fallback = 0) {
    auto* v = prop(p, name);
    return v ? v->as_uint(fallback) : fallback;
}

bool bool_prop(const Props* p, const char* name, bool fallback) {
    auto* v = prop(p, name);
    return v ? v->as_bool(fallback) : fallback;
}

std::vector<std::string> paths_prop(const Props* p, const char* name) {
    auto* v = prop(p, name);
    return v ? v->as_strings() : std::vector<std::string>();
}

// "/" is NetworkManager's null object path.
std::string path_prop(const Props* p, const char* name) {
    std::string s = str_prop(p, name);
    return s == "/" ? std::string() : s;
}

std::string normalize_mac(std::string mac) {
    for (auto& c : mac) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool zero = true;
    for (char c : mac)
        if (c != '0' && c != ':') zero = false;
    return zero ? std::string() : mac;
}

std::string ntop(int family, const void* addr) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (!inet_ntop(family, addr, buf, sizeof buf)) return {};
    return buf;
}

IpConfig ip_config(const Objects& objects, const std::string& path, bool v6) {
    IpConfig c;
    const Props* p = iface_of(objects, path, v6 ? kIp6 : kIp4);
    if (!p) return c;
    if (auto* data = prop(p, "AddressData")) {
        for (auto& entry : data->items()) {
            auto* a = entry.lookup("address");
            auto* pre = entry.lookup("prefix");
            if (!a) continue;
            std::string s = a->as_string();
            if (pre) s += "/" + std::to_string(pre->as_uint());
            c.addresses.push_back(std::move(s));
        }
    }
    if (std::string gw = str_prop(p, "Gateway"); !gw.empty()) c.gateways.push_back(gw);
    if (!v6) {
        if (auto* data = prop(p, "NameserverData")) {
            for (auto& entry : data->items())
                if (auto* a = entry.lookup("address")) c.dns.push_back(a->as_string());
        } else if (auto* ns = prop(p, "Nameservers")) {  // au, network byte order (NM < 1.14)
            for (auto& v : ns->items()) {
                uint32_t be = static_cast<uint32_t>(v.as_uint());
                c.dns.push_back(ntop(AF_INET, &be));
            }
        }
    } else if (auto* ns = prop(p, "Nameservers")) {  // aay
        for (auto& v : ns->items()) {
            auto* b = v.as_bytes();
            if (b && b->size() == 16) c.dns.push_back(ntop(AF_INET6, b->data()));
        }
    }
    return c;
}

}  // namespace

Connectivity connectivity_from_nm(uint32_t v) {
    switch (v) {
        case 1: return Connectivity::None;
        case 2: return Connectivity::Portal;
        case 3: return Connectivity::Limited;
        case 4: return Connectivity::Full;
        default: return Connectivity::Unknown;
    }
}

LinkType link_type_from_device_type(uint32_t t) {
    switch (t) {
        case 0: return LinkType::Unknown;
        case 1: return LinkType::Ethernet;
        case 2: return LinkType::WiFi;
        case 8: return LinkType::Cellular;     // modem
        case 13: return LinkType::Bridge;
        case 29: return LinkType::Vpn;         // wireguard
        case 16:                               // tun
        case 17:                               // ip-tunnel
        case 19: return LinkType::Tunnel;      // vxlan
        case 10:                               // bond
        case 11:                               // vlan
        case 15:                               // team
        case 18:                               // macvlan
        case 20:                               // veth
        case 22:                               // dummy
        case 31:                               // vrf
        case 34: return LinkType::Virtual;     // ipvlan
        case 32: return LinkType::Loopback;
        default: return LinkType::Other;
    }
}

LinkType link_type_from_connection_type(const std::string& t) {
    if (t.empty()) return LinkType::Unknown;
    if (t == "802-3-ethernet") return LinkType::Ethernet;
    if (t == "802-11-wireless") return LinkType::WiFi;
    if (t == "gsm" || t == "cdma") return LinkType::Cellular;
    if (t == "loopback") return LinkType::Loopback;
    if (t == "bridge") return LinkType::Bridge;
    if (t == "vpn" || t == "wireguard") return LinkType::Vpn;
    if (t == "tun" || t == "ip-tunnel" || t == "vxlan") return LinkType::Tunnel;
    if (t == "vlan" || t == "bond" || t == "team" || t == "dummy" || t == "veth" || t == "macvlan" || t == "vrf" ||
        t == "ipvlan")
        return LinkType::Virtual;
    return LinkType::Other;
}

LinkState link_state_from_device_state(uint32_t s) {
    if (s == 10 || s == 20) return LinkState::Unavailable;   // unmanaged, unavailable
    if (s == 30 || s == 120) return LinkState::Disconnected;  // disconnected, failed
    if (s >= 40 && s <= 90) return LinkState::Connecting;     // prepare .. secondaries
    if (s == 100) return LinkState::Connected;
    if (s == 110) return LinkState::Disconnecting;
    return LinkState::Unknown;
}

LinkState link_state_from_active_state(uint32_t s) {
    switch (s) {
        case 1: return LinkState::Connecting;
        case 2: return LinkState::Connected;
        case 3: return LinkState::Disconnecting;
        case 4: return LinkState::Disconnected;
        default: return LinkState::Unknown;
    }
}

WifiSecurity security_from_flags(uint32_t flags, uint32_t wpa, uint32_t rsn) {
    constexpr uint32_t kPrivacy = 0x1;
    constexpr uint32_t kPsk = 0x100, k8021x = 0x200, kSae = 0x400, kOwe = 0x800, kOweTm = 0x1000,
                       kSuiteB = 0x2000;
    if (rsn & (kSuiteB)) return WifiSecurity::Wpa3Enterprise;
    if (rsn & kSae) return WifiSecurity::Wpa3Personal;
    if (rsn & (kOwe | kOweTm)) return WifiSecurity::Owe;
    if (rsn & k8021x) return WifiSecurity::Wpa2Enterprise;
    if (rsn & kPsk) return WifiSecurity::Wpa2Personal;
    if (wpa & k8021x) return WifiSecurity::WpaEnterprise;
    if (wpa & kPsk) return WifiSecurity::WpaPersonal;
    if (flags & kPrivacy) return WifiSecurity::Wep;
    return WifiSecurity::Open;
}

uint32_t channel_from_frequency(uint32_t f) {
    if (f == 2484) return 14;
    if (f >= 2412 && f <= 2472) return (f - 2407) / 5;
    if (f >= 5955 && f <= 7115) return (f - 5950) / 5;  // 6 GHz
    if (f >= 5000 && f < 5950) return (f - 5000) / 5;
    if (f >= 4910 && f <= 4980) return (f - 4000) / 5;  // 4.9 GHz (Japan)
    if (f >= 58320 && f <= 70200) return (f - 56160) / 2160;
    return 0;
}

std::vector<std::string> wifi_devices(const Objects& objects) {
    std::vector<std::string> out;
    for (auto& path : paths_prop(iface_of(objects, kPath, kIface), "Devices"))
        if (iface_of(objects, path, kWireless)) out.push_back(path);
    return out;
}

std::optional<int64_t> last_scan(const Objects& objects, const std::string& device_path) {
    const Props* w = iface_of(objects, device_path, kWireless);
    if (!w) return std::nullopt;
    auto* v = prop(w, "LastScan");
    return v ? v->as_int(-1) : -1;
}

std::vector<WifiAccessPoint> access_points(const Objects& objects, const std::string& device_path) {
    std::vector<WifiAccessPoint> out;
    const Props* w = iface_of(objects, device_path, kWireless);
    if (!w) return out;
    std::string active = path_prop(w, "ActiveAccessPoint");
    for (auto& ap_path : paths_prop(w, "AccessPoints")) {
        const Props* ap = iface_of(objects, ap_path, kAccessPoint);
        if (!ap) continue;
        WifiAccessPoint a;
        a.device_id = device_path;
        if (auto* ssid = prop(ap, "Ssid"); ssid && ssid->as_bytes())
            a.ssid.assign(ssid->as_bytes()->begin(), ssid->as_bytes()->end());
        a.bssid = normalize_mac(str_prop(ap, "HwAddress"));
        a.strength_percent = static_cast<uint8_t>(std::min<uint64_t>(uint_prop(ap, "Strength"), 100));
        a.frequency_mhz = static_cast<uint32_t>(uint_prop(ap, "Frequency"));
        a.channel = channel_from_frequency(a.frequency_mhz);
        a.security = security_from_flags(static_cast<uint32_t>(uint_prop(ap, "Flags")),
                                         static_cast<uint32_t>(uint_prop(ap, "WpaFlags")),
                                         static_cast<uint32_t>(uint_prop(ap, "RsnFlags")));
        a.active = ap_path == active;
        out.push_back(std::move(a));
    }
    return out;
}

NetworkState build_state(const Objects& objects) {
    NetworkState s;
    const Props* m = iface_of(objects, kPath, kIface);
    if (!m) return s;
    s.connectivity = connectivity_from_nm(static_cast<uint32_t>(uint_prop(m, "Connectivity")));
    s.networking_enabled = bool_prop(m, "NetworkingEnabled", true);
    s.wifi_enabled = bool_prop(m, "WirelessEnabled", false);
    s.wifi_hardware_enabled = bool_prop(m, "WirelessHardwareEnabled", false);

    std::string primary_ac = path_prop(m, "PrimaryConnection");
    if (const Props* pac = iface_of(objects, primary_ac, kActive)) {
        auto devs = paths_prop(pac, "Devices");
        if (!devs.empty()) s.primary_device = devs[0];
    }

    for (auto& path : paths_prop(m, "Devices")) {
        const Props* d = iface_of(objects, path, kDevice);
        if (!d) continue;
        NetDevice n;
        n.id = path;
        n.interface_name = str_prop(d, "Interface");
        n.description = str_prop(d, "Driver");
        n.type = link_type_from_device_type(static_cast<uint32_t>(uint_prop(d, "DeviceType")));
        n.state = link_state_from_device_state(static_cast<uint32_t>(uint_prop(d, "State")));
        n.managed = bool_prop(d, "Managed", true);
        std::string mac = str_prop(d, "HwAddress");
        const Props* wired = iface_of(objects, path, kWired);
        const Props* wireless = iface_of(objects, path, kWireless);
        if (mac.empty()) mac = str_prop(wired ? wired : wireless, "HwAddress");
        n.mac = normalize_mac(mac);
        if (wired) n.speed_mbps = uint_prop(wired, "Speed");
        else if (wireless) n.speed_mbps = uint_prop(wireless, "Bitrate") / 1000;  // Kb/s
        if (const Props* ac = iface_of(objects, path_prop(d, "ActiveConnection"), kActive))
            n.connection = str_prop(ac, "Id");
        n.ipv4 = ip_config(objects, path_prop(d, "Ip4Config"), false);
        n.ipv6 = ip_config(objects, path_prop(d, "Ip6Config"), true);
        n.is_primary = !s.primary_device.empty() && path == s.primary_device;
        s.devices.push_back(std::move(n));
    }

    for (auto& path : paths_prop(m, "ActiveConnections")) {
        const Props* a = iface_of(objects, path, kActive);
        if (!a) continue;
        ActiveConnection c;
        c.id = path;
        c.name = str_prop(a, "Id");
        c.uuid = str_prop(a, "Uuid");
        c.type = link_type_from_connection_type(str_prop(a, "Type"));
        if (bool_prop(a, "Vpn", false)) c.type = LinkType::Vpn;
        c.state = link_state_from_active_state(static_cast<uint32_t>(uint_prop(a, "State")));
        c.device_ids = paths_prop(a, "Devices");
        c.default4 = bool_prop(a, "Default", false);
        c.default6 = bool_prop(a, "Default6", false);
        s.active_connections.push_back(std::move(c));
    }
    return s;
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

}  // namespace brosys::nm
