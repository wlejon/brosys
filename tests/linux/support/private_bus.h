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
    // "unix:path=...": stable across restart(), as a systemd bus address is.
    const std::string& address() const { return address_; }
    // As dbus-daemon printed it, with the first instance's ",guid=..." (which
    // pins that instance: sd-bus refuses a restarted daemon at it).
    const std::string& address_with_guid() const { return guid_address_; }
    const std::string& dir() const { return dir_->path(); }
    // Environment for a child that should see this bus as both its session
    // and its system bus.
    Env env() const;
    void kill();  // stops the daemon (for disconnect tests)
    // Stops the daemon and starts a new one at the same address (a daemon
    // restart: every connection drops, every name is released).
    bool restart();

private:
    bool launch(std::string* address);

    std::unique_ptr<TempDir> dir_;
    std::string config_;
    Daemon daemon_;
    std::string address_, guid_address_;
    std::string error_;
};

}  // namespace bstest
