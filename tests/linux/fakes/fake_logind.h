// A scripted org.freedesktop.login1 on a private bus: configurable CanX
// answers, session lookup (by pid, else the user's display session),
// Inhibit returning a real pipe fd (whose closing the test can observe),
// recorded action calls, and PrepareForSleep / PrepareForShutdown signals.
#pragma once

#include "linux/dbus/connection.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bstest {

struct FakeLogindConfig {
    std::map<std::string, std::string> can{{"CanSuspend", "yes"},    {"CanHibernate", "challenge"},
                                           {"CanHybridSleep", "na"}, {"CanReboot", "no"},
                                           {"CanPowerOff", "yes"}};
    bool session_for_pid = true;   // GetSessionByPID answers (else NoSessionForPID)
    bool display_session = true;   // the user has a display session (User.Display)
};

class FakeLogind {
public:
    FakeLogind(const std::string& bus_address, FakeLogindConfig config);
    ~FakeLogind();
    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    bool emit_prepare_for_sleep(bool starting);
    bool emit_prepare_for_shutdown(bool starting);

    // "Suspend(true)", "Lock", "Inhibit(sleep:shutdown,who,why,delay)", ...
    std::vector<std::string> calls() const;
    // Inhibitor fds handed out to `who` whose client side is still open
    // (upowerd takes a delay inhibitor of its own).
    int open_inhibitors(const std::string& who) const;

    static constexpr const char* kSessionPath = "/org/freedesktop/login1/session/_31";
    static constexpr const char* kUserPath = "/org/freedesktop/login1/user/_1000";

private:
    void record(std::string call);

    FakeLogindConfig config_;
    mutable std::mutex mu_;
    std::vector<std::string> calls_;
    std::vector<std::pair<std::string, int>> inhibit_read_ends_;  // who, read end
    bool ok_ = false;
    std::string error_;
    std::unique_ptr<brosys::dbus::Connection> conn_;  // last
};

}  // namespace bstest
