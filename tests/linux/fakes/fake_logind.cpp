#include "linux/fakes/fake_logind.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

using namespace brosys::dbus;

namespace bstest {

namespace {
constexpr const char* kPath = "/org/freedesktop/login1";
constexpr const char* kManager = "org.freedesktop.login1.Manager";
}  // namespace

FakeLogind::FakeLogind(const std::string& bus_address, FakeLogindConfig config) : config_(std::move(config)) {
    conn_ = Connection::open(BusKind::System, bus_address, "fake-logind", &error_);
    if (!conn_) return;

    auto m = std::make_shared<Interface>();
    m->name = kManager;
    for (auto& [method, answer] : config_.can) {
        std::string a = answer;
        m->methods.push_back({method, "", "s", {}, {"result"}, [a](const MethodCall&) {
                                  return MethodResult::ok({Value::str(a)});
                              }});
    }
    for (const char* action : {"Suspend", "Hibernate", "HybridSleep", "Reboot", "PowerOff"}) {
        std::string name = action;
        m->methods.push_back({name, "b", "", {"interactive"}, {}, [this, name](const MethodCall& c) {
                                  record(name + (c.args[0].as_bool() ? "(true)" : "(false)"));
                                  return MethodResult::ok();
                              }});
    }
    m->methods.push_back({"GetSessionByPID", "u", "o", {"pid"}, {"session"}, [this](const MethodCall&) {
                              if (!config_.session_for_pid)
                                  return MethodResult::error("org.freedesktop.login1.NoSessionForPID",
                                                             "PID does not belong to any known session");
                              return MethodResult::ok({Value::obj(kSessionPath)});
                          }});
    m->methods.push_back({"GetUser", "u", "o", {"uid"}, {"user"}, [](const MethodCall&) {
                              return MethodResult::ok({Value::obj(kUserPath)});
                          }});
    m->methods.push_back({"Inhibit", "ssss", "h", {"what", "who", "why", "mode"}, {"fd"}, [this](const MethodCall& c) {
                              int fds[2];
                              if (pipe2(fds, O_CLOEXEC) != 0) return MethodResult::error(kErrorFailed, "pipe");
                              {
                                  std::lock_guard<std::mutex> lock(mu_);
                                  inhibit_read_ends_.emplace_back(c.args[1].as_string(), fds[0]);
                              }
                              record("Inhibit(" + c.args[0].as_string() + "," + c.args[1].as_string() + "," +
                                     c.args[2].as_string() + "," + c.args[3].as_string() + ")");
                              return MethodResult::ok({Value::fd(UnixFd::adopt(fds[1]))});
                          }});
    m->signals.push_back({"PrepareForSleep", "b", {"start"}});
    m->signals.push_back({"PrepareForShutdown", "b", {"start"}});

    auto session = std::make_shared<Interface>();
    session->name = "org.freedesktop.login1.Session";
    session->methods.push_back({"Lock", "", "", {}, {}, [this](const MethodCall&) {
                                    record("Lock");
                                    return MethodResult::ok();
                                }});
    session->properties.push_back({"Id", "s", [] { return Value::str("31"); }, nullptr, "const"});

    auto user = std::make_shared<Interface>();
    user->name = "org.freedesktop.login1.User";
    user->properties.push_back({"Display", "(so)", [this] {
                                    if (!config_.display_session)
                                        return Value::structure({Value::str(""), Value::obj("/")});
                                    return Value::structure({Value::str("31"), Value::obj(kSessionPath)});
                                },
                                nullptr, "false"});

    if (!conn_->export_interface(kPath, m, &error_) || !conn_->export_interface(kSessionPath, session, &error_) ||
        !conn_->export_interface(kUserPath, user, &error_))
        return;
    if (conn_->request_name("org.freedesktop.login1", 0, &error_) != NameRequest::PrimaryOwner) {
        if (error_.empty()) error_ = "org.freedesktop.login1 is taken";
        return;
    }
    ok_ = true;
}

FakeLogind::~FakeLogind() {
    conn_.reset();
    for (auto& [who, fd] : inhibit_read_ends_) ::close(fd);
}

bool FakeLogind::emit_prepare_for_sleep(bool starting) {
    return conn_->emit_signal(kPath, kManager, "PrepareForSleep", {Value::boolean(starting)});
}

bool FakeLogind::emit_prepare_for_shutdown(bool starting) {
    return conn_->emit_signal(kPath, kManager, "PrepareForShutdown", {Value::boolean(starting)});
}

void FakeLogind::record(std::string call) {
    std::lock_guard<std::mutex> lock(mu_);
    calls_.push_back(std::move(call));
}

std::vector<std::string> FakeLogind::calls() const {
    std::lock_guard<std::mutex> lock(mu_);
    return calls_;
}

int FakeLogind::open_inhibitors(const std::string& who) const {
    std::lock_guard<std::mutex> lock(mu_);
    int open = 0;
    for (auto& [owner, fd] : inhibit_read_ends_) {
        if (owner != who) continue;
        pollfd p{fd, POLLIN, 0};
        // Every write end closed: POLLHUP on the read end.
        if (::poll(&p, 1, 0) == 0 || !(p.revents & POLLHUP)) ++open;
    }
    return open;
}

}  // namespace bstest
