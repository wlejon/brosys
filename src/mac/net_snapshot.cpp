// macOS network snapshot.
//
// Devices are the hardware ports (SCNetworkInterfaceCopyAll, what
// `networksetup -listallhardwareports` lists) plus any interface an active
// service runs on (VPN tunnels). Addresses come from getifaddrs; routers and
// DNS from the active service's State:/Network/Service/<id>/{IPv4,IPv6,DNS};
// the primary from State:/Network/Global/IPv4 (else IPv6), as `scutil --nwi`
// reports it. A service with addresses is an ActiveConnection; its id and
// uuid are the SC service ID (itself a UUID).
#include "mac/net_snapshot.h"

#include "mac/cf.h"

#include <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <set>

namespace brosys::mac {

namespace {

struct IfInfo {
    std::string mac;
    uint64_t baud = 0;
    bool running = false;
    std::vector<std::string> v4, v6;
};

int prefix_len(const uint8_t* mask, size_t n) {
    int bits = 0;
    for (size_t i = 0; i < n; ++i) bits += std::popcount(static_cast<unsigned>(mask[i]));
    return bits;
}

std::map<std::string, IfInfo> read_ifaddrs() {
    std::map<std::string, IfInfo> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* p = list; p; p = p->ifa_next) {
        if (!p->ifa_name) continue;
        IfInfo& e = out[p->ifa_name];
        e.running = e.running || (p->ifa_flags & IFF_RUNNING);
        if (!p->ifa_addr) continue;
        char text[INET6_ADDRSTRLEN] = {};
        switch (p->ifa_addr->sa_family) {
            case AF_LINK: {
                auto* dl = reinterpret_cast<const sockaddr_dl*>(p->ifa_addr);
                if (dl->sdl_alen == 6) {
                    auto* b = reinterpret_cast<const uint8_t*>(LLADDR(dl));
                    char buf[18];
                    std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
                    e.mac = buf;
                }
                if (p->ifa_data) e.baud = static_cast<const if_data*>(p->ifa_data)->ifi_baudrate;
                break;
            }
            case AF_INET: {
                auto* a = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
                inet_ntop(AF_INET, &a->sin_addr, text, sizeof text);
                int bits = 32;
                if (p->ifa_netmask)
                    bits = prefix_len(reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in*>(p->ifa_netmask)->sin_addr), 4);
                e.v4.push_back(std::string(text) + "/" + std::to_string(bits));
                break;
            }
            case AF_INET6: {
                in6_addr addr = reinterpret_cast<const sockaddr_in6*>(p->ifa_addr)->sin6_addr;
                // KAME: link-local addresses carry the scope id in bytes 2-3 inside the kernel.
                if (IN6_IS_ADDR_LINKLOCAL(&addr) || IN6_IS_ADDR_MC_LINKLOCAL(&addr)) addr.s6_addr[2] = addr.s6_addr[3] = 0;
                inet_ntop(AF_INET6, &addr, text, sizeof text);
                int bits = 128;
                if (p->ifa_netmask)
                    bits = prefix_len(reinterpret_cast<const sockaddr_in6*>(p->ifa_netmask)->sin6_addr.s6_addr, 16);
                e.v6.push_back(std::string(text) + "/" + std::to_string(bits));
                break;
            }
            default: break;
        }
    }
    freeifaddrs(list);
    return out;
}

struct Service {
    std::string name;
    std::string bsd;
    std::string if_type;
    bool enabled = false;
    bool hidden = false;  // HiddenConfiguration: not shown in System Settings (USB device-mode NCM ports)
};

struct ServiceState {
    std::string iface;
    bool v4 = false, v6 = false;
    std::vector<std::string> routers4, routers6, dns;
};

