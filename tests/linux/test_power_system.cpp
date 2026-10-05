// PowerService on the real system bus, read-only: the device list and
// power source must match `upower -d` (no DisplayDevice, no line power, no
// absent batteries), every CanX must match logind's own answer via busctl,
// lock availability must match GetSessionByPID / the user's Display
// session, and a short-lived Idle inhibitor must show up in
// `systemd-inhibit --list` and disappear when released. Never performs an
// action.
#include "brosys/power.h"
#include "check.h"
#include "linux/support/proc.h"

#include <set>
#include <sstream>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

constexpr const char* kName = "test_power_system";

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

// busctl's text form of a string reply: s "challenge" -> challenge.
std::string busctl_string(const std::string& out) {
    auto a = out.find('"');
    auto b = out.rfind('"');
    return a == std::string::npos || b <= a ? std::string() : out.substr(a + 1, b - a - 1);
}

brosys::Availability expected(const std::string& answer) {
    if (answer == "yes") return brosys::Availability::Yes;
    if (answer == "challenge") return brosys::Availability::NeedsAuth;
    if (answer == "no" || answer == "na") return brosys::Availability::No;
    return brosys::Availability::Unknown;
}

void check_upower(const brosys::PowerState& s) {
    if (!bstest::have_program("upower")) {
        std::printf("note: upower CLI missing, device comparison skipped\n");
        return;
    }
    auto r = bstest::run({"upower", "-d"});
    if (r.exit_code != 0) {
        std::printf("note: upower -d failed (%s), UPower not running?\n", r.err.c_str());
        CHECK(s.devices.empty());
        return;
    }
    std::set<std::string> listed;
    std::istringstream in(r.out);
    std::string line, current, kind;
    bool present = true, on_battery = false, lid_present = false;
    auto flush = [&] {
        if (!current.empty() && current.find("DisplayDevice") == std::string::npos && kind != "line-power" && present)
            listed.insert(current);
        current.clear();
        kind.clear();
        present = true;
    };
    while (std::getline(in, line)) {
        std::string t = line.substr(std::min(line.find_first_not_of(' '), line.size()));
        if (line.rfind("Device: ", 0) == 0) {
            flush();
            current = line.substr(8);
            continue;
        }
        if (line.rfind("Daemon:", 0) == 0) flush();
        if (!current.empty() && kind.empty() && line.size() > 2 && line[0] == ' ' && line[1] == ' ' && line[2] != ' ' &&
            line.find(':') == std::string::npos)
            kind = t;
        if (t.rfind("present:", 0) == 0) present = t.find("yes") != std::string::npos;
        if (t.rfind("on-battery:", 0) == 0) on_battery = t.find("yes") != std::string::npos;
        if (t.rfind("lid-is-present:", 0) == 0) lid_present = t.find("yes") != std::string::npos;
    }
    flush();
    std::set<std::string> mine;
    for (auto& d : s.devices) mine.insert(d.id);
    CHECK(mine == listed);
    std::printf("power: %zu device(s) (upower lists %zu), source %s\n", mine.size(), listed.size(),
                brosys::to_string(s.source));
    CHECK(s.source == (on_battery ? brosys::PowerSource::Battery : brosys::PowerSource::AC));
    CHECK_EQ(s.lid_present, lid_present);
    if (!s.has_system_battery()) CHECK(!s.percent.has_value());
}

void check_logind(const brosys::PowerCapabilities& c) {
    if (!bstest::have_program("busctl")) {
        std::printf("note: busctl missing, capability comparison skipped\n");
        return;
    }
    auto ask = [](const char* method) {
        auto r = bstest::run({"busctl", "call", "org.freedesktop.login1", "/org/freedesktop/login1",
                              "org.freedesktop.login1.Manager", method});
        return r.exit_code == 0 ? busctl_string(r.out) : std::string();
    };
    struct {
        const char* method;
        brosys::Availability got;
    } rows[] = {{"CanSuspend", c.suspend},
                {"CanHibernate", c.hibernate},
                {"CanHybridSleep", c.hybrid_sleep},
                {"CanReboot", c.reboot},
                {"CanPowerOff", c.power_off}};
    for (auto& row : rows) {
        std::string answer = ask(row.method);
        std::printf("logind %s: %s -> %s\n", row.method, answer.c_str(), brosys::to_string(row.got));
        CHECK(row.got == expected(answer));
    }
    // Lock: this process's session, else the user's display session.
    auto r = bstest::run({"busctl", "call", "org.freedesktop.login1", "/org/freedesktop/login1",
                          "org.freedesktop.login1.Manager", "GetSessionByPID", "u", std::to_string(getpid())});
    bool has_session = r.exit_code == 0;
    if (!has_session) {
        auto u = bstest::run({"loginctl", "show-user", std::to_string(getuid()), "-p", "Display", "--value"});
        has_session = u.exit_code == 0 && !trim(u.out).empty();
    }
    std::printf("lock: %s (session found: %s)\n", brosys::to_string(c.lock), has_session ? "yes" : "no");
    CHECK(c.lock == (has_session ? brosys::Availability::Yes : brosys::Availability::No));
}

void check_inhibitor(brosys::PowerService& power) {
    if (!bstest::have_program("systemd-inhibit")) {
        std::printf("note: systemd-inhibit missing, inhibitor check skipped\n");
        return;
    }
    std::string who = "brosys-test-" + std::to_string(getpid());
    auto listed = [&] {
        auto r = bstest::run({"systemd-inhibit", "--list", "--no-pager"});
        return r.out.find(who) != std::string::npos;
    };
    CHECK(!listed());
    brosys::InhibitRequest req;
    req.what = brosys::inhibit::Idle;
    req.who = who;
    req.why = "brosys power test (released immediately)";
    req.mode = brosys::InhibitMode::Block;
    std::string err;
    auto inh = power.inhibit(req, &err);
    CHECK(inh != nullptr);
    if (!inh) {
        std::fprintf(stderr, "inhibit: %s\n", err.c_str());
        return;
    }
    CHECK(bstest::wait_until(listed, 5000ms, 50ms));
    inh.reset();
    CHECK(bstest::wait_until([&] { return !listed(); }, 5000ms, 50ms));
}

}  // namespace

int main() {
    std::string err;
    auto power = brosys::PowerService::create(brosys::PowerConfig(), &err);
    if (!power) {
        // Refusing is right only when busctl agrees that neither daemon is there.
        bool upower = bstest::run({"busctl", "status", "org.freedesktop.UPower"}).exit_code == 0;
        bool logind = bstest::run({"busctl", "status", "org.freedesktop.login1"}).exit_code == 0;
        if (upower || logind) {
            bstest::fail(__FILE__, __LINE__, "create() failed although busctl sees " +
                                                 std::string(upower ? "UPower" : "logind") + ": " + err);
            return bstest::finish(kName);
        }
        bstest::skip(kName, "no power backend on the system bus: " + err);
    }
    auto first = power->events().drain();
    CHECK(first.size() >= 2);
    CHECK(!first.empty() && std::holds_alternative<brosys::PowerChanged>(first[0]));
    check_upower(power->state());
    auto owner = bstest::run({"busctl", "status", "org.freedesktop.login1"});
    if (owner.exit_code != 0) {
        std::printf("note: logind not running, capability checks skipped\n");
    } else {
        check_logind(power->capabilities());
        check_inhibitor(*power);
    }
    return bstest::finish(kName);
}
