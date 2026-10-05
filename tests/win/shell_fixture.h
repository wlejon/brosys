// A shell-mode tray host on a private desktop plus a real Shell_NotifyIcon
// client process on the same desktop. The user's desktop (and the Explorer
// tray on it) is never touched: Shell_NotifyIcon finds the tray per desktop.
#pragma once

#include "brosys/tray.h"
#include "check.h"
#include "win/tray_test_support.h"

#include <memory>
#include <sstream>
#include <string>

namespace bstest::win {

struct ShellFixture {
    PrivateDesktop desktop;
    LineProcess client;
    std::string window1;  // the client's first window, as the hex the tray ids use
    DWORD client_pid = 0;

    // Skips the test when no private desktop can be created here.
    void require_desktop(const char* test) {
        if (!desktop.handle())
            bstest::skip(test, "CreateDesktopW failed (" + std::to_string(desktop.error()) +
                                   "): no interactive window station to isolate a shell on");
    }

    bool start_client(std::string* why) {
        if (!client.start(exe_dir() + L"brosys_tray_client.exe", desktop.station_path(), why)) return false;
        auto ready = client.wait_prefix("ready ", 0);
        if (!ready) {
            *why = "client never reported ready";
            return false;
        }
        std::istringstream in(ready->substr(6));
        in >> client_pid >> window1;
        return true;
    }

    std::unique_ptr<brosys::TrayHost> create_host(brosys::TrayMode mode, std::string* error) {
        std::unique_ptr<brosys::TrayHost> host;
        desktop.run_on([&] {
            brosys::TrayConfig cfg;
            cfg.mode = mode;
            host = brosys::TrayHost::create(cfg, error);
        });
        return host;
    }

    // Owner pid of Shell_TrayWnd as seen from the private desktop (0: none).
    DWORD private_tray_owner() {
        DWORD pid = 0;
        desktop.run_on([&] {
            if (HWND w = FindWindowW(L"Shell_TrayWnd", nullptr)) GetWindowThreadProcessId(w, &pid);
        });
        return pid;
    }

    // What FindWindow(Shell_TrayWnd) resolves to inside the client: "<hwnd> <pid>".
    std::string client_find() {
        size_t m = client.mark();
        client.command("find");
        auto found = client.wait_prefix("find ", m, std::chrono::seconds(3));
        return found ? found->substr(5) : std::string("timeout");
    }

    // The gate before any Shell_NotifyIcon call: the client's lookup resolves
    // to a Shell_TrayWnd of this process, so the user's tray cannot see it.
    bool client_sees_this_process() {
        return private_tray_owner() == GetCurrentProcessId() &&
               client_find().ends_with(" " + std::to_string(GetCurrentProcessId()));
    }

    std::string item_id(unsigned uid) const { return "hwnd:" + window1 + ":" + std::to_string(uid); }
};

inline std::string hex(unsigned long long v) {
    char b[32];
    std::snprintf(b, sizeof b, "%llx", v);
    return b;
}

// "cb <window> <wparam> <lparam>" with the given values.
inline std::string cb_line(int window, unsigned long long wp, unsigned long long lp) {
    return "cb " + std::to_string(window) + " " + hex(wp) + " " + hex(lp);
}

}  // namespace bstest::win
