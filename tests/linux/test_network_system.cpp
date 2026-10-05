// NetworkService against the real NetworkManager, read-only: every device,
// IP configuration, active connection and the global flags must match
// `nmcli -t` (an independent libnm client), and the primary device must be
// the one carrying the kernel's preferred default route. Nothing is changed.
#include "brosys/network.h"
#include "check.h"
#include "linux/support/proc.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace {

constexpr const char* kName = "test_network_system";

// One `nmcli -t` line: fields split at unescaped ':', "\:" unescaped.
std::vector<std::string> split_terse(const std::string& line) {
    std::vector<std::string> out(1);
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            out.back() += line[++i];
        } else if (line[i] == ':') {
            out.emplace_back();
        } else {
            out.back() += line[i];
        }
    }
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

using Block = std::multimap<std::string, std::string>;  // nmcli field -> value(s)

std::vector<Block> device_blocks() {
    auto r = bstest::run({"nmcli", "-t", "-f",
                          "GENERAL.DEVICE,GENERAL.DBUS-PATH,GENERAL.HWADDR,GENERAL.STATE,GENERAL.CONNECTION,"
                          "GENERAL.NM-MANAGED,GENERAL.DRIVER,CAPABILITIES.SPEED,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS,"
                          "IP6.ADDRESS,IP6.GATEWAY,IP6.DNS",
                          "device", "show"});
    std::vector<Block> blocks(1);
    std::istringstream in(r.out);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            if (!blocks.back().empty()) blocks.emplace_back();
            continue;
        }
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        // The value is the rest of the line; MAC / IPv6 colons come escaped.
        std::string value = line.substr(colon + 1);
        std::string unescaped;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size()) ++i;
            unescaped += value[i];
        }
        auto bracket = key.find('[');
        if (bracket != std::string::npos) key = key.substr(0, bracket);
        blocks.back().emplace(key, unescaped);
    }
    if (blocks.back().empty()) blocks.pop_back();
    return blocks;
}

std::string one(const Block& b, const std::string& key) {
    auto it = b.find(key);
    return it == b.end() ? std::string() : it->second;
}

std::vector<std::string> all(const Block& b, const std::string& key) {
    std::vector<std::string> out;
    auto [lo, hi] = b.equal_range(key);
    for (auto it = lo; it != hi; ++it)
        if (!it->second.empty()) out.push_back(it->second);
    return out;
}

void check_devices(const brosys::NetworkState& s) {
    auto blocks = device_blocks();
    CHECK_EQ(blocks.size(), s.devices.size());
    for (auto& b : blocks) {
        std::string path = one(b, "GENERAL.DBUS-PATH");
        const brosys::NetDevice* d = nullptr;
        for (auto& x : s.devices)
            if (x.id == path) d = &x;
        CHECK(d != nullptr);
        if (!d) {
            std::fprintf(stderr, "  missing device %s\n", path.c_str());
            continue;
        }
        std::printf("device %-10s %-9s %-12s mac=%s conn='%s' speed=%llu v4=%zu v6=%zu%s\n", d->interface_name.c_str(),
                    brosys::to_string(d->type), brosys::to_string(d->state), d->mac.c_str(), d->connection.c_str(),
                    static_cast<unsigned long long>(d->speed_mbps), d->ipv4.addresses.size(), d->ipv6.addresses.size(),
                    d->is_primary ? " primary" : "");
        CHECK_EQ(d->interface_name, one(b, "GENERAL.DEVICE"));
        std::string mac = lower(one(b, "GENERAL.HWADDR"));
        if (mac == "00:00:00:00:00:00") mac.clear();
        CHECK_EQ(d->mac, mac);
        CHECK_EQ(d->description, one(b, "GENERAL.DRIVER"));
        CHECK_EQ(d->connection, one(b, "GENERAL.CONNECTION"));
        CHECK_EQ(d->managed, one(b, "GENERAL.NM-MANAGED") == "yes");
        int code = std::atoi(one(b, "GENERAL.STATE").c_str());  // "100 (connected)"
        brosys::LinkState want = code == 100 ? brosys::LinkState::Connected
                               : code == 30 || code == 120 ? brosys::LinkState::Disconnected
                               : code == 10 || code == 20 ? brosys::LinkState::Unavailable
                               : code == 110 ? brosys::LinkState::Disconnecting
                               : code >= 40 && code <= 90 ? brosys::LinkState::Connecting
                                                          : brosys::LinkState::Unknown;
        CHECK(d->state == want);
        std::string speed = one(b, "CAPABILITIES.SPEED");  // "1000 Mb/s" / "unknown"
        if (speed.find("Mb/s") != std::string::npos)
            CHECK_EQ(d->speed_mbps, static_cast<uint64_t>(std::atoll(speed.c_str())));
        CHECK(d->ipv4.addresses == all(b, "IP4.ADDRESS"));
        CHECK(d->ipv4.gateways == all(b, "IP4.GATEWAY"));
        CHECK(d->ipv4.dns == all(b, "IP4.DNS"));
        CHECK(d->ipv6.addresses == all(b, "IP6.ADDRESS"));
        CHECK(d->ipv6.gateways == all(b, "IP6.GATEWAY"));
        CHECK(d->ipv6.dns == all(b, "IP6.DNS"));
    }
}

brosys::LinkType nmcli_type(const std::string& t) {
    if (t == "ethernet" || t == "802-3-ethernet") return brosys::LinkType::Ethernet;
    if (t == "wifi" || t == "802-11-wireless") return brosys::LinkType::WiFi;
    if (t == "loopback") return brosys::LinkType::Loopback;
    if (t == "bridge") return brosys::LinkType::Bridge;
    if (t == "vpn" || t == "wireguard") return brosys::LinkType::Vpn;
    if (t == "gsm" || t == "cdma") return brosys::LinkType::Cellular;
    if (t == "tun" || t == "ip-tunnel" || t == "vxlan") return brosys::LinkType::Tunnel;
    return brosys::LinkType::Other;
}

