// Test for Bluetooth (BlueZ) management.
//
// Tests adapter discovery, powered state, discovery toggle, device enumeration,
// connect/disconnect/pair/remove, and AdapterChanged/DeviceFound/DeviceChanged/DeviceRemoved events.
#include "check.h"
#include "brosys/bluetooth.h"
#include "linux/desktop/event_log.h"
#include "linux/fakes/fake_bluez.h"
#include "linux/support/private_bus.h"

#include <csignal>

using namespace brosys;
using namespace std::chrono_literals;
using Log = bstest::EventLog<BluetoothEvent>;

namespace {

void test_bluetooth_fake() {
    bstest::PrivateBus bus;
    REQUIRE(bus.ok());

    bstest::FakeBlueZ fake(bus.address());
    REQUIRE(fake.ok());

    // Populate fake BlueZ with an adapter and a device
    fake.add_adapter("/org/bluez/hci0", {
        {"Address", dbus::Value::str("00:11:22:33:44:55")},
        {"Name", dbus::Value::str("hci0")},
        {"Alias", dbus::Value::str("Controller 0")},
        {"Powered", dbus::Value::boolean(false)},
        {"Discovering", dbus::Value::boolean(false)},
        {"Pairable", dbus::Value::boolean(true)},
        {"Discoverable", dbus::Value::boolean(false)},
    });

    fake.add_device("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF", {
        {"Address", dbus::Value::str("AA:BB:CC:DD:EE:FF")},
        {"Adapter", dbus::Value::obj("/org/bluez/hci0")},
        {"Name", dbus::Value::str("Wireless Headphones")},
        {"Alias", dbus::Value::str("Headphones")},
        {"Icon", dbus::Value::str("audio-headphones")},
        {"Paired", dbus::Value::boolean(true)},
        {"Connected", dbus::Value::boolean(false)},
        {"Trusted", dbus::Value::boolean(true)},
        {"Blocked", dbus::Value::boolean(false)},
        {"RSSI", dbus::Value::i16(-65)},
    });

    BluetoothConfig cfg;
    cfg.system_bus_address = bus.address();

    std::string err;
    auto service = BluetoothService::create(cfg, &err);
    REQUIRE(service);
    Log log(service->events());

    // 1. Initial snapshot
    auto adapters = service->adapters();
    REQUIRE(adapters.size() == 1);
    CHECK_EQ(adapters[0].id, std::string("/org/bluez/hci0"));
    CHECK_EQ(adapters[0].address, std::string("00:11:22:33:44:55"));
    CHECK_EQ(adapters[0].alias, std::string("Controller 0"));
    CHECK(!adapters[0].powered);
    CHECK(!adapters[0].discovering);

    auto def_a = service->default_adapter();
    REQUIRE(def_a.has_value());
    CHECK_EQ(def_a->id, std::string("/org/bluez/hci0"));

    auto devices = service->devices();
    REQUIRE(devices.size() == 1);
    CHECK_EQ(devices[0].id, std::string("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"));
    CHECK_EQ(devices[0].mac, std::string("AA:BB:CC:DD:EE:FF"));
    CHECK_EQ(devices[0].name, std::string("Wireless Headphones"));
    CHECK_EQ(devices[0].alias, std::string("Headphones"));
    CHECK_EQ(devices[0].icon, std::string("audio-headphones"));
    CHECK(devices[0].paired);
    CHECK(!devices[0].connected);
    CHECK(devices[0].trusted);
    REQUIRE(devices[0].rssi.has_value());
    CHECK_EQ(*devices[0].rssi, -65);

    // Case-insensitive MAC lookup
    auto dev_lookup = service->device("aa:bb:cc:dd:ee:ff");
    REQUIRE(dev_lookup.has_value());
    CHECK_EQ(dev_lookup->name, std::string("Wireless Headphones"));

    // 2. Power control
    CHECK(service->set_powered(true).ok);
    auto a_ev = log.wait<AdapterChanged>([](const AdapterChanged& e) { return e.adapter.powered; });
    REQUIRE(a_ev);
    CHECK(service->default_adapter()->powered);

    CHECK(service->set_powered(false).ok);
    a_ev = log.wait<AdapterChanged>([](const AdapterChanged& e) { return !e.adapter.powered; });
    REQUIRE(a_ev);
    CHECK(!service->default_adapter()->powered);

    // 3. Discovery control
    CHECK(service->start_discovery().ok);
    a_ev = log.wait<AdapterChanged>([](const AdapterChanged& e) { return e.adapter.discovering; });
    REQUIRE(a_ev);
    CHECK(service->default_adapter()->discovering);

    CHECK(service->stop_discovery().ok);
    a_ev = log.wait<AdapterChanged>([](const AdapterChanged& e) { return !e.adapter.discovering; });
    REQUIRE(a_ev);
    CHECK(!service->default_adapter()->discovering);

    // 4. Device operations (connect, disconnect, pair)
    CHECK(service->connect_device("AA:BB:CC:DD:EE:FF").ok);
    auto d_ev = log.wait<DeviceChanged>([](const DeviceChanged& e) { return e.device.connected; });
    REQUIRE(d_ev);
    CHECK(service->device("AA:BB:CC:DD:EE:FF")->connected);

    CHECK(service->disconnect_device("aa:bb:cc:dd:ee:ff").ok);
    d_ev = log.wait<DeviceChanged>([](const DeviceChanged& e) { return !e.device.connected; });
    REQUIRE(d_ev);
    CHECK(!service->device("AA:BB:CC:DD:EE:FF")->connected);

    CHECK(service->pair_device("AA:BB:CC:DD:EE:FF").ok);
    bool saw_pair = false;
    for (auto& c : fake.calls()) {
        if (c.find("Pair") != std::string::npos) saw_pair = true;
    }
    CHECK(saw_pair);

    // 5. Dynamic DeviceFound event
    fake.add_device("/org/bluez/hci0/dev_11_22_33_44_55_66", {
        {"Address", dbus::Value::str("11:22:33:44:55:66")},
        {"Adapter", dbus::Value::obj("/org/bluez/hci0")},
        {"Name", dbus::Value::str("Bluetooth Keyboard")},
        {"Alias", dbus::Value::str("Keyboard")},
        {"Icon", dbus::Value::str("input-keyboard")},
        {"Paired", dbus::Value::boolean(false)},
        {"Connected", dbus::Value::boolean(false)},
        {"Trusted", dbus::Value::boolean(false)},
        {"Blocked", dbus::Value::boolean(false)},
    });

    auto found = log.wait<DeviceFound>([](const DeviceFound& e) { return e.device.mac == "11:22:33:44:55:66"; });
    REQUIRE(found);
    CHECK_EQ(found->device.alias, std::string("Keyboard"));
    CHECK_EQ(service->devices().size(), 2u);

    // 6. Device remove
    CHECK(service->remove_device("11:22:33:44:55:66").ok);
    auto rem = log.wait<DeviceRemoved>([](const DeviceRemoved& e) { return e.mac == "11:22:33:44:55:66"; });
    REQUIRE(rem);
    CHECK_EQ(service->devices().size(), 1u);
    CHECK(!service->device("11:22:33:44:55:66").has_value());

    // 7. Non-existent device error reporting
    CHECK(!service->connect_device("99:99:99:99:99:99").ok);
    CHECK(!service->disconnect_device("99:99:99:99:99:99").ok);
    CHECK(!service->remove_device("99:99:99:99:99:99").ok);
}

void test_bluetooth_system() {
    BluetoothConfig cfg;
    std::string err;
    auto service = BluetoothService::create(cfg, &err);
    if (!service) {
        // The scripted-BlueZ half above has already run and counted; a skip
        // here would hide its failures, so the system half just says why.
        std::printf("[test_bluetooth] system BlueZ part not run: %s\n", err.c_str());
        return;
    }
    // If BlueZ is running on system bus, verify basic querying works
    auto adapters = service->adapters();
    for (auto& a : adapters) {
        CHECK(!a.id.empty());
    }
    auto devices = service->devices();
    for (auto& d : devices) {
        CHECK(!d.mac.empty());
    }
}

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    test_bluetooth_fake();
    test_bluetooth_system();
    return bstest::finish("test_bluetooth");
}
