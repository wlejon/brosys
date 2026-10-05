// Windows network against the OS's own answers: WMI (MSFT_NetIPAddress,
// MSFT_NetRoute, MSFT_NetIPInterface, MSFT_NetAdapter) for addresses, MACs
// and the default route the stack picks, GetBestInterfaceEx for a public
// destination, NLM for connectivity, `netsh wlan` for Wi-Fi scans when a
// WLAN service is present. Read-only: nothing is (re)configured.
#include "check.h"
#include "win/support/oracle.h"

#include "brosys/network.h"
#include "win/wifi.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netlistmgr.h>

#include <map>
#include <set>

using namespace brosys;
using namespace std::chrono_literals;
using bstest::win::WmiRow;

namespace {

ULONG if_index(const std::string& luid_text) {
    NET_LUID luid{};
    luid.Value = std::stoull(luid_text);
    NET_IFINDEX idx = 0;
    ConvertInterfaceLuidToIndex(&luid, &idx);
    return idx;
}

std::vector<WmiRow> wmi(const wchar_t* q) {
    std::vector<WmiRow> rows;
    std::string err;
    if (!bstest::win::wmi_query(L"ROOT\\StandardCimv2", q, rows, &err)) std::printf("note: %s\n", err.c_str());
    return rows;
}

void test_devices(const NetworkState& s) {
    std::printf("network: connectivity=%s primary=%s devices=%zu active=%zu\n", to_string(s.connectivity),
                s.primary_device.c_str(), s.devices.size(), s.active_connections.size());
    for (auto& d : s.devices)
        std::printf("  %s %-9s %-12s %-20s %s v4=%zu gw=%zu conn='%s'\n", d.is_primary ? "*" : " ", to_string(d.type),
                    to_string(d.state), d.interface_name.c_str(), d.mac.c_str(), d.ipv4.addresses.size(),
                    d.ipv4.gateways.size(), d.connection.c_str());
    CHECK(!s.devices.empty());

    // Addresses: every IPv4 address WMI reports for an interface we list.
    auto addrs = wmi(L"SELECT InterfaceIndex, IPAddress, PrefixLength, AddressFamily FROM MSFT_NetIPAddress WHERE AddressFamily = 2");
    std::map<ULONG, std::set<std::string>> wmi_v4;
    for (auto& r : addrs)
        wmi_v4[std::stoul(r["InterfaceIndex"])].insert(r["IPAddress"] + "/" + r["PrefixLength"]);
    CHECK(!wmi_v4.empty());
    int addr_compared = 0;
    for (auto& d : s.devices) {
        ULONG idx = if_index(d.id);
        CHECK(idx != 0);
        if (d.state != LinkState::Connected || wmi_v4.find(idx) == wmi_v4.end()) continue;
        std::set<std::string> ours(d.ipv4.addresses.begin(), d.ipv4.addresses.end());
        CHECK(ours == wmi_v4[idx]);
        ++addr_compared;
    }
    // Every connected interface with IPv4 is known to WMI and vice versa.
    for (auto& [idx, set] : wmi_v4) {
        bool found = false;
        for (auto& d : s.devices) found |= if_index(d.id) == idx;
        CHECK(found);
    }
    std::printf("IPv4 address sets compared with MSFT_NetIPAddress: %d\n", addr_compared);
    CHECK(addr_compared > 0);

    // MACs of physical adapters (MSFT_NetAdapter.PermanentAddress: 12 hex digits).
    auto adapters = wmi(L"SELECT InterfaceIndex, PermanentAddress FROM MSFT_NetAdapter");
    int compared = 0;
    for (auto& r : adapters) {
        std::string hex = bstest::win::lower(r["PermanentAddress"]);
        if (hex.size() != 12) continue;
        std::string mac;
        for (size_t i = 0; i < 12; i += 2) mac += (i ? ":" : "") + hex.substr(i, 2);
        for (auto& d : s.devices)
            if (if_index(d.id) == std::stoul(r["InterfaceIndex"])) {
                CHECK_EQ(d.mac, mac);
                ++compared;
            }
    }
    std::printf("MACs compared with MSFT_NetAdapter: %d (of %zu adapters)\n", compared, adapters.size());
}

void test_primary(const NetworkState& s) {
    // The stack's choice: lowest RouteMetric + InterfaceMetric among 0.0.0.0/0
    // routes on connected interfaces (MSFT_NetRoute / MSFT_NetIPInterface).
    auto routes = wmi(L"SELECT InterfaceIndex, RouteMetric FROM MSFT_NetRoute WHERE DestinationPrefix = '0.0.0.0/0'");
    auto ifaces = wmi(L"SELECT InterfaceIndex, InterfaceMetric, ConnectionState FROM MSFT_NetIPInterface WHERE AddressFamily = 2");
    std::map<ULONG, std::pair<ULONG, bool>> metric;  // index -> (interface metric, connected)
    for (auto& r : ifaces)
        metric[std::stoul(r["InterfaceIndex"])] = {std::stoul(r["InterfaceMetric"]), r["ConnectionState"] == "1"};
    ULONG best = 0, best_metric = ULONG_MAX;
    for (auto& r : routes) {
        ULONG idx = std::stoul(r["InterfaceIndex"]);
        auto m = metric.find(idx);
        if (m == metric.end() || !m->second.second) continue;
        ULONG total = std::stoul(r["RouteMetric"]) + m->second.first;
        if (total < best_metric) {
            best_metric = total;
            best = idx;
        }
    }
    if (!best) {
        CHECK(s.primary_device.empty() || s.primary() != nullptr);
        std::printf("note: no IPv4 default route\n");
        return;
    }
    REQUIRE(s.primary());
    CHECK_EQ(if_index(s.primary_device), best);
    int primaries = 0;
    for (auto& d : s.devices) primaries += d.is_primary ? 1 : 0;
    CHECK_EQ(primaries, 1);

    // And the interface the stack would route a public destination through.
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    inet_pton(AF_INET, "1.1.1.1", &dst.sin_addr);
    DWORD out_idx = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&dst), &out_idx) == NO_ERROR) CHECK_EQ(if_index(s.primary_device), ULONG(out_idx));

    bool default4 = false;
    for (auto& c : s.active_connections)
        if (c.default4) {
            default4 = true;
            CHECK(c.device_ids.size() == 1 && c.device_ids[0] == s.primary_device);
        }
    CHECK(default4);
}

