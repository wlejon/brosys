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
}

}  // namespace

int main() {
    run_test();
    return bstest::finish(kName);
}
