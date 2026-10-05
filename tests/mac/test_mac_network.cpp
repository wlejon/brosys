// macOS network against the OS's own answers: `scutil --nwi` (the primary
// interface and its address), scutil's State:/Network/Global/IPv4 and DNS
// (router, primary service, DNS servers), `networksetup
// -listallhardwareports` (ports, BSD names, MAC addresses), ifconfig (each
// device's addresses), `networksetup -getairportpower` (Wi-Fi radio) and
// `scutil -r` (reachability). Read-only: nothing is configured; a Wi-Fi scan
// is requested only while the radio is already on.
#include "check.h"
#include "mac/support/oracle.h"

#include "brosys/network.h"

#include <cstdlib>
#include <set>

using namespace brosys;
using namespace std::chrono_literals;
namespace om = bstest::mac;

namespace {

const NetDevice* find(const NetworkState& s, const std::string& id) {
    for (auto& d : s.devices)
        if (d.id == id) return &d;
    return nullptr;
}

bool has(const std::vector<std::string>& v, const std::string& x) {
    for (auto& e : v)
        if (e == x) return true;
    return false;
}

std::string scutil_show(const std::string& key) { return om::run("echo 'show " + key + "' | scutil").out; }

// "  ServerAddresses : <array> {\n    0 : 10.1.0.6\n  }" -> {"10.1.0.6"}
std::vector<std::string> scutil_array(const std::string& dict, const std::string& key) {
    std::vector<std::string> out;
    size_t at = dict.find(key + " : <array> {");
    if (at == std::string::npos) return out;
    size_t end = dict.find('}', at);
    for (const auto& l : om::lines(dict.substr(at, end - at))) {
        size_t c = l.find(" : ");
        if (c != std::string::npos && l.find("<array>") == std::string::npos) out.push_back(om::trim(l.substr(c + 3)));
    }
    return out;
}

int prefix_of_mask(unsigned long mask) {
    int n = 0;
    while (mask & 0x80000000ul) {
        ++n;
        mask = (mask << 1) & 0xfffffffful;
    }
    return n;
}

void print(const NetworkState& s) {
    std::printf("connectivity=%s wifi=%d/%d primary=%s\n", to_string(s.connectivity), s.wifi_enabled,
                s.wifi_hardware_enabled, s.primary_device.c_str());
    for (auto& d : s.devices) {
        std::printf("  %s '%s' %s %s mac=%s conn='%s' managed=%d primary=%d\n", d.id.c_str(), d.description.c_str(),
                    to_string(d.type), to_string(d.state), d.mac.c_str(), d.connection.c_str(), d.managed, d.is_primary);
        for (auto& a : d.ipv4.addresses) std::printf("    v4 %s\n", a.c_str());
        for (auto& a : d.ipv6.addresses) std::printf("    v6 %s\n", a.c_str());
        for (auto& g : d.ipv4.gateways) std::printf("    gw4 %s\n", g.c_str());
        for (auto& n : d.ipv4.dns) std::printf("    dns %s\n", n.c_str());
    }
    for (auto& c : s.active_connections)
        std::printf("  conn %s '%s' %s %s d4=%d d6=%d devs=%zu\n", c.id.c_str(), c.name.c_str(), to_string(c.type),
                    to_string(c.state), c.default4, c.default6, c.device_ids.size());
}

void test_state(NetworkService& svc) {
    bool first = false;
    for (auto& e : svc.events().drain()) first |= std::holds_alternative<NetworkChanged>(e);
    CHECK(first);  // queued before create() returned
    NetworkState s = svc.state();
    print(s);

    // The hardware ports.
    std::string ports = om::run("networksetup -listallhardwareports").out;
    std::string port;
    std::string dev;
    size_t nports = 0;
    for (const auto& l : om::lines(ports)) {
        if (auto p = om::after(l, "Hardware Port: ")) port = *p;
        else if (auto d = om::after(l, "Device: ")) dev = *d;
        else if (auto m = om::after(l, "Ethernet Address: ")) {
            ++nports;
            const NetDevice* nd = find(s, dev);
            CHECK(nd != nullptr);
            if (!nd) {
                std::printf("  missing port %s (%s)\n", dev.c_str(), port.c_str());
                continue;
            }
            CHECK_EQ(nd->interface_name, dev);
            CHECK_EQ(nd->description, port);
            // The live address (ifconfig's ether), which differs from the
            // port's burned-in one under Wi-Fi's private-address rotation.
            std::string live = om::after(om::run("ifconfig " + dev).out, "\tether ").value_or(*m);
            if (live != "N/A") CHECK_EQ(nd->mac, live);
            if (port == "Wi-Fi") CHECK(nd->type == LinkType::WiFi);
            if (port == "Thunderbolt Bridge") CHECK(nd->type == LinkType::Bridge);
        }
    }
    CHECK(nports > 0);

    // The primary: scutil --nwi lists the IPv4 interfaces in service order.
    std::string nwi = om::run("scutil --nwi").out;
    std::string g4 = scutil_show("State:/Network/Global/IPv4");
    std::string primary = om::after(g4, "PrimaryInterface : ").value_or("");
    CHECK_EQ(s.primary_device, primary);
    if (!primary.empty()) {
        CHECK(om::contains(nwi, primary + " : flags"));
        const NetDevice* p = s.primary();
        REQUIRE(p != nullptr);
        CHECK(p->is_primary);
        CHECK(p->state == LinkState::Connected);
        if (auto router = om::after(g4, "Router : ")) CHECK(has(p->ipv4.gateways, *router));
        // DNS of the primary = the resolver's global servers.
        for (const auto& ns : scutil_array(scutil_show("State:/Network/Global/DNS"), "ServerAddresses"))
            CHECK(has(p->ipv4.dns, ns) || has(p->ipv6.dns, ns));
        std::string service = om::after(g4, "PrimaryService : ").value_or("");
        bool found = false;
        for (auto& c : s.active_connections)
            if (c.id == service) {
                found = true;
                CHECK(c.default4);
                CHECK(has(c.device_ids, primary));
                CHECK(c.state == LinkState::Connected);
            }
        CHECK(found);
    }
    size_t primaries = 0;
    for (auto& d : s.devices) primaries += d.is_primary;
    CHECK_EQ(primaries, size_t(primary.empty() ? 0 : 1));

    // Each device's addresses = ifconfig's.
    for (auto& d : s.devices) {
        std::set<std::string> v4, v6;
        for (const auto& l : om::lines(om::run("ifconfig " + d.interface_name).out)) {
            std::string t = om::trim(l);
            if (om::starts_with(t, "inet ")) {
                // inet 10.1.0.8 netmask 0xffffff00 broadcast 10.1.0.255
                std::string addr = t.substr(5, t.find(' ', 5) - 5);
                auto mask = om::after(t, "netmask ").value_or("0");
                v4.insert(addr + "/" + std::to_string(prefix_of_mask(std::strtoul(mask.c_str(), nullptr, 16))));
            } else if (om::starts_with(t, "inet6 ")) {
                // inet6 fe80::89d:1f68:fe0f:aa09%en7 prefixlen 64 secured scopeid 0xd
                std::string addr = t.substr(6, t.find(' ', 6) - 6);
                addr = addr.substr(0, addr.find('%'));
                std::string len = om::after(t, "prefixlen ").value_or("");
                v6.insert(addr + "/" + len.substr(0, len.find(' ')));
            }
        }
        std::set<std::string> got4(d.ipv4.addresses.begin(), d.ipv4.addresses.end());
        std::set<std::string> got6(d.ipv6.addresses.begin(), d.ipv6.addresses.end());
        CHECK(got4 == v4);
        CHECK(got6 == v6);
        if (got4 != v4 || got6 != v6) std::printf("  address mismatch on %s\n", d.id.c_str());
    }

    // Connectivity: Network.framework's default path vs SystemConfiguration's
    // reachability of the internet as a whole.
    std::string reach = om::trim(om::run("scutil -r 0.0.0.0").out);
    std::printf("scutil -r 0.0.0.0: %s\n", reach.c_str());
    if (reach == "Reachable") CHECK(s.connectivity == Connectivity::Full);
    if (reach == "Not Reachable") CHECK(s.connectivity == Connectivity::None);

    for (auto& c : s.active_connections) {
        CHECK(!c.name.empty());
        for (auto& id : c.device_ids) CHECK(find(s, id) != nullptr);
    }
    // Managed = an enabled service in System Settings' current set:
    //   (1) USB 10/100/1000 LAN            ((*) when disabled)
    //   (Hardware Port: USB 10/100/1000 LAN, Device: en7)
    std::set<std::string> enabled;
    bool disabled = false;
    for (const auto& l : om::lines(om::run("networksetup -listnetworkserviceorder").out)) {
        if (om::starts_with(l, "(Hardware Port:")) {
            auto dev = om::after(l, "Device: ").value_or("");
            if (!dev.empty() && dev.back() == ')') dev.pop_back();
            if (!disabled && !dev.empty()) enabled.insert(dev);
        } else if (om::starts_with(l, "(")) {
            disabled = om::starts_with(l, "(*)");
        }
    }
    for (auto& d : s.devices) {
        CHECK_EQ(d.managed, enabled.count(d.interface_name) == 1);
        if (d.managed != (enabled.count(d.interface_name) == 1)) std::printf("  managed? %s\n", d.id.c_str());
    }
}

void test_wifi(NetworkService& svc) {
    NetworkState s = svc.state();
    std::vector<std::string> wifi;
    for (auto& d : s.devices)
        if (d.type == LinkType::WiFi) wifi.push_back(d.id);
    CHECK_EQ(s.wifi_hardware_enabled, !wifi.empty());
    if (wifi.empty()) {
        std::printf("no Wi-Fi device\n");
        CHECK(!svc.request_wifi_scan("").ok);
        return;
    }
    bool any_on = false;
    for (auto& w : wifi) {
        std::string power = om::run("networksetup -getairportpower " + w).out;
        bool on = om::contains(power, ": On");
        std::printf("%s", power.c_str());
        any_on |= on;
        const NetDevice* d = find(s, w);
        if (!on) CHECK(d->state == LinkState::Unavailable);
    }
    CHECK_EQ(s.wifi_enabled, any_on);
    for (auto& ap : svc.access_points("")) {
        CHECK(has(wifi, ap.device_id));
        CHECK(ap.strength_percent <= 100);
    }

    if (!any_on) {
        // The radio is off and stays off: a scan request is refused, with the reason.
        Result r = svc.request_wifi_scan("");
        CHECK(!r.ok);
        CHECK(om::contains(r.error, "powered off"));
        std::printf("scan with the radio off: %s\n", r.error.c_str());
    } else {
        // Scanning changes nothing; completion (ok or not) is reported once per device.
        svc.events().drain();
        Result r = svc.request_wifi_scan(wifi[0]);
        CHECK(r.ok);
        bool done = bstest::wait_until(
            [&] {
                for (auto& e : svc.events().drain())
                    if (auto* c = std::get_if<WifiScanCompleted>(&e); c && c->device_id == wifi[0]) {
                        std::printf("scan: ok=%d '%s' %zu AP(s)\n", c->ok, c->error.c_str(), c->access_points.size());
                        return true;
                    }
                return false;
            },
            20s);
        CHECK(done);
    }
    CHECK(!svc.request_wifi_scan("en-no-such").ok);
}

}  // namespace

int main() {
    std::string err;
    auto svc = NetworkService::create({}, &err);
    if (!svc) bstest::skip("test_mac_network", "NetworkService::create: " + err);
    test_state(*svc);
    test_wifi(*svc);
    svc.reset();
    for (int i = 0; i < 5; ++i) CHECK(NetworkService::create({}, &err) != nullptr);
    return bstest::finish("test_mac_network");
}