void test_connectivity(const NetworkState& s) {
    INetworkListManager* nlm = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_INetworkListManager,
                                reinterpret_cast<void**>(&nlm))))
        return;
    NLM_CONNECTIVITY c{};
    nlm->GetConnectivity(&c);
    nlm->Release();
    bool internet = (c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
    std::printf("NLM connectivity 0x%x (internet=%d)\n", c, internet);
    CHECK_EQ(s.connectivity == Connectivity::Full, internet);
    if (c == NLM_CONNECTIVITY_DISCONNECTED) CHECK(s.connectivity == Connectivity::None);
}

void test_wifi(NetworkService& svc, const NetworkState& s) {
    bool has_wifi = false;
    for (auto& d : s.devices) has_wifi |= d.type == LinkType::WiFi;
    Result r = svc.request_wifi_scan("");
    if (!has_wifi) {
        CHECK(!r.ok);
        CHECK(!r.error.empty());
        CHECK(svc.access_points("").empty());
        std::printf("no Wi-Fi device: scan refused (%s)\n", r.error.c_str());
        return;
    }
    REQUIRE(r.ok);
    std::vector<WifiScanCompleted> done;
    CHECK(bstest::wait_until([&] {
        for (auto& e : svc.events().drain())
            if (auto* w = std::get_if<WifiScanCompleted>(&e)) done.push_back(*w);
        return !done.empty();
    }, 20000ms));
    REQUIRE(!done.empty());
    CHECK(done[0].ok);
    auto tool = bstest::win::run_tool(L"netsh.exe wlan show networks mode=bssid");
    std::string out = bstest::win::lower(tool.out);
    int seen = 0;
    for (auto& ap : done[0].access_points) {
        CHECK(ap.bssid.size() == 17);
        CHECK(ap.frequency_mhz > 2000 && ap.channel > 0);
        CHECK(ap.rssi_dbm.has_value());
        if (out.find(ap.bssid) != std::string::npos) ++seen;
    }
    std::printf("scan: %zu APs, %d also listed by netsh\n", done[0].access_points.size(), seen);
    if (!done[0].access_points.empty()) CHECK(seen > 0);
}

std::vector<uint8_t> rsn(std::initializer_list<uint8_t> akm_types) {
    std::vector<uint8_t> body = {1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4};
    body.push_back(static_cast<uint8_t>(akm_types.size()));
    body.push_back(0);
    for (auto t : akm_types) body.insert(body.end(), {0x00, 0x0f, 0xac, t});
    std::vector<uint8_t> ie = {48, static_cast<uint8_t>(body.size())};
    ie.insert(ie.end(), body.begin(), body.end());
    return ie;
}

void test_ie_parsing() {
    using win::security_from_ies;
    auto sec = [](const std::vector<uint8_t>& ie, bool privacy) { return security_from_ies(ie.data(), ie.size(), privacy); };
    CHECK(sec(rsn({2}), true) == WifiSecurity::Wpa2Personal);
    CHECK(sec(rsn({2, 8}), true) == WifiSecurity::Wpa3Personal);  // transition mode: strongest offered
    CHECK(sec(rsn({1}), true) == WifiSecurity::Wpa2Enterprise);
    CHECK(sec(rsn({18}), true) == WifiSecurity::Owe);
    std::vector<uint8_t> wpa = {221, 22, 0x00, 0x50, 0xf2, 1, 1, 0, 0x00, 0x50, 0xf2, 2,
                                1, 0, 0x00, 0x50, 0xf2, 2, 1, 0, 0x00, 0x50, 0xf2, 2};
    CHECK(sec(wpa, true) == WifiSecurity::WpaPersonal);
    std::vector<uint8_t> ssid_only = {0, 3, 'a', 'b', 'c'};
    CHECK(sec(ssid_only, true) == WifiSecurity::Wep);
    CHECK(sec(ssid_only, false) == WifiSecurity::Open);
    std::vector<uint8_t> truncated = {48, 40, 1, 0};
    CHECK(sec(truncated, true) == WifiSecurity::Wep);  // malformed IE ignored, never read past the end
    CHECK_EQ(win::channel_from_mhz(2412), 1u);
    CHECK_EQ(win::channel_from_mhz(2484), 14u);
    CHECK_EQ(win::channel_from_mhz(5180), 36u);
    CHECK_EQ(win::channel_from_mhz(5955), 1u);  // 6 GHz channel 1
}

}  // namespace

int main() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    test_ie_parsing();
    std::string err;
    auto svc = NetworkService::create(NetworkConfig{}, &err);
    if (!svc) {
        std::printf("create failed: %s\n", err.c_str());
        return 1;
    }
    bool initial = false;
    for (auto& e : svc->events().drain()) initial |= std::holds_alternative<NetworkChanged>(e);
    CHECK(initial);
    NetworkState s = svc->state();
    test_devices(s);
    test_primary(s);
    test_connectivity(s);
    test_wifi(*svc, s);
    svc.reset();
    if (SUCCEEDED(hr)) CoUninitialize();
    return bstest::finish("test_win_network");
}
