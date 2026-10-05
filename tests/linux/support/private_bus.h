// A private dbus-daemon for one test: every name may be owned, so it can
// stand in for the session bus or (with real daemons such as upowerd run
// against it) for the system bus. Nothing on the user's real buses is
// touched.
#pragma once

#include "linux/support/proc.h"

#include <memory>
#include <string>

namespace bstest {

class PrivateBus {
public:
    // Starts dbus-daemon; ok() is false (with error()) when it cannot.
    PrivateBus();
    ~PrivateBus();
    bool ok() const { return !address_.empty(); }
    const std::string& error() const { return error_; }
    const std::string& address() const { return address_; }  // "unix:path=..."
    const std::string& dir() const { return dir_->path(); }
    // Environment for a child that should see this bus as both its session
    // and its system bus.
    Env env() const;
    void kill();  // stops the daemon (for disconnect tests)

private:
    std::unique_ptr<TempDir> dir_;
    Daemon daemon_;
    std::string address_;
    std::string error_;
};

}  // namespace bstest
