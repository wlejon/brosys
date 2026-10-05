// The pure translation layers of the Linux power / network / audio
// backends, fed with the values UPower, NetworkManager and PipeWire send.
#include "check.h"
#include "linux/network/nm_model.h"
#include "linux/power/upower_model.h"
#ifdef BROSYS_TEST_PIPEWIRE
#include "linux/audio/pw_model.h"
#include "linux/audio/pw_pod.h"
#include <spa/pod/builder.h>
#endif

#include <cmath>
#include <iostream>

using brosys::dbus::Value;
namespace bs = brosys;

namespace {

bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }

void test_upower_device() {
    bs::upower::Props p{
        {"Type", Value::u32(2)},         {"PowerSupply", Value::boolean(true)},
        {"IsPresent", Value::boolean(true)}, {"State", Value::u32(2)},
        {"Technology", Value::u32(1)},   {"Percentage", Value::dbl(42.5)},
        {"TimeToEmpty", Value::i64(3600)}, {"TimeToFull", Value::i64(0)},
        {"Energy", Value::dbl(21.0)},    {"EnergyFull", Value::dbl(50.0)},
        {"EnergyFullDesign", Value::dbl(55.0)}, {"EnergyRate", Value::dbl(-7.5)},
        {"Vendor", Value::str("ACME")},  {"Model", Value::str("B1")}, {"Serial", Value::str("007")},
    };
    auto d = bs::upower::device_from_props("/org/freedesktop/UPower/devices/battery_BAT0", p);
    REQUIRE(d.has_value());
    CHECK(d->kind == bs::PowerDeviceKind::Battery);
    CHECK(d->power_supply);
    CHECK(d->state == bs::BatteryState::Discharging);
    CHECK(d->technology == bs::BatteryTechnology::LithiumIon);
    CHECK(d->percent && near(*d->percent, 42.5));
    CHECK(d->time_to_empty_s && *d->time_to_empty_s == 3600);
    CHECK(!d->time_to_full_s);
    CHECK(d->energy_rate_w && near(*d->energy_rate_w, 7.5));
    CHECK(d->energy_full_design_wh && near(*d->energy_full_design_wh, 55.0));
    CHECK_EQ(d->vendor, std::string("ACME"));

    // Filtering.
    auto line = p;
    line["Type"] = Value::u32(1);
    CHECK(!bs::upower::device_from_props("/x/line_power_AC", line));
    auto absent = p;
    absent["IsPresent"] = Value::boolean(false);
    CHECK(!bs::upower::device_from_props("/x/battery_BAT1", absent));
    CHECK(!bs::upower::device_from_props(bs::upower::kDisplayDevice, p));

    // Peripherals: no energy figures when the capacity is unknown.
    bs::upower::Props mouse{{"Type", Value::u32(5)}, {"PowerSupply", Value::boolean(false)},
                            {"Percentage", Value::dbl(80)}, {"EnergyFull", Value::dbl(0)}};
    auto m = bs::upower::device_from_props("/x/mouse", mouse);
    REQUIRE(m.has_value());
    CHECK(m->kind == bs::PowerDeviceKind::Mouse);
    CHECK(!m->power_supply);
    CHECK(!m->energy_wh && !m->energy_full_wh);
    CHECK(bs::upower::kind_from_type(3) == bs::PowerDeviceKind::Ups);
    CHECK(bs::upower::kind_from_type(12) == bs::PowerDeviceKind::Gamepad);
    CHECK(bs::upower::kind_from_type(19) == bs::PowerDeviceKind::Headset);
    CHECK(bs::upower::kind_from_type(4) == bs::PowerDeviceKind::Other);
}

