// NetworkService against a scripted NetworkManager on a private bus: the
// initial tree, a burst of property changes coalescing into one snapshot,
// devices appearing / disappearing, primary connection changes,
// invalidated properties, Wi-Fi scans (completion via LastScan, timeout,
// refusal), and NetworkManager restarts.
#include "brosys/network.h"
#include "check.h"
#include "linux/fakes/event_log.h"
#include "linux/fakes/fake_nm.h"
#include "linux/support/private_bus.h"

using namespace std::chrono_literals;
using brosys::NetworkChanged;
using brosys::WifiScanCompleted;
using brosys::dbus::Value;

namespace {

constexpr const char* kName = "test_network_fake";
constexpr const char* kNM = "org.freedesktop.NetworkManager";
constexpr const char* kMgr = "/org/freedesktop/NetworkManager";
constexpr const char* kDevice = "org.freedesktop.NetworkManager.Device";
constexpr const char* kWired = "org.freedesktop.NetworkManager.Device.Wired";
constexpr const char* kWireless = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char* kActive = "org.freedesktop.NetworkManager.Connection.Active";
constexpr const char* kIp4 = "org.freedesktop.NetworkManager.IP4Config";
constexpr const char* kAp = "org.freedesktop.NetworkManager.AccessPoint";

constexpr const char* kEth = "/org/freedesktop/NetworkManager/Devices/2";
constexpr const char* kWifi = "/org/freedesktop/NetworkManager/Devices/3";
constexpr const char* kVeth = "/org/freedesktop/NetworkManager/Devices/9";
constexpr const char* kAc = "/org/freedesktop/NetworkManager/ActiveConnection/1";
constexpr const char* kIp4Path = "/org/freedesktop/NetworkManager/IP4Config/1";
constexpr const char* kAp1 = "/org/freedesktop/NetworkManager/AccessPoint/1";
constexpr const char* kAp2 = "/org/freedesktop/NetworkManager/AccessPoint/2";

Value paths(std::vector<std::string> p) {
    std::vector<Value> v;
    for (auto& s : p) v.push_back(Value::obj(s));
    return Value::array("o", v);
}

Value addresses(const std::string& a, uint32_t prefix) {
    return Value::array("a{sv}", {Value::vardict({{"address", Value::str(a)}, {"prefix", Value::u32(prefix)}})});
}

Value bytes(const std::string& s) { return Value::bytes(std::vector<uint8_t>(s.begin(), s.end())); }

void populate(bstest::FakeNM& nm) {
    nm.add_object(kMgr, {{kNM,
                          {{"Connectivity", Value::u32(4)},
                           {"NetworkingEnabled", Value::boolean(true)},
                           {"WirelessEnabled", Value::boolean(true)},
                           {"WirelessHardwareEnabled", Value::boolean(true)},
                           {"Devices", paths({kEth, kWifi})},
                           {"ActiveConnections", paths({kAc})},
                           {"PrimaryConnection", Value::obj(kAc)}}}},
                  true);
    nm.add_object(kEth, {{kDevice,
                          {{"Interface", Value::str("eth0")},
                           {"Driver", Value::str("e1000e")},
                           {"DeviceType", Value::u32(1)},
                           {"State", Value::u32(100)},
                           {"Managed", Value::boolean(true)},
                           {"HwAddress", Value::str("52:54:00:12:34:56")},
                           {"ActiveConnection", Value::obj(kAc)},
                           {"Ip4Config", Value::obj(kIp4Path)},
                           {"Ip6Config", Value::obj("/")}}},
                         {kWired, {{"Speed", Value::u32(1000)}}}},
                  true);
    nm.add_object(kIp4Path, {{kIp4,
                              {{"AddressData", addresses("192.168.7.2", 24)},
                               {"Gateway", Value::str("192.168.7.1")},
                               {"NameserverData", Value::array("a{sv}", {Value::vardict({{"address", Value::str("192.168.7.1")}})})}}}},
                  true);
    nm.add_object(kAc, {{kActive,
                         {{"Id", Value::str("Wired 1")},
                          {"Uuid", Value::str("u-1")},
                          {"Type", Value::str("802-3-ethernet")},
                          {"State", Value::u32(2)},
                          {"Devices", paths({kEth})},
                          {"Default", Value::boolean(true)},
                          {"Default6", Value::boolean(false)},
                          {"Vpn", Value::boolean(false)}}}},
                  true);
    nm.add_object(kWifi, {{kDevice,
                           {{"Interface", Value::str("wlan0")},
                            {"Driver", Value::str("iwlwifi")},
                            {"DeviceType", Value::u32(2)},
                            {"State", Value::u32(30)},
                            {"Managed", Value::boolean(true)},
                            {"HwAddress", Value::str("02:00:00:00:01:00")},
                            {"ActiveConnection", Value::obj("/")},
                            {"Ip4Config", Value::obj("/")},
                            {"Ip6Config", Value::obj("/")}}},
                          {kWireless,
                           {{"AccessPoints", paths({kAp1})},
                            {"ActiveAccessPoint", Value::obj("/")},
                            {"Bitrate", Value::u32(0)},
                            {"LastScan", Value::i64(500)}}}},
                  true);
    nm.add_object(kAp1, {{kAp,
                          {{"Ssid", bytes("cafe")},
                           {"HwAddress", Value::str("02:00:00:00:00:AA")},
                           {"Strength", Value::byte(60)},
                           {"Frequency", Value::u32(2462)},
                           {"Flags", Value::u32(1)},
                           {"WpaFlags", Value::u32(0)},
                           {"RsnFlags", Value::u32(0x188)}}}},
                  true);
}

int count_changed(const std::vector<brosys::NetworkEvent>& evs) {
    int n = 0;
    for (auto& e : evs) n += std::holds_alternative<NetworkChanged>(e);
    return n;
}

void run_test() {
    bstest::PrivateBus bus;
    if (!bus.ok()) bstest::skip(kName, bus.error());
    auto nm = std::make_unique<bstest::FakeNM>(bus.address());
    REQUIRE(nm->ok());
    populate(*nm);
    REQUIRE(nm->own_name());

    brosys::NetworkConfig cfg;
    cfg.system_bus_address = bus.address();
    cfg.scan_timeout_ms = 600;
    std::string err;
    auto net = brosys::NetworkService::create(cfg, &err);
    REQUIRE(net);
    bstest::EventLog<brosys::NetworkEvent> log(net->events());

    // ---- initial snapshot, queued before create() returned
    auto first = net->events().drain();
    REQUIRE(first.size() == 1);
    REQUIRE(std::holds_alternative<NetworkChanged>(first[0]));
    auto s = std::get<NetworkChanged>(first[0]).state;
    CHECK(s == net->state());
    CHECK(s.connectivity == brosys::Connectivity::Full);
    CHECK(s.wifi_enabled && s.wifi_hardware_enabled && s.networking_enabled);
    REQUIRE(s.devices.size() == 2);
    CHECK_EQ(s.primary_device, std::string(kEth));
    auto* eth = s.primary();
    REQUIRE(eth != nullptr);
    CHECK_EQ(eth->interface_name, std::string("eth0"));
    CHECK_EQ(eth->description, std::string("e1000e"));
    CHECK_EQ(eth->mac, std::string("52:54:00:12:34:56"));
    CHECK_EQ(eth->connection, std::string("Wired 1"));
    CHECK(eth->ipv4.addresses == std::vector<std::string>{"192.168.7.2/24"});
    CHECK(eth->ipv4.dns == std::vector<std::string>{"192.168.7.1"});
    CHECK(eth->ipv6 == brosys::IpConfig());
    CHECK(s.devices[1].type == brosys::LinkType::WiFi && s.devices[1].state == brosys::LinkState::Disconnected);
    REQUIRE(s.active_connections.size() == 1);
    CHECK(s.active_connections[0].device_ids == std::vector<std::string>{kEth});
    auto aps = net->access_points("");
    REQUIRE(aps.size() == 1);
    CHECK_EQ(aps[0].ssid, std::string("cafe"));
    CHECK_EQ(aps[0].bssid, std::string("02:00:00:00:00:aa"));
    CHECK_EQ(aps[0].channel, 11u);
    CHECK(aps[0].security == brosys::WifiSecurity::Wpa2Personal);
    CHECK(net->access_points(kEth).empty());

    // ---- a burst of separate PropertiesChanged signals -> one snapshot
    nm->batch([&] {
        nm->set(kIp4Path, kIp4, {{"AddressData", addresses("192.168.7.9", 24)}});
        nm->set(kMgr, kNM, {{"Connectivity", Value::u32(3)}});
        nm->set(kEth, kWired, {{"Speed", Value::u32(100)}});
        nm->set(kAc, kActive, {{"Default6", Value::boolean(true)}});
        nm->set(kAp1, kAp, {{"Strength", Value::byte(61)}});  // not part of NetworkState
    });
    auto ev = log.wait<NetworkChanged>();
    REQUIRE(ev.has_value());
    CHECK(ev->state.connectivity == brosys::Connectivity::Limited);
    REQUIRE(ev->state.primary());
    CHECK(ev->state.primary()->ipv4.addresses == std::vector<std::string>{"192.168.7.9/24"});
    CHECK_EQ(ev->state.primary()->speed_mbps, uint64_t(100));
    CHECK(ev->state.active_connections[0].default6);
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(count_changed(log.unseen()), 0);  // everything came in that one snapshot
    CHECK_EQ(int(net->access_points(kWifi)[0].strength_percent), 61);

    // A change NetworkState does not show produces no snapshot.
    nm->set(kAp1, kAp, {{"Strength", Value::byte(62)}});
    std::this_thread::sleep_for(200ms);
    CHECK_EQ(count_changed(log.unseen()), 0);

    // ---- a device appears, then goes
    nm->batch([&] {
        nm->add_object(kVeth, {{kDevice,
                                {{"Interface", Value::str("veth0")},
                                 {"DeviceType", Value::u32(20)},
                                 {"State", Value::u32(10)},
                                 {"Managed", Value::boolean(false)},
                                 {"HwAddress", Value::str("00:00:00:00:00:00")}}}});
        nm->set(kMgr, kNM, {{"Devices", paths({kEth, kWifi, kVeth})}});
    });
    ev = log.wait<NetworkChanged>([](const NetworkChanged& c) { return c.state.devices.size() == 3; });
    REQUIRE(ev.has_value());
    auto& veth = ev->state.devices[2];
    CHECK(veth.type == brosys::LinkType::Virtual && veth.state == brosys::LinkState::Unavailable && !veth.managed);
    CHECK(veth.mac.empty());
    nm->batch([&] {
        nm->set(kMgr, kNM, {{"Devices", paths({kEth, kWifi})}});
        nm->remove_object(kVeth);
    });
    ev = log.wait<NetworkChanged>([](const NetworkChanged& c) { return c.state.devices.size() == 2; });
    CHECK(ev.has_value());

    // ---- the primary connection goes away
    nm->set(kMgr, kNM, {{"PrimaryConnection", Value::obj("/")}});
    ev = log.wait<NetworkChanged>([](const NetworkChanged& c) { return c.state.primary_device.empty(); });
    REQUIRE(ev.has_value());
    CHECK(!ev->state.devices[0].is_primary);
    nm->set(kMgr, kNM, {{"PrimaryConnection", Value::obj(kAc)}});
    CHECK(log.wait<NetworkChanged>([](const NetworkChanged& c) { return c.state.primary_device == kEth; }));

    // ---- invalidated properties are fetched again
    nm->invalidate(kMgr, kNM, {{"WirelessEnabled", Value::boolean(false)}});
    ev = log.wait<NetworkChanged>([](const NetworkChanged& c) { return !c.state.wifi_enabled; });
    CHECK(ev.has_value());

    // ---- a scan that completes: a new AP shows up and LastScan moves
    nm->set_scan_hook([&](const std::string& dev) {
        nm->add_object(kAp2, {{kAp,
                               {{"Ssid", bytes("lab")},
                                {"HwAddress", Value::str("02:00:00:00:00:BB")},
                                {"Strength", Value::byte(40)},
                                {"Frequency", Value::u32(5745)},
                                {"Flags", Value::u32(1)},
                                {"WpaFlags", Value::u32(0)},
                                {"RsnFlags", Value::u32(0x400)}}}});
        nm->set(dev, kWireless, {{"AccessPoints", paths({kAp1, kAp2})}});
    });
    nm->set_scan_mode(bstest::FakeNM::ScanMode::Complete, 100);
    log.skip_all();
    auto r = net->request_wifi_scan("");
    CHECK(r.ok);
    auto done = log.wait<WifiScanCompleted>();
    REQUIRE(done.has_value());
    CHECK(done->ok);
    CHECK_EQ(done->device_id, std::string(kWifi));
    REQUIRE(done->access_points.size() == 2);
    CHECK_EQ(done->access_points[1].ssid, std::string("lab"));
    CHECK_EQ(done->access_points[1].channel, 149u);
    CHECK(done->access_points[1].security == brosys::WifiSecurity::Wpa3Personal);
    CHECK_EQ(net->access_points(kWifi).size(), size_t(2));
    std::this_thread::sleep_for(800ms);  // past the timeout: no late failure for a finished scan
    for (auto& e : log.unseen()) CHECK(!std::holds_alternative<WifiScanCompleted>(e));

    // ---- a scan that never finishes times out
    nm->set_scan_mode(bstest::FakeNM::ScanMode::Ignore);
    CHECK(net->request_wifi_scan(kWifi).ok);
    done = log.wait<WifiScanCompleted>(3000ms);
    REQUIRE(done.has_value());
    CHECK(!done->ok);
    CHECK(!done->error.empty());
    CHECK_EQ(done->access_points.size(), size_t(2));

    // ---- refused scans, non-Wi-Fi devices
    nm->set_scan_mode(bstest::FakeNM::ScanMode::Fail);
    r = net->request_wifi_scan(kWifi);
    CHECK(!r.ok);
    CHECK(r.error.find("NotAllowed") != std::string::npos);
    r = net->request_wifi_scan("");  // every device: refused, and reported per device
    CHECK(!r.ok);
    done = log.wait<WifiScanCompleted>(2000ms);
    CHECK(done && !done->ok && done->device_id == kWifi);
    CHECK(!net->request_wifi_scan(kEth).ok);
    CHECK_EQ(nm->scan_requests(), 4);

    // ---- an OS-initiated scan (LastScan moving on its own) is reported too
    nm->set(kWifi, kWireless, {{"LastScan", Value::i64(999999)}});
    done = log.wait<WifiScanCompleted>(2000ms);
    CHECK(done && done->ok);

    // ---- NetworkManager restarts
    nm.reset();
    ev = log.wait<NetworkChanged>([](const NetworkChanged& c) { return c.state.devices.empty(); });
    CHECK(ev && ev->state.connectivity == brosys::Connectivity::Unknown);
    nm = std::make_unique<bstest::FakeNM>(bus.address());
    REQUIRE(nm->ok());
    populate(*nm);
    REQUIRE(nm->own_name());
    ev = log.wait<NetworkChanged>([](const NetworkChanged& c) { return c.state.devices.size() == 2; });
    CHECK(ev && ev->state.primary_device == kEth);

    // ---- the system bus daemon restarts (same address): empty while it is
    // down, then reloaded once NetworkManager (the fake, which reconnects and
    // re-owns its name) is back on the new daemon.
    REQUIRE(bus.restart());
    ev = log.wait<NetworkChanged>([](const NetworkChanged& c) { return c.state.devices.empty(); });
    CHECK(ev.has_value());
    ev = log.wait<NetworkChanged>([](const NetworkChanged& c) { return c.state.devices.size() == 2; }, 15000ms);
    CHECK(ev && ev->state.primary_device == kEth);
    // Signals arrive on the new connection: an OS-initiated scan completes.
    nm->set(kWifi, kWireless, {{"LastScan", Value::i64(1234567)}});
    done = log.wait<WifiScanCompleted>(5000ms);
    CHECK(done && done->ok);

    // ---- network control operations (Wi-Fi, disconnect, VPN)
    // 1. connect_wifi (WPA2-Personal, auto-detecting Wi-Fi device)
    auto res = net->connect_wifi("", "TestWifi", "secret123", brosys::WifiSecurity::Wpa2Personal);
    CHECK(res.ok);
    auto aa_calls = nm->add_and_activate_calls();
    REQUIRE(aa_calls.size() == 1);
    CHECK_EQ(aa_calls[0].device, std::string(kWifi));
    CHECK_EQ(aa_calls[0].specific_object, std::string("/"));
    auto* conn_sec = aa_calls[0].connection.lookup("connection");
    REQUIRE(conn_sec != nullptr);
    CHECK_EQ(conn_sec->lookup("id")->as_string(), std::string("TestWifi"));
    CHECK_EQ(conn_sec->lookup("type")->as_string(), std::string("802-11-wireless"));
    auto* wifi_sec = aa_calls[0].connection.lookup("802-11-wireless");
    REQUIRE(wifi_sec != nullptr);
    CHECK_EQ(wifi_sec->lookup("mode")->as_string(), std::string("infrastructure"));
    auto* sec_sec = aa_calls[0].connection.lookup("802-11-wireless-security");
    REQUIRE(sec_sec != nullptr);
    CHECK_EQ(sec_sec->lookup("key-mgmt")->as_string(), std::string("wpa-psk"));
    CHECK_EQ(sec_sec->lookup("psk")->as_string(), std::string("secret123"));

    // connect_wifi (WPA3-Personal with device path)
    res = net->connect_wifi(kWifi, "Wpa3Wifi", "pass3", brosys::WifiSecurity::Wpa3Personal);
    CHECK(res.ok);
    aa_calls = nm->add_and_activate_calls();
    REQUIRE(aa_calls.size() == 2);
    CHECK_EQ(aa_calls[1].device, std::string(kWifi));
    sec_sec = aa_calls[1].connection.lookup("802-11-wireless-security");
    REQUIRE(sec_sec != nullptr);
    CHECK_EQ(sec_sec->lookup("key-mgmt")->as_string(), std::string("sae"));
    CHECK_EQ(sec_sec->lookup("psk")->as_string(), std::string("pass3"));

    // connect_wifi (Open security with interface name)
    res = net->connect_wifi("wlan0", "OpenNet", "", brosys::WifiSecurity::Open);
    CHECK(res.ok);
    aa_calls = nm->add_and_activate_calls();
    REQUIRE(aa_calls.size() == 3);
    CHECK_EQ(aa_calls[2].device, std::string(kWifi));
    CHECK(aa_calls[2].connection.lookup("802-11-wireless-security") == nullptr);

    // connect_wifi (WEP security)
    res = net->connect_wifi("wlan0", "WepNet", "weppass", brosys::WifiSecurity::Wep);
    CHECK(res.ok);
    aa_calls = nm->add_and_activate_calls();
    REQUIRE(aa_calls.size() == 4);
    sec_sec = aa_calls[3].connection.lookup("802-11-wireless-security");
    REQUIRE(sec_sec != nullptr);
    CHECK_EQ(sec_sec->lookup("key-mgmt")->as_string(), std::string("none"));
    CHECK_EQ(sec_sec->lookup("wep-key0")->as_string(), std::string("weppass"));

    // connect_wifi failure handling
    nm->set_add_and_activate_hook([](const auto&, const auto&, const auto&) {
        return brosys::dbus::MethodResult::error("org.freedesktop.NetworkManager.Failed", "Connection failed");
    });
    res = net->connect_wifi("wlan0", "FailNet", "pass", brosys::WifiSecurity::Wpa2Personal);
    CHECK(!res.ok);
    CHECK(res.error.find("Connection failed") != std::string::npos);
    nm->set_add_and_activate_hook(nullptr);

    // 2. disconnect
    // Disconnect active connection by default ("")
    res = net->disconnect("");
    CHECK(res.ok);
    auto deact_calls = nm->deactivate_calls();
    REQUIRE(deact_calls.size() == 1);
    CHECK_EQ(deact_calls[0], std::string(kAc));

    // Disconnect active connection by name
    res = net->disconnect("Wired 1");
    CHECK(res.ok);
    deact_calls = nm->deactivate_calls();
    REQUIRE(deact_calls.size() == 2);
    CHECK_EQ(deact_calls[1], std::string(kAc));

    // Disconnect active connection by UUID
    res = net->disconnect("u-1");
    CHECK(res.ok);
    deact_calls = nm->deactivate_calls();
    REQUIRE(deact_calls.size() == 3);
    CHECK_EQ(deact_calls[2], std::string(kAc));

    // Disconnect device by interface name
    res = net->disconnect("wlan0");
    CHECK(res.ok);
    auto dev_disc_calls = nm->device_disconnect_calls();
    REQUIRE(dev_disc_calls.size() == 1);
    CHECK_EQ(dev_disc_calls[0], std::string(kWifi));

    // Disconnect device by device path
    res = net->disconnect(kWifi);
    CHECK(res.ok);
    dev_disc_calls = nm->device_disconnect_calls();
    REQUIRE(dev_disc_calls.size() == 2);
    CHECK_EQ(dev_disc_calls[1], std::string(kWifi));

    // Disconnect non-existent device/connection
    res = net->disconnect("unknown-net-dev");
    CHECK(!res.ok);

    // Disconnect error hook
    nm->set_deactivate_hook([](const auto&) {
        return brosys::dbus::MethodResult::error("org.freedesktop.NetworkManager.Failed", "Deactivation failed");
    });
    res = net->disconnect(kAc);
    CHECK(!res.ok);
    CHECK(res.error.find("Deactivation failed") != std::string::npos);
    nm->set_deactivate_hook(nullptr);

    // 3. connect_vpn
    // Add VPN connections to Settings
    constexpr const char* kSettingsVpn = "/org/freedesktop/NetworkManager/Settings/10";
    constexpr const char* kSettingsWg = "/org/freedesktop/NetworkManager/Settings/11";
    std::vector<std::pair<std::string, Value>> vpn_props;
    vpn_props.emplace_back("id", Value::str("Corporate VPN"));
    vpn_props.emplace_back("uuid", Value::str("vpn-uuid-corp"));
    vpn_props.emplace_back("type", Value::str("vpn"));
    nm->add_connection(kSettingsVpn, Value::dict("s", "a{sv}", {{Value::str("connection"), Value::vardict(vpn_props)}}));

    std::vector<std::pair<std::string, Value>> wg_props;
    wg_props.emplace_back("id", Value::str("Home WG"));
    wg_props.emplace_back("uuid", Value::str("wg-uuid-home"));
    wg_props.emplace_back("type", Value::str("wireguard"));
    nm->add_connection(kSettingsWg, Value::dict("s", "a{sv}", {{Value::str("connection"), Value::vardict(wg_props)}}));

    // Connect by name
    res = net->connect_vpn("Corporate VPN");
    CHECK(res.ok);
    auto act_calls = nm->activate_calls();
    REQUIRE(act_calls.size() == 1);
    CHECK_EQ(act_calls[0].connection, std::string(kSettingsVpn));
    CHECK_EQ(act_calls[0].device, std::string("/"));

    // Connect by UUID
    res = net->connect_vpn("vpn-uuid-corp");
    CHECK(res.ok);
    act_calls = nm->activate_calls();
    REQUIRE(act_calls.size() == 2);
    CHECK_EQ(act_calls[1].connection, std::string(kSettingsVpn));

    // Connect wireguard VPN
    res = net->connect_vpn("Home WG");
    CHECK(res.ok);
    act_calls = nm->activate_calls();
    REQUIRE(act_calls.size() == 3);
    CHECK_EQ(act_calls[2].connection, std::string(kSettingsWg));

    // Connect non-existent VPN
    res = net->connect_vpn("NoSuchVPN");
    CHECK(!res.ok);
    CHECK(res.error.find("VPN connection not found") != std::string::npos);

    // Connect VPN error hook
    nm->set_activate_hook([](const auto&, const auto&, const auto&) {
        return brosys::dbus::MethodResult::error("org.freedesktop.NetworkManager.Failed", "Activation rejected");
    });
    res = net->connect_vpn("Corporate VPN");
    CHECK(!res.ok);
    CHECK(res.error.find("Activation rejected") != std::string::npos);
    nm->set_activate_hook(nullptr);

    // 4. disconnect_vpn
    // Add active VPN connection to FakeNM
    constexpr const char* kAcVpn = "/org/freedesktop/NetworkManager/ActiveConnection/20";
    nm->add_object(kAcVpn, {{kActive,
                             {{"Id", Value::str("Corporate VPN")},
                              {"Uuid", Value::str("vpn-uuid-corp")},
                              {"Type", Value::str("vpn")},
                              {"State", Value::u32(2)},
                              {"Devices", paths({})},
                              {"Default", Value::boolean(false)},
                              {"Default6", Value::boolean(false)},
                              {"Vpn", Value::boolean(true)}}}});
    nm->set(kMgr, kNM, {{"ActiveConnections", paths({kAc, kAcVpn})}});

    ev = log.wait<NetworkChanged>([](const NetworkChanged& c) { return c.state.active_connections.size() == 2; });
    REQUIRE(ev.has_value());

    // Disconnect active VPN by name
    res = net->disconnect_vpn("Corporate VPN");
    CHECK(res.ok);
    deact_calls = nm->deactivate_calls();
    CHECK_EQ(deact_calls.back(), std::string(kAcVpn));

    // Disconnect active VPN by UUID
    res = net->disconnect_vpn("vpn-uuid-corp");
    CHECK(res.ok);
    deact_calls = nm->deactivate_calls();
    CHECK_EQ(deact_calls.back(), std::string(kAcVpn));

    // Disconnect active VPN by object path
    res = net->disconnect_vpn(kAcVpn);
    CHECK(res.ok);
    deact_calls = nm->deactivate_calls();
    CHECK_EQ(deact_calls.back(), std::string(kAcVpn));

    // Disconnect non-existent VPN
    res = net->disconnect_vpn("NoSuchVPN");
    CHECK(!res.ok);
    CHECK(res.error.find("active VPN connection not found") != std::string::npos);
}

}  // namespace

int main() {
    run_test();
    return bstest::finish(kName);
}
