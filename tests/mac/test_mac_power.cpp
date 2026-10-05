// macOS power against the OS's own answers: `pmset -g batt` (source, each
// power source's id / percent / state), ioreg's AppleClamshellState, `pmset
// -g` SleepDisabled + the console user for the capabilities, `pmset -g
// assertions` for inhibitors. Read-only toward the Mac: no action is ever
// requested except Hibernate / HybridSleep, which are refused before
// anything happens; the inhibitor assertions live for well under a second.
// The will-sleep / Delay handshake is driven through the backend's test seam
// with synthetic notifications (a real sleep is never requested).
#include "check.h"
#include "mac/power_mac.h"
#include "mac/support/oracle.h"

#include "brosys/power.h"

#include <IOKit/IOMessage.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>

using namespace brosys;
using namespace std::chrono_literals;
namespace om = bstest::mac;

namespace {

template <class T>
std::vector<T> take(PowerEventQueue& q) {
    std::vector<T> out;
    for (auto& e : q.drain())
        if (auto* x = std::get_if<T>(&e)) out.push_back(*x);
    return out;
}

void test_state(PowerService& svc) {
    // The first snapshot (state and capabilities) is queued before create() returns.
    bool saw_state = false, saw_caps = false;
    for (auto& e : svc.events().drain()) {
        saw_state |= std::holds_alternative<PowerChanged>(e);
        saw_caps |= std::holds_alternative<PowerCapabilitiesChanged>(e);
    }
    CHECK(saw_state);
    CHECK(saw_caps);

    om::Output batt = om::run("pmset -g batt");
    REQUIRE(batt.ok());
    PowerState s = svc.state();
    std::printf("pmset -g batt:\n%s", batt.out.c_str());
    if (om::contains(batt.out, "'AC Power'")) CHECK(s.source == PowerSource::AC);
    if (om::contains(batt.out, "'Battery Power'")) CHECK(s.source == PowerSource::Battery);
    if (om::contains(batt.out, "'UPS Power'")) CHECK(s.source == PowerSource::Battery);

    // " -InternalBattery-0 (id=22216803)\t100%; charged; 0:00 remaining present: true"
    size_t sources = 0;
    for (const auto& line : om::lines(batt.out)) {
        if (!om::starts_with(line, " -")) continue;
        ++sources;
        size_t idp = line.find("(id=");
        if (idp == std::string::npos) continue;
        std::string id = "iops:" + line.substr(idp + 4, line.find(')', idp) - idp - 4);
        const PowerDevice* d = nullptr;
        for (auto& x : s.devices)
            if (x.id == id) d = &x;
        CHECK(d != nullptr);
        if (!d) {
            std::printf("  no device %s\n", id.c_str());
            continue;
        }
        std::printf("  %s kind=%s supply=%d state=%s pct=%.1f model='%s' energy=%.2f/%.2f Wh\n", d->id.c_str(),
                    to_string(d->kind), d->power_supply, to_string(d->state), d->percent.value_or(-1),
                    d->model.c_str(), d->energy_wh.value_or(-1), d->energy_full_wh.value_or(-1));
        size_t tab = line.find('\t');
        REQUIRE(tab != std::string::npos);
        std::string rest = line.substr(tab + 1);
        int pct = std::atoi(rest.c_str());
        CHECK(d->percent.has_value());
        if (d->percent) CHECK(std::fabs(*d->percent - pct) <= 1.0);
        std::string st = om::trim(rest.substr(rest.find(';') + 1));
        st = st.substr(0, st.find(';'));
        std::printf("  pmset state '%s'\n", st.c_str());
        if (st == "charged") CHECK(d->state == BatteryState::FullyCharged);
        else if (st == "charging" || st == "finishing charge") CHECK(d->state == BatteryState::Charging);
        else if (st == "discharging") CHECK(d->state == BatteryState::Discharging || d->state == BatteryState::Empty);
        else if (st == "AC attached") CHECK(d->state == BatteryState::PendingCharge);
        if (om::contains(line, "InternalBattery")) {
            CHECK(d->power_supply);
            CHECK(d->kind == PowerDeviceKind::Battery);
            if (d->energy_wh && d->energy_full_wh) CHECK(*d->energy_wh <= *d->energy_full_wh * 1.05);
        }
    }
    CHECK_EQ(s.devices.size(), sources);
    CHECK_EQ(s.percent.has_value(), s.has_system_battery());

    // The lid: ioreg's AppleClamshellState on IOPMrootDomain (absent on desktops).
    om::Output lid = om::run("ioreg -r -k AppleClamshellState -d 1");
    auto clam = om::after(lid.out, "\"AppleClamshellState\" = ");
    CHECK_EQ(s.lid_present, clam.has_value());
    if (clam) CHECK_EQ(s.lid_closed, *clam == "Yes");
    std::printf("lid present=%d closed=%d (ioreg %s)\n", s.lid_present, s.lid_closed, clam.value_or("absent").c_str());
}

void test_capabilities(PowerService& svc) {
    PowerCapabilities c = svc.capabilities();
    std::printf("capabilities: suspend=%s hibernate=%s hybrid=%s reboot=%s poweroff=%s lock=%s\n",
                to_string(c.suspend), to_string(c.hibernate), to_string(c.hybrid_sleep), to_string(c.reboot),
                to_string(c.power_off), to_string(c.lock));
    // macOS has no user-requestable hibernate: hibernatemode only picks what sleep writes.
    CHECK(c.hibernate == Availability::No);
    CHECK(c.hybrid_sleep == Availability::No);

    bool console = om::console_user() == om::current_user();
    om::Output pm = om::run("pmset -g");
    bool sleep_disabled = om::after(pm.out, "SleepDisabled").value_or("0") == "1";
    std::printf("console user '%s' (we are '%s'), SleepDisabled=%d\n", om::console_user().c_str(),
                om::current_user().c_str(), sleep_disabled);
    CHECK(c.suspend == (console && !sleep_disabled ? Availability::Yes : Availability::No));

    void* login = dlopen("/System/Library/PrivateFrameworks/login.framework/Versions/Current/login", RTLD_LAZY);
    bool can_lock = login && dlsym(login, "SACLockScreenImmediate");
    CHECK(c.lock == (console && can_lock ? Availability::Yes : Availability::No));

    // Reboot / power off go through loginwindow by AppleEvent: whatever the
    // consent state, both answer the same, and never Unknown.
    CHECK(c.reboot == c.power_off);
    CHECK(c.reboot != Availability::Unknown);

    // Refused before anything happens.
    Result h = svc.request(PowerAction::Hibernate);
    CHECK(!h.ok);
    CHECK(!h.error.empty());
    CHECK(!svc.request(PowerAction::HybridSleep).ok);
}

// pmset -g assertions lines of this process naming `tag`.
std::vector<std::string> my_assertions(const std::string& tag) {
    std::vector<std::string> out;
    std::string me = "pid " + std::to_string(getpid()) + "(";
    for (const auto& line : om::lines(om::run("pmset -g assertions").out))
        if (om::contains(line, me) && om::contains(line, tag)) out.push_back(line);
    return out;
}

bool has_type(const std::vector<std::string>& lines, const std::string& type) {
    for (const auto& l : lines)
        if (om::contains(l, "] ") && om::contains(l, " " + type + " named:")) return true;
    return false;
}

void test_inhibitors(PowerService& svc) {
    const std::string tag = "brosys-test-" + std::to_string(getpid());
    std::string err;
    {
        auto idle = svc.inhibit({inhibit::Idle, "brosys test", tag + " idle", InhibitMode::Block}, &err);
        REQUIRE(idle != nullptr);
        auto a = my_assertions(tag + " idle");
        CHECK(has_type(a, "PreventUserIdleSystemSleep"));
        CHECK(has_type(a, "PreventUserIdleDisplaySleep"));
        CHECK(!has_type(a, "PreventSystemSleep"));
    }
    CHECK(my_assertions(tag + " idle").empty());  // released with the handle
    {
        auto sleep = svc.inhibit({inhibit::Sleep, "brosys test", tag + " sleep", InhibitMode::Block}, &err);
        REQUIRE(sleep != nullptr);
        auto a = my_assertions(tag + " sleep");
        CHECK(has_type(a, "PreventSystemSleep"));
        CHECK_EQ(a.size(), size_t(1));
    }
    CHECK(my_assertions(tag + " sleep").empty());

    // What macOS cannot do is refused with a reason, not faked.
    for (uint32_t what : {inhibit::Shutdown, inhibit::LidSwitch, inhibit::PowerKey, uint32_t(0)}) {
        err.clear();
        CHECK(svc.inhibit({what, "brosys test", tag, InhibitMode::Block}, &err) == nullptr);
        CHECK(!err.empty());
    }
    err.clear();
    CHECK(svc.inhibit({inhibit::Idle, "brosys test", tag, InhibitMode::Delay}, &err) == nullptr);
    CHECK(!err.empty());
    CHECK(my_assertions(tag).empty());
}

// The will-sleep handshake with synthetic IOKit notifications.
void test_sleep_gate(PowerService& svc) {
    namespace pt = mac::power_testing;
    svc.events().drain();

    // Can-sleep queries are answered at once (idle sleep is vetoed by assertions instead).
    pt::deliver_system_power(svc, kIOMessageCanSystemSleep, 0x11);
    CHECK(pt::acknowledged(svc) == std::vector<intptr_t>{0x11});

    // No Delay inhibitor: will-sleep is acknowledged at once, after SleepPrepare.
    pt::deliver_system_power(svc, kIOMessageSystemWillSleep, 0x22);
    CHECK((pt::acknowledged(svc) == std::vector<intptr_t>{0x11, 0x22}));
    auto prep = take<SleepPrepare>(svc.events());
    CHECK(prep.size() == 1 && prep[0].starting);
    pt::deliver_system_power(svc, kIOMessageSystemHasPoweredOn, 0);
    prep = take<SleepPrepare>(svc.events());
    CHECK(prep.size() == 1 && !prep[0].starting);

    // A Delay inhibitor holds the acknowledgement until it is released.
    std::string err;
    auto delay = svc.inhibit({inhibit::Sleep, "brosys test", "flush", InhibitMode::Delay}, &err);
    REQUIRE(delay != nullptr);
    pt::deliver_system_power(svc, kIOMessageSystemWillSleep, 0x33);
    prep = take<SleepPrepare>(svc.events());
    CHECK(prep.size() == 1 && prep[0].starting);
    CHECK_EQ(pt::acknowledged(svc).size(), size_t(2));
    delay.reset();
    CHECK((pt::acknowledged(svc) == std::vector<intptr_t>{0x11, 0x22, 0x33}));
    pt::deliver_system_power(svc, kIOMessageSystemHasPoweredOn, 0);

    // Two Delay inhibitors: the last release acknowledges.
    auto d1 = svc.inhibit({inhibit::Sleep, "a", "a", InhibitMode::Delay}, &err);
    auto d2 = svc.inhibit({inhibit::Sleep, "b", "b", InhibitMode::Delay}, &err);
    REQUIRE(d1 && d2);
    pt::deliver_system_power(svc, kIOMessageSystemWillSleep, 0x44);
    d1.reset();
    CHECK_EQ(pt::acknowledged(svc).size(), size_t(3));
    d2.reset();
    CHECK_EQ(pt::acknowledged(svc).back(), intptr_t(0x44));
    pt::deliver_system_power(svc, kIOMessageSystemHasPoweredOn, 0);
    // Powered-on without a preceding will-sleep reports nothing.
    svc.events().drain();
    pt::deliver_system_power(svc, kIOMessageSystemHasPoweredOn, 0);
    CHECK(take<SleepPrepare>(svc.events()).empty());
}

// A Delay inhibitor may outlive its service.
void test_teardown() {
    std::string err;
    auto svc = PowerService::create({}, &err);
    REQUIRE(svc != nullptr);
    auto delay = svc->inhibit({inhibit::Sleep, "brosys test", "outlives", InhibitMode::Delay}, &err);
    REQUIRE(delay != nullptr);
    svc.reset();
    delay.reset();
    for (int i = 0; i < 5; ++i) {
        auto s = PowerService::create({}, &err);
        CHECK(s != nullptr);
    }
}

}  // namespace

int main() {
    std::string err;
    auto svc = PowerService::create({}, &err);
    if (!svc) bstest::skip("test_mac_power", "PowerService::create: " + err);
    test_state(*svc);
    test_capabilities(*svc);
    test_inhibitors(*svc);
    test_sleep_gate(*svc);
    svc.reset();
    test_teardown();
    return bstest::finish("test_mac_power");
}