void test_upower_aggregate() {
    bs::PowerState s;
    bs::PowerDevice a;
    a.kind = bs::PowerDeviceKind::Battery;
    a.power_supply = true;
    a.state = bs::BatteryState::Discharging;
    a.percent = 50;
    a.energy_wh = 25;
    a.energy_full_wh = 50;
    a.energy_rate_w = 10;
    a.time_to_empty_s = 9000;
    bs::PowerDevice b = a;
    b.percent = 100;
    b.energy_wh = 50;
    b.energy_full_wh = 50;
    bs::PowerDevice mouse;
    mouse.kind = bs::PowerDeviceKind::Mouse;
    mouse.percent = 5;
    bs::PowerDevice ups;
    ups.kind = bs::PowerDeviceKind::Ups;
    ups.power_supply = true;
    ups.percent = 1;

    s.devices = {mouse, ups};
    bs::upower::aggregate(s);
    CHECK(!s.percent && !s.time_to_empty_s);  // no power-supply battery
    CHECK(!s.has_system_battery());

    s.devices = {a, mouse, ups};
    bs::upower::aggregate(s);
    CHECK(s.percent && near(*s.percent, 50));
    CHECK(s.time_to_empty_s && *s.time_to_empty_s == 9000);

    s.devices = {a, b, mouse};
    bs::upower::aggregate(s);
    CHECK(s.percent && near(*s.percent, 75));                // 75 Wh of 100 Wh
    CHECK(s.time_to_empty_s && *s.time_to_empty_s == 13500);  // 75 Wh at 20 W
    CHECK(!s.time_to_full_s);
}

void test_logind_mapping() {
    CHECK(bs::upower::availability_from_logind("yes") == bs::Availability::Yes);
    CHECK(bs::upower::availability_from_logind("challenge") == bs::Availability::NeedsAuth);
    CHECK(bs::upower::availability_from_logind("no") == bs::Availability::No);
    CHECK(bs::upower::availability_from_logind("na") == bs::Availability::No);
    CHECK(bs::upower::availability_from_logind("") == bs::Availability::Unknown);
    CHECK_EQ(bs::upower::inhibit_what(bs::inhibit::Sleep | bs::inhibit::Idle), std::string("sleep:idle"));
    CHECK_EQ(bs::upower::inhibit_what(bs::inhibit::Shutdown | bs::inhibit::LidSwitch | bs::inhibit::PowerKey),
             std::string("shutdown:handle-lid-switch:handle-power-key"));
    CHECK_EQ(bs::upower::inhibit_what(0), std::string());
}

void test_nm_mappings() {
    using bs::WifiSecurity;
    CHECK(bs::nm::security_from_flags(0, 0, 0) == WifiSecurity::Open);
    CHECK(bs::nm::security_from_flags(1, 0, 0) == WifiSecurity::Wep);
    CHECK(bs::nm::security_from_flags(1, 0x100 | 0x8, 0) == WifiSecurity::WpaPersonal);
    CHECK(bs::nm::security_from_flags(1, 0, 0x100 | 0x8) == WifiSecurity::Wpa2Personal);
    CHECK(bs::nm::security_from_flags(1, 0, 0x100 | 0x400) == WifiSecurity::Wpa3Personal);  // transition mode
    CHECK(bs::nm::security_from_flags(1, 0, 0x200) == WifiSecurity::Wpa2Enterprise);
    CHECK(bs::nm::security_from_flags(1, 0x200, 0) == WifiSecurity::WpaEnterprise);
    CHECK(bs::nm::security_from_flags(1, 0, 0x2000) == WifiSecurity::Wpa3Enterprise);
    CHECK(bs::nm::security_from_flags(0, 0, 0x800) == WifiSecurity::Owe);
    CHECK_EQ(bs::nm::channel_from_frequency(2412), 1u);
    CHECK_EQ(bs::nm::channel_from_frequency(2472), 13u);
    CHECK_EQ(bs::nm::channel_from_frequency(2484), 14u);
    CHECK_EQ(bs::nm::channel_from_frequency(5180), 36u);
    CHECK_EQ(bs::nm::channel_from_frequency(5825), 165u);
    CHECK_EQ(bs::nm::channel_from_frequency(5955), 1u);
    CHECK_EQ(bs::nm::channel_from_frequency(6415), 93u);
    CHECK_EQ(bs::nm::channel_from_frequency(1000), 0u);
    CHECK(bs::nm::link_type_from_device_type(1) == bs::LinkType::Ethernet);
    CHECK(bs::nm::link_type_from_device_type(32) == bs::LinkType::Loopback);
    CHECK(bs::nm::link_type_from_device_type(13) == bs::LinkType::Bridge);
    CHECK(bs::nm::link_state_from_device_state(100) == bs::LinkState::Connected);
    CHECK(bs::nm::link_state_from_device_state(70) == bs::LinkState::Connecting);
    CHECK(bs::nm::link_state_from_device_state(10) == bs::LinkState::Unavailable);
    CHECK(bs::nm::connectivity_from_nm(4) == bs::Connectivity::Full);
    CHECK(bs::nm::connectivity_from_nm(2) == bs::Connectivity::Portal);
}