LinkType link_type(const std::string& sc_type, const std::string& bsd) {
    if (sc_type == to_utf8(kSCNetworkInterfaceTypeIEEE80211)) return LinkType::WiFi;
    if (sc_type == to_utf8(kSCNetworkInterfaceTypeEthernet)) return LinkType::Ethernet;
    // "Bridge" and "VPN" are the values of kSCNetworkInterfaceTypeBridge / VPN,
    // which are declared only in SystemConfiguration's private headers.
    if (sc_type == "Bridge") return LinkType::Bridge;
    if (sc_type == to_utf8(kSCNetworkInterfaceTypeBond) || sc_type == to_utf8(kSCNetworkInterfaceTypeVLAN))
        return LinkType::Virtual;
    if (sc_type == to_utf8(kSCNetworkInterfaceTypeWWAN)) return LinkType::Cellular;
    if (sc_type == "VPN" || sc_type == to_utf8(kSCNetworkInterfaceTypeIPSec) ||
        sc_type == to_utf8(kSCNetworkInterfaceTypeL2TP) || sc_type == to_utf8(kSCNetworkInterfaceTypePPP))
        return LinkType::Vpn;
    if (bsd.rfind("utun", 0) == 0 || bsd.rfind("ipsec", 0) == 0 || bsd.rfind("ppp", 0) == 0) return LinkType::Vpn;
    if (bsd.rfind("bridge", 0) == 0) return LinkType::Bridge;
    if (bsd.rfind("gif", 0) == 0 || bsd.rfind("stf", 0) == 0) return LinkType::Tunnel;
    return LinkType::Other;
}

std::map<std::string, Service> read_services() {
    std::map<std::string, Service> out;
    CFRef<SCPreferencesRef> prefs(SCPreferencesCreate(nullptr, CFSTR("brosys"), nullptr));
    if (!prefs) return out;
    // The services of the current set (Location), as System Settings and
    // networksetup show them; the other sets' services are inert.
    CFRef<SCNetworkSetRef> set(SCNetworkSetCopyCurrent(prefs.get()));
    CFRef<CFArrayRef> all(set ? SCNetworkSetCopyServices(set.get()) : SCNetworkServiceCopyAll(prefs.get()));
    for (CFIndex i = 0, n = all ? CFArrayGetCount(all.get()) : 0; i < n; ++i) {
        auto s = static_cast<SCNetworkServiceRef>(const_cast<void*>(CFArrayGetValueAtIndex(all.get(), i)));
        Service svc;
        svc.name = to_utf8(SCNetworkServiceGetName(s));
        svc.enabled = SCNetworkServiceGetEnabled(s);
        CFRef<CFStringRef> sid_path(CFStringCreateWithFormat(nullptr, nullptr, CFSTR("/NetworkServices/%@/Interface"),
                                                             SCNetworkServiceGetServiceID(s)));
        if (auto itf = static_cast<CFDictionaryRef>(SCPreferencesPathGetValue(prefs.get(), sid_path.get())))
            svc.hidden = dict_bool(itf, CFSTR("HiddenConfiguration")).value_or(false);
        if (SCNetworkInterfaceRef itf = SCNetworkServiceGetInterface(s)) {
            svc.bsd = to_utf8(SCNetworkInterfaceGetBSDName(itf));
            svc.if_type = to_utf8(SCNetworkInterfaceGetInterfaceType(itf));
        }
        out[to_utf8(SCNetworkServiceGetServiceID(s))] = std::move(svc);
    }
    return out;
}

// "State:/Network/Service/<sid>/IPv4" -> sid, "IPv4".
bool split_service_key(const std::string& key, std::string& sid, std::string& leaf) {
    static const std::string prefix = "State:/Network/Service/";
    if (key.rfind(prefix, 0) != 0) return false;
    size_t slash = key.find('/', prefix.size());
    if (slash == std::string::npos) return false;
    sid = key.substr(prefix.size(), slash - prefix.size());
    leaf = key.substr(slash + 1);
    return true;
}

void split_dns(const std::vector<std::string>& servers, IpConfig& v4, IpConfig& v6) {
    for (const auto& s : servers) (s.find(':') == std::string::npos ? v4 : v6).dns.push_back(s);
}

}  // namespace

std::vector<std::string> network_watch_patterns() {
    return {"State:/Network/Global/(IPv4|IPv6|DNS)", "State:/Network/Interface/[^/]+/(Link|IPv4|IPv6|AirPort)",
            "State:/Network/Service/[^/]+/(IPv4|IPv6|DNS)", "Setup:/Network/Service/.*", "Setup:/Network/Global/IPv4"};
}

