#pragma once

#include "brosys/power.h"
#include "linux/dbus/connection.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace brosys {

class LinuxScreenSaverServer final : public ScreenSaverServer {
public:
    explicit LinuxScreenSaverServer(const ScreenSaverConfig& config);
    ~LinuxScreenSaverServer() override;

    bool start(std::string* error);

    uint32_t active_inhibitions() const override;
    bool is_active() const override;
    void set_active(bool active) override;
    Result simulate_user_activity() override;

private:
    struct InhibitEntry {
        uint32_t cookie = 0;
        std::string sender;
        std::string app_name;
        std::string reason;
        bool is_throttle = false;
        dbus::UnixFd logind_fd;
        uint64_t watch_id = 0;
    };

    std::shared_ptr<dbus::Interface> make_interface();
    dbus::MethodResult on_inhibit(const dbus::MethodCall& c, bool is_throttle);
    dbus::MethodResult on_uninhibit(const dbus::MethodCall& c);
    void release_cookie(uint32_t cookie);
    uint32_t allocate_cookie();

    ScreenSaverConfig config_;
    std::unique_ptr<dbus::Connection> session_conn_;
    std::unique_ptr<dbus::Connection> system_conn_;
    mutable std::mutex mu_;
    std::map<uint32_t, InhibitEntry> entries_;
    uint32_t next_cookie_ = 1;
    bool active_ = false;
};

}  // namespace brosys