Value addr(const std::string& a, uint32_t prefix) {
    return Value::vardict({{"address", Value::str(a)}, {"prefix", Value::u32(prefix)}});
}

void test_nm_tree() {
    using namespace bs::nm;
    Objects o;
    o[kPath][kIface] = {
        {"Connectivity", Value::u32(4)},
        {"NetworkingEnabled", Value::boolean(true)},
        {"WirelessEnabled", Value::boolean(true)},
        {"WirelessHardwareEnabled", Value::boolean(false)},
        {"Devices", Value::array("o", {Value::obj("/d/1"), Value::obj("/d/2"), Value::obj("/d/missing")})},
        {"ActiveConnections", Value::array("o", {Value::obj("/ac/1")})},
        {"PrimaryConnection", Value::obj("/ac/1")},
    };
    o["/d/1"][kDevice] = {{"Interface", Value::str("enp7s0")}, {"Driver", Value::str("igb")},
                          {"DeviceType", Value::u32(1)},       {"State", Value::u32(100)},
                          {"Managed", Value::boolean(true)},   {"HwAddress", Value::str("A8:A1:59:3F:28:10")},
                          {"ActiveConnection", Value::obj("/ac/1")}, {"Ip4Config", Value::obj("/ip4/1")},
                          {"Ip6Config", Value::obj("/ip6/1")}};
    o["/d/1"][kWired] = {{"Speed", Value::u32(1000)}};
    std::vector<uint8_t> dns6(16, 0);
    dns6[0] = 0xfd;
    dns6[15] = 1;
    o["/ip4/1"][kIp4] = {{"AddressData", Value::array("a{sv}", {addr("10.1.0.6", 24)})},
                         {"Gateway", Value::str("10.1.0.1")},
                         {"NameserverData", Value::array("a{sv}", {Value::vardict({{"address", Value::str("10.1.0.6")}})})}};
    o["/ip6/1"][kIp6] = {{"AddressData", Value::array("a{sv}", {addr("fe80::1", 64)})},
                         {"Gateway", Value::str("")},
                         {"Nameservers", Value::array("ay", {Value::bytes(dns6)})}};
    o["/ac/1"][kActive] = {{"Id", Value::str("Wired connection 1")}, {"Uuid", Value::str("c5ab")},
                           {"Type", Value::str("802-3-ethernet")},   {"State", Value::u32(2)},
                           {"Devices", Value::array("o", {Value::obj("/d/1")})},
                           {"Default", Value::boolean(true)},        {"Default6", Value::boolean(false)}};
    o["/d/2"][kDevice] = {{"Interface", Value::str("wlan0")}, {"DeviceType", Value::u32(2)}, {"State", Value::u32(30)},
                          {"ActiveConnection", Value::obj("/")}, {"Ip4Config", Value::obj("/")}};
    o["/d/2"][kWireless] = {{"HwAddress", Value::str("02:00:00:00:01:00")},
                            {"Bitrate", Value::u32(54000)},
                            {"AccessPoints", Value::array("o", {Value::obj("/ap/1"), Value::obj("/ap/2")})},
                            {"ActiveAccessPoint", Value::obj("/ap/2")},
                            {"LastScan", Value::i64(1234)}};
    o["/ap/1"][kAccessPoint] = {{"Ssid", Value::bytes({'h', 'o', 'm', 'e'})}, {"HwAddress", Value::str("02:00:00:00:00:00")},
                                {"Strength", Value::byte(70)},  {"Frequency", Value::u32(2437)},
                                {"Flags", Value::u32(1)},       {"WpaFlags", Value::u32(0)},
                                {"RsnFlags", Value::u32(0x188)}};
    o["/ap/2"][kAccessPoint] = {{"Ssid", Value::bytes({})}, {"HwAddress", Value::str("02:00:00:00:00:01")},
                                {"Strength", Value::byte(30)}, {"Frequency", Value::u32(5180)},
                                {"Flags", Value::u32(0)}};

    auto s = build_state(o);
    CHECK(s.connectivity == bs::Connectivity::Full);
    CHECK(s.wifi_enabled && !s.wifi_hardware_enabled);
    REQUIRE(s.devices.size() == 2);
    CHECK_EQ(s.primary_device, std::string("/d/1"));
    auto* p = s.primary();
    REQUIRE(p != nullptr);
    CHECK_EQ(p->interface_name, std::string("enp7s0"));
    CHECK_EQ(p->mac, std::string("a8:a1:59:3f:28:10"));
    CHECK_EQ(p->speed_mbps, uint64_t(1000));
    CHECK_EQ(p->connection, std::string("Wired connection 1"));
    CHECK(p->ipv4.addresses == std::vector<std::string>{"10.1.0.6/24"});
    CHECK(p->ipv4.gateways == std::vector<std::string>{"10.1.0.1"});
    CHECK(p->ipv4.dns == std::vector<std::string>{"10.1.0.6"});
    CHECK(p->ipv6.addresses == std::vector<std::string>{"fe80::1/64"});
    CHECK(p->ipv6.gateways.empty());
    CHECK(p->ipv6.dns == std::vector<std::string>{"fd00::1"});
    auto& w = s.devices[1];
    CHECK(w.type == bs::LinkType::WiFi && w.state == bs::LinkState::Disconnected && !w.is_primary);
    CHECK_EQ(w.mac, std::string("02:00:00:00:01:00"));
    CHECK_EQ(w.speed_mbps, uint64_t(54));
    REQUIRE(s.active_connections.size() == 1);
    CHECK(s.active_connections[0].default4 && !s.active_connections[0].default6);
    CHECK(s.active_connections[0].type == bs::LinkType::Ethernet);

    auto aps = access_points(o, "/d/2");
    REQUIRE(aps.size() == 2);
    CHECK_EQ(aps[0].ssid, std::string("home"));
    CHECK_EQ(aps[0].channel, 6u);
    CHECK(aps[0].security == bs::WifiSecurity::Wpa2Personal);
    CHECK(!aps[0].active && aps[1].active);
    CHECK(aps[1].ssid.empty() && aps[1].channel == 36 && aps[1].security == bs::WifiSecurity::Open);
    CHECK(wifi_devices(o) == std::vector<std::string>{"/d/2"});
    CHECK(last_scan(o, "/d/2") == std::optional<int64_t>(1234));
    CHECK(!last_scan(o, "/d/1"));

    // PropertiesChanged.
    CHECK(apply_properties_changed(o, "/d/2", kWireless, Value::vardict({{"LastScan", Value::i64(99)}})));
    CHECK(!apply_properties_changed(o, "/d/2", kWireless, Value::vardict({{"LastScan", Value::i64(99)}})));
    CHECK(last_scan(o, "/d/2") == std::optional<int64_t>(99));
}

