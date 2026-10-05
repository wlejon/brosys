// Windows power against the OS's own answers: GetSystemPowerStatus and WMI
// Win32_Battery for the state, `powercfg /a` and `whoami /priv` for the
// capabilities, CallNtPowerInformation / ShutdownBlockReasonQuery for
// inhibitors. Read-only: no action is ever requested except HybridSleep,
// which is refused before anything happens.
#include "check.h"
#include "win/support/oracle.h"

#include "brosys/power.h"

#include <windows.h>
#include <powrprof.h>

using namespace brosys;
using namespace std::chrono_literals;
using bstest::win::lower;

namespace {

HWND service_window() {
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"brosys.power", nullptr))) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) return h;
    }
    return nullptr;
}

template <class T>
std::vector<T> take(PowerEventQueue& q) {
    std::vector<T> out;
    for (auto& e : q.drain())
        if (auto* x = std::get_if<T>(&e)) out.push_back(*x);
    return out;
}

void test_state(PowerService& svc) {
    // The first snapshot is queued before create() returns.
    auto first = svc.events().drain();
    bool saw_state = false, saw_caps = false;
    for (auto& e : first) {
        saw_state |= std::holds_alternative<PowerChanged>(e);
        saw_caps |= std::holds_alternative<PowerCapabilitiesChanged>(e);
    }
    CHECK(saw_state);
    CHECK(saw_caps);

    PowerState s = svc.state();
    SYSTEM_POWER_STATUS sps{};
    REQUIRE(GetSystemPowerStatus(&sps));
    if (sps.ACLineStatus == 1) CHECK(s.source == PowerSource::AC);
    if (sps.ACLineStatus == 0) CHECK(s.source == PowerSource::Battery);
    bool os_has_battery = sps.BatteryFlag != 128 && sps.BatteryFlag != 255;
    CHECK_EQ(s.has_system_battery(), os_has_battery);

    std::vector<bstest::win::WmiRow> rows;
    std::string err;
    if (bstest::win::wmi_query(L"ROOT\\CIMV2", L"SELECT * FROM Win32_Battery", rows, &err)) {
        size_t system_batteries = 0;
        for (auto& d : s.devices)
            if (d.power_supply && d.kind == PowerDeviceKind::Battery) ++system_batteries;
        CHECK_EQ(system_batteries, rows.size());
        std::printf("power: %zu device(s), WMI Win32_Battery rows %zu\n", s.devices.size(), rows.size());
        if (!rows.empty() && !s.devices.empty()) {
            // Win32_Battery.EstimatedChargeRemaining is the same aggregate percent.
            int wmi_pct = std::atoi(rows[0]["EstimatedChargeRemaining"].c_str());
            CHECK(s.percent.has_value());
            if (s.percent) CHECK(std::abs(*s.percent - wmi_pct) <= 2.0);
            // Chemistry 6 = Lithium-ion, 8 = Lithium Polymer.
            int chem = std::atoi(rows[0]["Chemistry"].c_str());
            if (chem == 6) CHECK(s.devices[0].technology == BatteryTechnology::LithiumIon);
            if (chem == 8) CHECK(s.devices[0].technology == BatteryTechnology::LithiumPolymer);
        }
    } else {
        std::printf("note: %s (battery cross-check skipped)\n", err.c_str());
    }
    for (auto& d : s.devices) {
        std::printf("  %s kind=%s supply=%d state=%s tech=%s pct=%.1f model='%s'\n", d.id.c_str(), to_string(d.kind),
                    d.power_supply, to_string(d.state), to_string(d.technology), d.percent.value_or(-1),
                    d.model.c_str());
        if (d.percent) CHECK(*d.percent >= 0 && *d.percent <= 100);
    }
    if (!os_has_battery) {
        CHECK(!s.percent.has_value());
        CHECK(!s.time_to_empty_s.has_value());
    }
    SYSTEM_POWER_CAPABILITIES spc{};
    if (GetPwrCapabilities(&spc)) CHECK_EQ(s.lid_present, spc.LidPresent != FALSE);
}