void check_connections(const brosys::NetworkState& s) {
    auto r = bstest::run({"nmcli", "-t", "-f", "NAME,UUID,TYPE,STATE,DEVICE,ACTIVE-PATH", "connection", "show",
                          "--active"});
    std::istringstream in(r.out);
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
        auto f = split_terse(line);
        if (f.size() < 6) continue;
        ++n;
        const brosys::ActiveConnection* c = nullptr;
        for (auto& x : s.active_connections)
            if (x.id == f[5]) c = &x;
        CHECK(c != nullptr);
        if (!c) continue;
        std::printf("connection '%s' %s %s default4=%d default6=%d\n", c->name.c_str(), brosys::to_string(c->type),
                    brosys::to_string(c->state), c->default4, c->default6);
        CHECK_EQ(c->name, f[0]);
        CHECK_EQ(c->uuid, f[1]);
        if (nmcli_type(f[2]) != brosys::LinkType::Other) CHECK(c->type == nmcli_type(f[2]));
        CHECK(c->state == (f[3] == "activated"     ? brosys::LinkState::Connected
                           : f[3] == "activating" ? brosys::LinkState::Connecting
                           : f[3] == "deactivating" ? brosys::LinkState::Disconnecting
                                                    : brosys::LinkState::Disconnected));
        // The connection's device(s), by interface name.
        std::vector<std::string> ifaces;
        for (auto& id : c->device_ids)
            for (auto& d : s.devices)
                if (d.id == id) ifaces.push_back(d.interface_name);
        CHECK(std::find(ifaces.begin(), ifaces.end(), f[4]) != ifaces.end());
        auto g = bstest::run({"nmcli", "-t", "-f", "GENERAL.DEFAULT,GENERAL.DEFAULT6", "connection", "show", c->uuid});
        CHECK_EQ(c->default4, g.out.find("GENERAL.DEFAULT:yes") != std::string::npos);
        CHECK_EQ(c->default6, g.out.find("GENERAL.DEFAULT6:yes") != std::string::npos);
    }
    CHECK_EQ(n, s.active_connections.size());
}

void check_general(const brosys::NetworkState& s) {
    auto r = bstest::run({"nmcli", "-t", "-f", "CONNECTIVITY", "general"});
    std::string conn = r.out.substr(0, r.out.find('\n'));
    CHECK_EQ(std::string(brosys::to_string(s.connectivity)), conn);
    r = bstest::run({"nmcli", "networking"});
    CHECK_EQ(s.networking_enabled, r.out.rfind("enabled", 0) == 0);
    r = bstest::run({"nmcli", "radio", "wifi"});
    CHECK_EQ(s.wifi_enabled, r.out.rfind("enabled", 0) == 0);
    std::printf("connectivity %s, networking %d, wifi %d (hw %d)\n", brosys::to_string(s.connectivity),
                s.networking_enabled, s.wifi_enabled, s.wifi_hardware_enabled);

    // The primary device carries the kernel's preferred default route.
    if (bstest::have_program("ip")) {
        auto route = bstest::run({"ip", "-o", "route", "show", "default"});
        std::istringstream rin(route.out);
        std::string line, dev;
        int best = -1;
        while (std::getline(rin, line)) {
            std::istringstream words(line);
            std::string w, d;
            int metric = 0;
            while (words >> w) {
                if (w == "dev") words >> d;
                if (w == "metric") words >> metric;
            }
            if (!d.empty() && (best < 0 || metric < best)) {
                best = metric;
                dev = d;
            }
        }
        auto* p = s.primary();
        // NetworkManager can only name a primary among devices it manages. A machine whose
        // default route is configured by something else (systemd-networkd, netplan on a
        // server or CI runner) has NM running with that device unmanaged: there the kernel's
        // route says nothing about NM's primary, and the backend must not invent one.
        const brosys::NetDevice* route_dev = nullptr;
        for (auto& d : s.devices)
            if (d.interface_name == dev) route_dev = &d;
        if (!dev.empty() && (!route_dev || !route_dev->managed || route_dev->connection.empty())) {
            std::printf("note: default route via %s, which has no NetworkManager connection; primary cross-check skipped\n",
                        dev.c_str());
            int primaries = 0;
            for (auto& d : s.devices) primaries += d.is_primary;
            CHECK(primaries <= 1);
        } else if (!dev.empty()) {
            CHECK(p != nullptr);
            if (p) CHECK_EQ(p->interface_name, dev);
            int primaries = 0;
            for (auto& d : s.devices) primaries += d.is_primary;
            CHECK_EQ(primaries, 1);
        }
    }
}

}  // namespace

int main() {
    if (!bstest::have_program("nmcli")) bstest::skip(kName, "nmcli not installed");
    std::string err;
    auto net = brosys::NetworkService::create(brosys::NetworkConfig(), &err);
    if (!net) bstest::skip(kName, "NetworkManager unavailable: " + err);
    auto first = net->events().drain();
    CHECK_EQ(first.size(), size_t(1));
    auto s = net->state();
    check_general(s);
    check_devices(s);
    check_connections(s);

    bool wifi = false;
    for (auto& d : s.devices) wifi |= d.type == brosys::LinkType::WiFi;
    if (!wifi) {
        CHECK(net->access_points("").empty());
        auto r = net->request_wifi_scan("");
        CHECK(!r.ok);
        std::printf("no Wi-Fi devices: request_wifi_scan -> '%s'\n", r.error.c_str());
    }
    return bstest::finish(kName);
}