#ifdef BROSYS_TEST_PIPEWIRE
void test_pw_model() {
    using namespace bs::pw;
    CHECK(near(linear_to_ui(0.064f), 0.4, 1e-4));
    CHECK(near(ui_to_linear(0.4f), 0.064, 1e-6));
    CHECK_EQ(parse_default_name("{\"name\":\"alsa_output.x\"}"), std::string("alsa_output.x"));
    CHECK_EQ(parse_default_name("{ name = \"a b\" }"), std::string("a b"));
    CHECK_EQ(parse_default_name("{ \"name\": \"q\\\"x\\u00e9\" }"), std::string("q\"x\xc3\xa9"));
    CHECK_EQ(parse_default_name(default_json("we\"ird\\name")), std::string("we\"ird\\name"));
    CHECK_EQ(parse_default_name("garbage"), std::string());

    CHECK(direction_of("Audio/Sink", {}) == bs::AudioDirection::Output);
    CHECK(direction_of("Audio/Source/Virtual", {}) == bs::AudioDirection::Input);
    CHECK(!direction_of("Stream/Output/Audio", {}));
    CHECK(!direction_of("Audio/Source", {{"stream.monitor", "true"}}));
    CHECK(!direction_of("Video/Source", {}));
    CHECK_EQ(form_factor_from_port_type("speaker"), std::string("speakers"));
    CHECK_EQ(form_factor_from_port_type("mic"), std::string("microphone"));
    CHECK_EQ(form_factor_from_port_type("hdmi"), std::string("hdmi"));
    CHECK_EQ(form_factor_from_props({{"device.form-factor", "headphone"}}), std::string("headphones"));

    Graph g;
    g.devices[49] = Device{49, {{"device.description", "HDA"}}, true, {}};
    Route r;
    r.index = 5;
    r.device = 11;
    r.port_type = "spdif";
    r.has_volume = true;
    r.volumes = {0.064f, 0.064f};
    g.devices[49].routes.push_back(r);
    g.nodes[53] = Node{53,
                       {{"media.class", "Audio/Sink"}, {"node.name", "out"}, {"node.description", "Out"},
                        {"device.id", "49"}, {"card.profile.device", "11"}},
                       true, true, {0.064f, 0.008f}, true, false};
    g.nodes[54] = Node{54, {{"media.class", "Audio/Source"}, {"node.name", "in"}}, false, false, {}, false, false};
    g.default_sink = "out";
    g.default_source = "nope";
    auto s = build_state(g);
    REQUIRE(s.devices.size() == 1);  // 54 has not settled
    auto& d = s.devices[0];
    CHECK_EQ(d.id, std::string("out"));
    CHECK_EQ(d.device_name, std::string("HDA"));
    CHECK_EQ(d.form_factor, std::string("spdif"));
    CHECK(d.is_default && s.default_output == "out" && s.default_input.empty());
    CHECK(near(d.volume, 0.4, 1e-4));
    REQUIRE(d.channel_volumes.size() == 2);
    CHECK(near(d.channel_volumes[1], 0.2, 1e-4));
    CHECK(route_for_node(g, g.nodes[53]) != nullptr);

    auto init = initial_events(s);
    CHECK_EQ(init.size(), size_t(3));
    CHECK(std::holds_alternative<bs::AudioDeviceAdded>(init[0]));
    CHECK(std::holds_alternative<bs::AudioDefaultChanged>(init[2]));

    // Diff: a mute flip and a new device, default moved.
    Graph g2 = g;
    g2.nodes[53].mute = true;
    g2.nodes[54].settled = true;
    g2.default_source = "in";
    auto s2 = build_state(g2);
    auto ev = diff(s, s2);
    REQUIRE(ev.size() == 3);
    CHECK(std::holds_alternative<bs::AudioDeviceAdded>(ev[0]));
    REQUIRE(std::holds_alternative<bs::AudioDeviceChanged>(ev[1]));
    CHECK_EQ(std::get<bs::AudioDeviceChanged>(ev[1]).changes, bs::audio_change::Mute);
    REQUIRE(std::holds_alternative<bs::AudioDefaultChanged>(ev[2]));
    CHECK(std::get<bs::AudioDefaultChanged>(ev[2]).direction == bs::AudioDirection::Input);
    g2.nodes.erase(53);
    auto ev2 = diff(s2, build_state(g2));
    REQUIRE(ev2.size() == 2);
    CHECK(std::holds_alternative<bs::AudioDeviceRemoved>(ev2[0]));
    CHECK(std::get<bs::AudioDefaultChanged>(ev2[1]).id.empty());
}

