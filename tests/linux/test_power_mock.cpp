// PowerService against a real upowerd running on a private bus over a
// umockdev sysfs (AC adapter, system batteries, a Bluetooth mouse battery),
// plus a scripted logind on the same bus. Every UPower value the service
// reports is cross-checked with `upower -d` pointed at that bus; sysfs
// changes (unplugging AC, discharging, adding / hiding / removing a battery,
// upowerd restarts) must arrive as PowerChanged snapshots. logind:
// CanX -> Availability, lock via the session, interactive actions,
// Inhibit fds, PrepareForSleep / PrepareForShutdown, logind restarts.
#include "brosys/power.h"
#include "check.h"
#include "linux/fakes/event_log.h"
#include "linux/fakes/fake_logind.h"
#include "linux/support/private_bus.h"

#ifdef BROSYS_HAVE_UMOCKDEV
#include <umockdev.h>
#endif

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unistd.h>

using namespace std::chrono_literals;
using brosys::PowerChanged;
using brosys::PowerCapabilitiesChanged;
using brosys::PowerDevice;
using brosys::PowerDeviceKind;
using brosys::PowerState;
using brosys::Availability;

namespace {

constexpr const char* kName = "test_power_mock";

#ifndef BROSYS_HAVE_UMOCKDEV
void run_test() { bstest::skip(kName, "built without umockdev (pkg-config umockdev-1.0, libumockdev-dev)"); }
#else

std::string find_upowerd() {
    for (const char* p : {"/usr/libexec/upowerd", "/usr/lib/upower/upowerd", "/usr/libexec/upower/upowerd"})
        if (access(p, X_OK) == 0) return p;
    return {};
}

struct Testbed {
    UMockdevTestbed* tb = umockdev_testbed_new();
    ~Testbed() { g_object_unref(tb); }
    std::string root() const {
        gchar* r = umockdev_testbed_get_root_dir(tb);
        std::string s = r ? r : "";
        g_free(r);
        return s;
    }
    std::string add(const char* subsystem, const char* name, const std::string& parent,
                    std::vector<std::string> attrs, std::vector<std::string> props) {
        // umockdev_testbed_add_devicev takes NULL-terminated name/value arrays.
        std::vector<const gchar*> a, p;
        for (auto& s : attrs) a.push_back(s.c_str());
        a.push_back(nullptr);
        for (auto& s : props) p.push_back(s.c_str());
        p.push_back(nullptr);
        gchar* path = umockdev_testbed_add_devicev(tb, subsystem, name, parent.empty() ? nullptr : parent.c_str(),
                                                   const_cast<gchar**>(a.data()), const_cast<gchar**>(p.data()));
        std::string out = path ? path : "";
        g_free(path);
        return out;
    }
    void set(const std::string& dev, const char* attr, const std::string& value) {
        umockdev_testbed_set_attribute(tb, dev.c_str(), attr, value.c_str());
    }
    void change(const std::string& dev) { umockdev_testbed_uevent(tb, dev.c_str(), "change"); }
};

struct Upowerd {
    bstest::Daemon daemon;
    Upowerd(const std::string& upowerd, const Testbed& bed, const bstest::PrivateBus& bus, const std::string& history) {
        bstest::Env env = bus.env();
        env["UMOCKDEV_DIR"] = bed.root();
        env["UPOWER_HISTORY_DIR"] = history;
        daemon = bstest::Daemon({"umockdev-wrapper", upowerd}, env, false);
    }
};

bool wait_owner(brosys::dbus::Connection& c, const std::string& name, bool present) {
    return bstest::wait_until([&] { return c.get_name_owner(name).empty() != present; }, 15000ms, 50ms);
}

// `upower -d` against the private bus: path -> {kind line, power supply, percentage}.
struct UpowerCli {
    struct Dev {
        std::string kind;
        bool power_supply = false;
        bool present = true;
        double percent = -1;
        bool time_to_empty = false;
        bool time_to_full = false;
    };
    std::map<std::string, Dev> devices;
    bool on_battery = false;
    double display_percent = -1;
};

UpowerCli upower_dump(const bstest::PrivateBus& bus) {
    UpowerCli out;
    auto r = bstest::run({"upower", "-d"}, bus.env());
    std::istringstream in(r.out);
    std::string line, current;
    bool in_kind = false;
    while (std::getline(in, line)) {
        if (line.rfind("Device: ", 0) == 0) {
            current = line.substr(8);
            out.devices[current];
            in_kind = false;
            continue;
        }
        if (line.rfind("Daemon:", 0) == 0) current.clear();
        auto trimmed = line.substr(std::min(line.find_first_not_of(' '), line.size()));
        if (trimmed.rfind("on-battery:", 0) == 0) out.on_battery = trimmed.find("yes") != std::string::npos;
        if (current.empty()) continue;
        auto& d = out.devices[current];
        // The kind is the first line indented by exactly two spaces after the header fields.
        if (line.size() > 2 && line[0] == ' ' && line[1] == ' ' && line[2] != ' ' && line.find(':') == std::string::npos &&
            !in_kind) {
            d.kind = trimmed;
            in_kind = true;
        }
        if (trimmed.rfind("power supply:", 0) == 0) d.power_supply = trimmed.find("yes") != std::string::npos;
        if (trimmed.rfind("present:", 0) == 0) d.present = trimmed.find("yes") != std::string::npos;
        if (trimmed.rfind("time to empty:", 0) == 0) d.time_to_empty = true;
        if (trimmed.rfind("time to full:", 0) == 0) d.time_to_full = true;
        if (trimmed.rfind("percentage:", 0) == 0) {
            d.percent = std::atof(trimmed.substr(11).c_str());
            if (current.find("DisplayDevice") != std::string::npos) out.display_percent = d.percent;
        }
    }
    return out;
}

const PowerDevice* find_dev(const PowerState& s, const std::string& suffix) {
    for (auto& d : s.devices)
        if (d.id.size() >= suffix.size() && d.id.compare(d.id.size() - suffix.size(), suffix.size(), suffix) == 0)
            return &d;
    return nullptr;
}

// Every device the service lists is one `upower -d` lists as present and
// not line power, and vice versa (DisplayDevice excluded).
void compare_with_cli(const PowerState& s, const bstest::PrivateBus& bus) {
    auto cli = upower_dump(bus);
    size_t expected = 0;
    for (auto& [path, d] : cli.devices) {
        if (path.find("DisplayDevice") != std::string::npos || d.kind == "line-power" || !d.present) continue;
        ++expected;
        const PowerDevice* mine = nullptr;
        for (auto& x : s.devices)
            if (x.id == path) mine = &x;
        CHECK(mine != nullptr);
        if (!mine) {
            std::fprintf(stderr, "  missing %s (%s)\n", path.c_str(), d.kind.c_str());
            continue;
        }
        CHECK_EQ(mine->power_supply, d.power_supply);
        CHECK_EQ(mine->time_to_empty_s.has_value(), d.time_to_empty);
        CHECK_EQ(mine->time_to_full_s.has_value(), d.time_to_full);
        CHECK(mine->percent && std::fabs(*mine->percent - d.percent) < 0.6);  // upower -d rounds
        CHECK_EQ(std::string(brosys::to_string(mine->kind)), d.kind == "gaming-input" ? std::string("gamepad") : d.kind);
    }
    CHECK_EQ(s.devices.size(), expected);
    CHECK_EQ(s.source == brosys::PowerSource::Battery, cli.on_battery);
}

void run_test() {
    // (Checked before anything starts: skip() exits without unwinding.)
    std::string upowerd = find_upowerd();
    if (upowerd.empty()) bstest::skip(kName, "upowerd not installed");
    if (!bstest::have_program("umockdev-wrapper")) bstest::skip(kName, "umockdev-wrapper not installed");
    if (!bstest::have_program("upower")) bstest::skip(kName, "the upower client is not installed");
    bstest::PrivateBus bus;
    if (!bus.ok()) bstest::skip(kName, bus.error());

    Testbed bed;
    std::string ac = bed.add("power_supply", "AC", "", {"type", "Mains", "online", "1"}, {});
    std::string bat0 = bed.add("power_supply", "BAT0", "",
                               {"type", "Battery", "present", "1", "status", "Charging", "energy_full", "60000000",
                                "energy_full_design", "80000000", "energy_now", "48000000", "power_now", "10000000",
                                "voltage_now", "12000000", "technology", "Li-ion", "manufacturer", "ACME",
                                "model_name", "PowerCell", "serial_number", "SN-1"},
                               {"POWER_SUPPLY_ONLINE", "1"});
    // A Bluetooth HID mouse: UPower finds the kind from the sibling input
    // device. (Parents are given as path prefixes: umockdev resolves a
    // `parent` argument only inside a preloaded process.)
    bed.add("bluetooth", "usb1/bluetooth/hci0/hci0:01", "", {}, {});
    bed.add("input", "usb1/bluetooth/hci0/hci0:01/input2/mouse3", "", {},
            {"DEVNAME", "input/mouse3", "ID_INPUT_MOUSE", "1"});
    bed.add("power_supply", "usb1/bluetooth/hci0/hci0:01/1/power_supply/hid-00:22:33:44:55:66-battery", "",
            {"type", "Battery", "scope", "Device", "present", "1", "online", "1", "status", "Discharging", "capacity",
             "30", "model_name", "Fancy BT mouse"},
            {});
    REQUIRE(!ac.empty() && !bat0.empty());

    auto logind = std::make_unique<bstest::FakeLogind>(bus.address(), bstest::FakeLogindConfig());
    REQUIRE(logind->ok());
    bstest::TempDir history("brosys-upower-history");
    auto daemon = std::make_unique<Upowerd>(upowerd, bed, bus, history.path());

    std::string err;
    auto watcher = brosys::dbus::Connection::open(brosys::dbus::BusKind::System, bus.address(), "watcher", &err);
    REQUIRE(watcher);
    REQUIRE(wait_owner(*watcher, "org.freedesktop.UPower", true));

    brosys::PowerConfig cfg;
    cfg.system_bus_address = bus.address();
    auto power = brosys::PowerService::create(cfg, &err);
    REQUIRE(power);
    bstest::EventLog<brosys::PowerEvent> log(power->events());

    // ---- the initial snapshot is queued before create() returns
    auto first = power->events().drain();
    REQUIRE(first.size() >= 2);
    REQUIRE(std::holds_alternative<PowerChanged>(first[0]));
    CHECK(std::holds_alternative<PowerCapabilitiesChanged>(first[1]));
    PowerState s = std::get<PowerChanged>(first[0]).state;
    CHECK(s == power->state());
    CHECK(s.source == brosys::PowerSource::AC);
    CHECK(!s.lid_present);
    CHECK_EQ(s.devices.size(), size_t(2));  // BAT0 + mouse; no AC, no display device
    const PowerDevice* b0 = find_dev(s, "battery_BAT0");
    REQUIRE(b0 != nullptr);
    CHECK(b0->kind == PowerDeviceKind::Battery && b0->power_supply);
    CHECK(b0->state == brosys::BatteryState::Charging);
    CHECK(b0->technology == brosys::BatteryTechnology::LithiumIon);
    CHECK(b0->percent && std::fabs(*b0->percent - 80.0) < 0.01);
    CHECK(b0->energy_full_wh && std::fabs(*b0->energy_full_wh - 60.0) < 0.01);
    CHECK(b0->energy_full_design_wh && std::fabs(*b0->energy_full_design_wh - 80.0) < 0.01);
    CHECK(b0->energy_rate_w && std::fabs(*b0->energy_rate_w - 10.0) < 0.01);
    CHECK_EQ(b0->vendor, std::string("ACME"));
    CHECK_EQ(b0->model, std::string("PowerCell"));
    CHECK_EQ(b0->serial, std::string("SN-1"));
    const PowerDevice* mouse = nullptr;
    for (auto& d : s.devices)
        if (d.kind == PowerDeviceKind::Mouse) mouse = &d;
    REQUIRE(mouse != nullptr);
    CHECK(!mouse->power_supply);
    CHECK(mouse->percent && std::fabs(*mouse->percent - 30.0) < 0.01);
    CHECK(s.has_system_battery());
    CHECK(s.percent && std::fabs(*s.percent - 80.0) < 0.01);  // the mouse does not count
    compare_with_cli(s, bus);

    auto caps = power->capabilities();
    CHECK(caps.suspend == Availability::Yes);
    CHECK(caps.hibernate == Availability::NeedsAuth);
    CHECK(caps.hybrid_sleep == Availability::No);  // "na"
    CHECK(caps.reboot == Availability::No);
    CHECK(caps.power_off == Availability::Yes);
    CHECK(caps.lock == Availability::Yes);
    log.skip_all();

    // ---- unplug AC, discharge
    bed.set(ac, "online", "0");
    bed.set(bat0, "status", "Discharging");
    bed.set(bat0, "energy_now", "30000000");
    bed.change(ac);
    bed.change(bat0);
    auto ev = log.wait<PowerChanged>([](const PowerChanged& c) {
        auto* b = find_dev(c.state, "battery_BAT0");
        return c.state.source == brosys::PowerSource::Battery && b && b->percent && std::fabs(*b->percent - 50) < 0.01;
    });
    CHECK(ev.has_value());
    if (ev) {
        auto* b = find_dev(ev->state, "battery_BAT0");
        CHECK(b && b->state == brosys::BatteryState::Discharging);
        CHECK(!ev->state.time_to_full_s.has_value());
        compare_with_cli(ev->state, bus);  // includes whether UPower has a time-to-empty estimate yet
    }

    // ---- a second system battery appears: aggregate over both (and not the mouse)
    std::string bat1 = bed.add("power_supply", "BAT1", "",
                               {"type", "Battery", "present", "1", "status", "Discharging", "energy_full", "40000000",
                                "energy_full_design", "40000000", "energy_now", "40000000", "power_now", "10000000",
                                "voltage_now", "12000000"},
                               {});
    ev = log.wait<PowerChanged>([](const PowerChanged& c) { return find_dev(c.state, "battery_BAT1") != nullptr; });
    REQUIRE(ev.has_value());
    // 30 Wh + 40 Wh of 60 Wh + 40 Wh; UPower's own display device agrees.
    CHECK(ev->state.percent && std::fabs(*ev->state.percent - 70.0) < 0.01);
    auto cli = upower_dump(bus);
    CHECK(std::fabs(cli.display_percent - 70.0) < 0.6);
    compare_with_cli(power->state(), bus);

    // ---- BAT1 not present: excluded; then removed altogether
    bed.set(bat1, "present", "0");
    bed.change(bat1);
    ev = log.wait<PowerChanged>([](const PowerChanged& c) { return find_dev(c.state, "battery_BAT1") == nullptr; });
    CHECK(ev.has_value());
    if (ev) CHECK(ev->state.percent && std::fabs(*ev->state.percent - 50.0) < 0.01);
    umockdev_testbed_remove_device(bed.tb, bat1.c_str());
    CHECK(bstest::wait_until([&] { return upower_dump(bus).devices.count("/org/freedesktop/UPower/devices/battery_BAT1") == 0; },
                             10000ms, 100ms));
    compare_with_cli(power->state(), bus);

    // ---- logind: actions, lock, inhibitors, sleep / shutdown signals
    CHECK(power->request(brosys::PowerAction::Suspend).ok);
    CHECK(power->request(brosys::PowerAction::Lock).ok);
    CHECK(power->request(brosys::PowerAction::PowerOff).ok);
    {
        brosys::InhibitRequest req;
        req.what = brosys::inhibit::Sleep | brosys::inhibit::Shutdown;
        req.who = "brosys-test";
        req.why = "testing";
        req.mode = brosys::InhibitMode::Delay;
        auto inh = power->inhibit(req, &err);
        CHECK(inh != nullptr);
        CHECK_EQ(logind->open_inhibitors("brosys-test"), 1);
        inh.reset();
        CHECK(bstest::wait_until([&] { return logind->open_inhibitors("brosys-test") == 0; }, 2000ms));
        brosys::InhibitRequest none;
        none.what = 0;
        CHECK(power->inhibit(none, &err) == nullptr);
    }
    auto calls = logind->calls();
    auto has = [&](const std::string& c) { return std::find(calls.begin(), calls.end(), c) != calls.end(); };
    CHECK(has("Suspend(true)"));
    CHECK(has("Lock"));
    CHECK(has("PowerOff(true)"));
    CHECK(has("Inhibit(sleep:shutdown,brosys-test,testing,delay)"));

    CHECK(logind->emit_prepare_for_sleep(true));
    auto sp = log.wait<brosys::SleepPrepare>();
    CHECK(sp && sp->starting);
    CHECK(logind->emit_prepare_for_sleep(false));
    sp = log.wait<brosys::SleepPrepare>();
    CHECK(sp && !sp->starting);
    CHECK(logind->emit_prepare_for_shutdown(true));
    auto shp = log.wait<brosys::ShutdownPrepare>();
    CHECK(shp && shp->starting);

    // ---- logind goes away: everything unknown
    log.skip_all();
    logind.reset();
    auto pc = log.wait<PowerCapabilitiesChanged>(
        [](const PowerCapabilitiesChanged& c) { return c.capabilities.suspend == Availability::Unknown; });
    CHECK(pc && pc->capabilities.lock == Availability::Unknown && pc->capabilities.power_off == Availability::Unknown);
    CHECK(!power->request(brosys::PowerAction::Lock).ok);

    // ---- it comes back with other answers; this process has no session of
    // its own, so lock falls back to the user's display session
    bstest::FakeLogindConfig second;
    second.can["CanSuspend"] = "challenge";
    second.can["CanReboot"] = "yes";
    second.session_for_pid = false;
    logind = std::make_unique<bstest::FakeLogind>(bus.address(), second);
    REQUIRE(logind->ok());
    pc = log.wait<PowerCapabilitiesChanged>(
        [](const PowerCapabilitiesChanged& c) { return c.capabilities.suspend == Availability::NeedsAuth; });
    REQUIRE(pc.has_value());
    CHECK(pc->capabilities.reboot == Availability::Yes);
    CHECK(pc->capabilities.lock == Availability::Yes);
    CHECK(pc->capabilities == power->capabilities());
    CHECK(power->request(brosys::PowerAction::Lock).ok);
    auto calls2 = logind->calls();
    CHECK(std::find(calls2.begin(), calls2.end(), "Lock") != calls2.end());

    // ---- and once more without any session: lock is unavailable
    logind.reset();
    CHECK(log.wait<PowerCapabilitiesChanged>(
        [](const PowerCapabilitiesChanged& c) { return c.capabilities.suspend == Availability::Unknown; }));
    bstest::FakeLogindConfig third;
    third.session_for_pid = false;
    third.display_session = false;
    logind = std::make_unique<bstest::FakeLogind>(bus.address(), third);
    REQUIRE(logind->ok());
    pc = log.wait<PowerCapabilitiesChanged>(
        [](const PowerCapabilitiesChanged& c) { return c.capabilities.suspend == Availability::Yes; });
    CHECK(pc && pc->capabilities.lock == Availability::No);

    // ---- upowerd restarts: devices vanish, then come back
    daemon.reset();
    ev = log.wait<PowerChanged>([](const PowerChanged& c) { return c.state.devices.empty(); });
    CHECK(ev && ev->state.source == brosys::PowerSource::Unknown);
    daemon = std::make_unique<Upowerd>(upowerd, bed, bus, history.path());
    ev = log.wait<PowerChanged>([](const PowerChanged& c) { return c.state.devices.size() == 2; }, 20000ms);
    CHECK(ev.has_value());
    if (ev) compare_with_cli(ev->state, bus);

    // ---- the system bus daemon itself restarts (same address). Everything
    // goes unknown; the service reconnects; logind (the fake) re-owns its
    // name as the real one would be restarted with the bus; upowerd loses
    // its bus and is started again, as systemd would.
    log.skip_all();
    REQUIRE(bus.restart());
    pc = log.wait<PowerCapabilitiesChanged>(
        [](const PowerCapabilitiesChanged& c) { return c.capabilities.suspend == Availability::Unknown; });
    CHECK(pc.has_value());
    daemon.reset();  // (whether upowerd exits by itself without its bus is upowerd's business)
    daemon = std::make_unique<Upowerd>(upowerd, bed, bus, history.path());
    CHECK(bstest::wait_until([&] {
        return power->state().devices.size() == 2 && power->capabilities().suspend == Availability::Yes;
    }, 20000ms));
    CHECK(power->capabilities().lock == Availability::No);  // the third logind's answers
    compare_with_cli(power->state(), bus);
    // Both arrived as events too.
    bool saw_devices = false, saw_caps = false;
    for (auto& e : log.unseen()) {
        if (auto* c = std::get_if<PowerChanged>(&e)) saw_devices |= c->state.devices.size() == 2;
        if (auto* c = std::get_if<PowerCapabilitiesChanged>(&e)) saw_caps |= c->capabilities.suspend == Availability::Yes;
    }
    CHECK(saw_devices && saw_caps);
    CHECK(power->request(brosys::PowerAction::Suspend).ok);  // calls go out on the new connection
    CHECK(power->inhibit([] {
        brosys::InhibitRequest r;
        r.what = brosys::inhibit::Sleep;
        r.who = "after-restart";
        return r;
    }(), &err) != nullptr);
}

// Neither UPower nor logind on the bus: no service, as NetworkService
// without NetworkManager.
void test_no_backend() {
    bstest::PrivateBus empty;
    REQUIRE(empty.ok());
    brosys::PowerConfig cfg;
    cfg.system_bus_address = empty.address();
    std::string err;
    CHECK(brosys::PowerService::create(cfg, &err) == nullptr);
    CHECK(err.find("UPower") != std::string::npos && err.find("logind") != std::string::npos);
    // logind alone is enough.
    bstest::FakeLogind logind(empty.address(), bstest::FakeLogindConfig());
    REQUIRE(logind.ok());
    auto power = brosys::PowerService::create(cfg, &err);
    REQUIRE(power);
    CHECK(power->state().devices.empty());
    CHECK(power->capabilities().suspend == Availability::Yes);
}
#endif  // BROSYS_HAVE_UMOCKDEV

}  // namespace

int main(int, char** argv) {
#ifdef BROSYS_HAVE_UMOCKDEV
    // umockdev's uevent injection resolves sysfs paths through its preload
    // library, so the test itself runs under umockdev-wrapper (as UPower's
    // own integration tests do); upowerd and the upower CLI inherit it.
    if (!std::getenv("BROSYS_TEST_UNDER_UMOCKDEV")) {
        std::string wrapper = bstest::find_program("umockdev-wrapper");
        if (wrapper.empty()) bstest::skip(kName, "umockdev-wrapper not installed");
        char self[4096] = {};
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n <= 0) bstest::skip(kName, "cannot resolve /proc/self/exe");
        setenv("BROSYS_TEST_UNDER_UMOCKDEV", "1", 1);
        std::fflush(stdout);
        char* args[] = {wrapper.data(), self, nullptr};
        execv(wrapper.c_str(), args);
        std::fprintf(stderr, "execv %s failed\n", wrapper.c_str());
        return 1;
    }
#endif
    (void)argv;
    run_test();
#ifdef BROSYS_HAVE_UMOCKDEV
    test_no_backend();
#endif
    return bstest::finish(kName);
}