void test_capabilities(PowerService& svc) {
    PowerCapabilities c = svc.capabilities();
    std::printf("caps: suspend=%s hibernate=%s hybrid=%s reboot=%s power_off=%s lock=%s\n", to_string(c.suspend),
                to_string(c.hibernate), to_string(c.hybrid_sleep), to_string(c.reboot), to_string(c.power_off),
                to_string(c.lock));
    auto priv = bstest::win::run_tool(L"whoami.exe /priv");
    REQUIRE(priv.exit_code == 0);
    bool has_priv = priv.out.find("SeShutdownPrivilege") != std::string::npos;
    // No machine policy here hides shutdown (NoClose), so the privilege decides.
    CHECK_EQ(c.reboot == Availability::Yes, has_priv);
    CHECK_EQ(c.power_off == Availability::Yes, has_priv);

    // powercfg /a lists available states first, then "not available".
    // powercfg exits nonzero on some machines (a VM with no sleep state at all); the parse
    // below, not the exit code, decides whether its output is usable.
    auto pc = bstest::win::run_tool(L"powercfg.exe /a");
    if (pc.exit_code != 0) std::printf("note: powercfg /a exited %d:\n%s\n", static_cast<int>(pc.exit_code), pc.out.c_str());
    std::string out = lower(pc.out);
    size_t split = out.find("not available");
    std::string avail = out.substr(0, split);
    bool s3 = avail.find("standby (s3)") != std::string::npos || avail.find("standby (s1)") != std::string::npos ||
              avail.find("standby (s2)") != std::string::npos;
    bool hib = avail.find("hibernate") != std::string::npos;
    if (split == std::string::npos) {
        std::printf("note: unrecognised powercfg output (localized?), sleep-state cross-check skipped\n");
    } else {
        CHECK_EQ(c.suspend == Availability::Yes, s3 && has_priv);
        CHECK_EQ(c.hibernate == Availability::Yes, hib && has_priv);
    }
    CHECK(c.hybrid_sleep == Availability::No);
    CHECK(c.lock == Availability::Yes);  // an interactive session without DisableLockWorkstation

    // Refused before anything happens.
    Result r = svc.request(PowerAction::HybridSleep);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

ULONG execution_state() {
    ULONG es = 0;
    CallNtPowerInformation(SystemExecutionState, nullptr, 0, &es, sizeof es);
    return es;
}

void test_inhibitors(PowerService& svc) {
    std::string err;
    CHECK(!svc.inhibit({inhibit::Sleep, "brosys-test", "delay", InhibitMode::Delay}, &err));
    CHECK(!err.empty());
    CHECK(!svc.inhibit({inhibit::LidSwitch, "brosys-test", "lid", InhibitMode::Block}, &err));

    ULONG before = execution_state();
    {
        auto inh = svc.inhibit({inhibit::Idle, "brosys-test", "test idle inhibitor", InhibitMode::Block}, &err);
        REQUIRE(inh);
        ULONG during = execution_state();
        std::printf("execution state: before=0x%lx during=0x%lx\n", before, during);
        CHECK((during & ES_DISPLAY_REQUIRED) != 0);
        CHECK((during & ES_SYSTEM_REQUIRED) != 0);
    }
    ULONG after = execution_state();
    std::printf("execution state after release=0x%lx\n", after);
    if (!(before & ES_DISPLAY_REQUIRED)) CHECK((after & ES_DISPLAY_REQUIRED) == 0);

    HWND hwnd = service_window();
    REQUIRE(hwnd);
    wchar_t reason[256] = {};
    DWORD len = 256;
    CHECK(!ShutdownBlockReasonQuery(hwnd, reason, &len));
    {
        auto a = svc.inhibit({inhibit::Shutdown, "brosys-test", "saving", InhibitMode::Block}, &err);
        auto b = svc.inhibit({inhibit::Shutdown, "brosys-test", "second", InhibitMode::Block}, &err);
        REQUIRE(a && b);
        len = 256;
        CHECK(ShutdownBlockReasonQuery(hwnd, reason, &len));
        CHECK(std::wstring(reason).find(L"saving") != std::wstring::npos);
        // A session-end query is refused while blocked, and reported.
        svc.events().drain();
        CHECK_EQ(SendMessageW(hwnd, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF), LRESULT(FALSE));
        SendMessageW(hwnd, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF);  // the logoff was cancelled
        auto prep = take<ShutdownPrepare>(svc.events());
        CHECK(prep.size() == 2 && prep[0].starting && !prep[1].starting);
        a.reset();
        len = 256;
        CHECK(ShutdownBlockReasonQuery(hwnd, reason, &len));  // b still holds it
    }
    CHECK(bstest::wait_until([&] {
        DWORD l = 256;
        return !ShutdownBlockReasonQuery(hwnd, reason, &l);
    }, 2000ms));
    CHECK_EQ(SendMessageW(hwnd, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF), LRESULT(TRUE));
    SendMessageW(hwnd, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF);
    svc.events().drain();
}

// The window's handlers, driven by the broadcast messages Windows sends
// (posted here: the machine is never actually suspended).
void test_broadcast_handling(PowerService& svc) {
    HWND hwnd = service_window();
    REQUIRE(hwnd);
    svc.events().drain();
    PostMessageW(hwnd, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    PostMessageW(hwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    PostMessageW(hwnd, WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0);  // one resume per suspend
    std::vector<SleepPrepare> sleeps;
    CHECK(bstest::wait_until([&] {
        auto more = take<SleepPrepare>(svc.events());
        sleeps.insert(sleeps.end(), more.begin(), more.end());
        return sleeps.size() >= 2;
    }, 3000ms));
    std::this_thread::sleep_for(100ms);
    auto more = take<SleepPrepare>(svc.events());
    sleeps.insert(sleeps.end(), more.begin(), more.end());
    CHECK_EQ(sleeps.size(), size_t(2));
    if (sleeps.size() == 2) CHECK(sleeps[0].starting && !sleeps[1].starting);
    // A status-change broadcast re-snapshots; an unchanged state pushes nothing.
    PostMessageW(hwnd, WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE, 0);
    std::this_thread::sleep_for(200ms);
    CHECK(take<PowerChanged>(svc.events()).empty());
}

}  // namespace

int main() {
    std::string err;
    auto svc = PowerService::create(PowerConfig{}, &err);
    if (!svc) {
        std::printf("create failed: %s\n", err.c_str());
        return 1;
    }
    test_state(*svc);
    test_capabilities(*svc);
    test_inhibitors(*svc);
    test_broadcast_handling(*svc);
    svc.reset();
    CHECK(service_window() == nullptr);
    return bstest::finish("test_win_power");
}