void test_pw_pods() {
    using namespace bs::pw;
    uint8_t buf[2048];
    spa_pod_builder b;
    spa_pod_builder_init(&b, buf, sizeof buf);
    std::vector<float> vols{0.5f, 0.25f};
    const spa_pod* props = build_props(&b, &vols, true);
    PropsValues v;
    REQUIRE(parse_props(props, &v));
    CHECK(v.has_volume && v.volumes == vols);
    CHECK(v.has_mute && v.mute);

    spa_pod_builder_init(&b, buf, sizeof buf);
    Route r;
    r.index = 3;
    r.device = 7;
    const spa_pod* route = build_route(&b, r, &vols, std::nullopt);
    Route back;
    REQUIRE(parse_route(route, &back));
    CHECK_EQ(back.index, 3);
    CHECK_EQ(back.device, 7);
    CHECK(back.has_volume && back.volumes == vols);
    CHECK(!back.has_mute);
    CHECK(!parse_props(route, &v));
}
#endif

}  // namespace

int main() {
    test_upower_device();
    test_upower_aggregate();
    test_logind_mapping();
    test_nm_mappings();
    test_nm_tree();
#ifdef BROSYS_TEST_PIPEWIRE
    test_pw_model();
    test_pw_pods();
#endif
    return bstest::finish("test_system_models");
}