NetworkState read_network_state(const std::vector<WifiInterfaceInfo>& wifi, Connectivity connectivity) {
    NetworkState st;
    st.connectivity = connectivity;
    st.networking_enabled = true;
    st.wifi_hardware_enabled = !wifi.empty();  // Macs have no radio kill switch
    for (const auto& w : wifi) st.wifi_enabled = st.wifi_enabled || w.power_on;

    CFRef<SCDynamicStoreRef> store(SCDynamicStoreCreate(nullptr, CFSTR("brosys"), nullptr, nullptr));
    CFRef<CFDictionaryRef> values;
    if (store) {
        const void* pats[] = {CFSTR("State:/Network/Global/(IPv4|IPv6|DNS)"),
                              CFSTR("State:/Network/Service/[^/]+/(IPv4|IPv6|DNS)"),
                              CFSTR("State:/Network/Interface/[^/]+/Link")};
        CFRef<CFArrayRef> patterns(CFArrayCreate(nullptr, pats, 3, &kCFTypeArrayCallBacks));
        values = CFRef<CFDictionaryRef>(SCDynamicStoreCopyMultiple(store.get(), nullptr, patterns.get()));
    }
    auto value = [&](const std::string& key) -> CFDictionaryRef {
        if (!values) return nullptr;
        auto k = make_string(key);
        CFTypeRef v = CFDictionaryGetValue(values.get(), k.get());
        return v && CFGetTypeID(v) == CFDictionaryGetTypeID() ? static_cast<CFDictionaryRef>(v) : nullptr;
    };

    CFDictionaryRef g4 = value("State:/Network/Global/IPv4");
    CFDictionaryRef g6 = value("State:/Network/Global/IPv6");
    const std::string primary_svc4 = dict_string(g4, CFSTR("PrimaryService")).value_or("");
    const std::string primary_svc6 = dict_string(g6, CFSTR("PrimaryService")).value_or("");
    st.primary_device = dict_string(g4, CFSTR("PrimaryInterface")).value_or("");
    if (st.primary_device.empty()) st.primary_device = dict_string(g6, CFSTR("PrimaryInterface")).value_or("");
    const auto global_dns = string_array(dict_array(value("State:/Network/Global/DNS"), CFSTR("ServerAddresses")));

    // Active service states.
    std::map<std::string, ServiceState> states;
    if (values) {
        CFIndex n = CFDictionaryGetCount(values.get());
        std::vector<const void*> keys(static_cast<size_t>(n)), vals(static_cast<size_t>(n));
        CFDictionaryGetKeysAndValues(values.get(), keys.data(), vals.data());
        for (CFIndex i = 0; i < n; ++i) {
            std::string sid, leaf;
            if (!split_service_key(to_utf8(keys[size_t(i)]), sid, leaf)) continue;
            auto d = static_cast<CFDictionaryRef>(vals[size_t(i)]);
            ServiceState& s = states[sid];
            if (leaf == "DNS") {
                s.dns = string_array(dict_array(d, CFSTR("ServerAddresses")));
                continue;
            }
            const bool has = !string_array(dict_array(d, CFSTR("Addresses"))).empty();
            if (auto itf = dict_string(d, CFSTR("InterfaceName"))) s.iface = *itf;
            auto router = dict_string(d, CFSTR("Router"));
            if (leaf == "IPv4") {
                s.v4 = has;
                if (router) s.routers4.push_back(*router);
            } else if (leaf == "IPv6") {
                s.v6 = has;
                if (router) s.routers6.push_back(*router);
            }
        }
    }

    const auto services = read_services();
    const auto ifs = read_ifaddrs();
    std::map<std::string, const WifiInterfaceInfo*> wifi_by_name;
    for (const auto& w : wifi) wifi_by_name[w.name] = &w;

    // Device list: hardware ports, then interfaces of active services.
    struct Port {
        std::string bsd, type, display, hw_mac;
    };
    std::vector<Port> ports;
    std::set<std::string> listed;
    CFRef<CFArrayRef> hw(SCNetworkInterfaceCopyAll());
    for (CFIndex i = 0, n = hw ? CFArrayGetCount(hw.get()) : 0; i < n; ++i) {
        auto itf = static_cast<SCNetworkInterfaceRef>(const_cast<void*>(CFArrayGetValueAtIndex(hw.get(), i)));
        Port p{to_utf8(SCNetworkInterfaceGetBSDName(itf)), to_utf8(SCNetworkInterfaceGetInterfaceType(itf)),
               to_utf8(SCNetworkInterfaceGetLocalizedDisplayName(itf)),
               to_utf8(SCNetworkInterfaceGetHardwareAddressString(itf))};
        if (p.bsd.empty() || !listed.insert(p.bsd).second) continue;
        ports.push_back(std::move(p));
    }
    for (const auto& [sid, s] : states) {
        if ((s.v4 || s.v6) && !s.iface.empty() && s.iface != "lo0" && listed.insert(s.iface).second) {
            auto svc = services.find(sid);
            ports.push_back(Port{s.iface, svc != services.end() ? svc->second.if_type : std::string(),
                                 svc != services.end() ? svc->second.name : s.iface, {}});
        }
    }

    for (const auto& p : ports) {
        NetDevice d;
        d.id = d.interface_name = p.bsd;
        d.description = p.display.empty() ? p.bsd : p.display;
        d.type = link_type(p.type, p.bsd);
        auto fi = ifs.find(p.bsd);
        const IfInfo* info = fi != ifs.end() ? &fi->second : nullptr;
        d.mac = info && !info->mac.empty() ? info->mac : normalize_mac(p.hw_mac);
        if (info) {
            d.ipv4.addresses = info->v4;
            d.ipv6.addresses = info->v6;
            d.speed_mbps = info->baud / 1000000;
        }
        auto wi = wifi_by_name.find(p.bsd);
        const WifiInterfaceInfo* w = wi != wifi_by_name.end() ? wi->second : nullptr;
        if (w && w->tx_rate_mbps > 0) d.speed_mbps = static_cast<uint64_t>(w->tx_rate_mbps);

        const std::string* sid = nullptr;
        for (const auto& [id, s] : states)
            if (s.iface == p.bsd && (s.v4 || s.v6)) sid = &id;
        bool link = info && info->running;
        if (CFDictionaryRef l = value("State:/Network/Interface/" + p.bsd + "/Link"))
            link = dict_bool(l, CFSTR("Active")).value_or(link);

        if (w && !w->power_on) d.state = LinkState::Unavailable;
        else if (sid) d.state = LinkState::Connected;
        else if (!link) d.state = LinkState::Unavailable;
        else d.state = LinkState::Disconnected;

        // Managed = configured in System Settings (an enabled network service);
        // utuns of NetworkExtension VPNs / iCloud Private Relay are not.
        d.managed = false;
        for (const auto& [id, s] : services)
            if (s.bsd == p.bsd && s.enabled && !s.hidden) d.managed = true;
        d.is_primary = !st.primary_device.empty() && p.bsd == st.primary_device;
        if (sid) {
            const ServiceState& s = states.at(*sid);
            auto svc = services.find(*sid);
            d.connection = w && !w->ssid.empty() ? w->ssid : svc != services.end() ? svc->second.name : p.bsd;
            d.ipv4.gateways = s.routers4;
            d.ipv6.gateways = s.routers6;
            split_dns(s.dns.empty() && d.is_primary ? global_dns : s.dns, d.ipv4, d.ipv6);

            ActiveConnection ac;
            ac.id = ac.uuid = *sid;
            ac.name = d.connection;
            ac.type = d.type;
            ac.state = LinkState::Connected;
            ac.device_ids = {p.bsd};
            ac.default4 = *sid == primary_svc4;
            ac.default6 = *sid == primary_svc6;
            st.active_connections.push_back(std::move(ac));
        }
        st.devices.push_back(std::move(d));
    }
    return st;
}

}  // namespace brosys::mac
