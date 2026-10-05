// macOS power capabilities and actions.
//
//   suspend      IOPMSleepSystem; the root domain accepts it from the console
//                user (or root) when sleep is not disabled (pmset disablesleep).
//   hibernate,   not separately requestable: hibernatemode / standby are
//   hybrid sleep pmset policies applied to ordinary sleep.
//   reboot,      AppleEvents to loginwindow (Apple's QA1134), which apps may
//   power off    still cancel; macOS asks the user before letting a process
//                send them (Automation consent), so the availability is the
//                answer of AEDeterminePermissionToAutomateTarget.
//   lock         SACLockScreenImmediate from the private login.framework (the
//                Control Center "Lock Screen" item); there is no public API.
#include "mac/cf.h"
#include "mac/power_mac.h"

#include <CoreServices/CoreServices.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <dlfcn.h>
#include <unistd.h>

#include <cstring>

namespace brosys::mac {

namespace {

constexpr const char* kLoginWindow = "com.apple.loginwindow";
constexpr const char* kLoginFramework = "/System/Library/PrivateFrameworks/login.framework/Versions/Current/login";

// The process belongs to the user logged in at the console (or is root).
bool console_session() {
    if (getuid() == 0) return true;
    uid_t uid = 0;
    CFRef<CFStringRef> user(SCDynamicStoreCopyConsoleUser(nullptr, &uid, nullptr));
    return user && uid == getuid();
}

using LockFn = int (*)();

LockFn lock_function() {
    static LockFn fn = [] {
        void* h = dlopen(kLoginFramework, RTLD_LAZY | RTLD_LOCAL);
        return h ? reinterpret_cast<LockFn>(dlsym(h, "SACLockScreenImmediate")) : nullptr;
    }();
    return fn;
}

struct LoginWindow {
    AEAddressDesc desc{};
    bool ok = false;
    LoginWindow() {
        ok = AECreateDesc(typeApplicationBundleID, kLoginWindow, std::strlen(kLoginWindow), &desc) == noErr;
    }
    ~LoginWindow() {
        if (ok) AEDisposeDesc(&desc);
    }
    LoginWindow(const LoginWindow&) = delete;
    LoginWindow& operator=(const LoginWindow&) = delete;
};

Availability loginwindow_events() {
    LoginWindow lw;
    if (!lw.ok) return Availability::Unknown;
    OSStatus st = AEDeterminePermissionToAutomateTarget(&lw.desc, typeWildCard, typeWildCard, false);
    switch (st) {
        case noErr: return Availability::Yes;
        case errAEEventWouldRequireUserConsent: return Availability::NeedsAuth;
        case errAEEventNotPermitted: return Availability::No;
        case procNotFound: return Availability::No;  // no loginwindow reachable from this session
        default: return Availability::Unknown;
    }
}

Result send_loginwindow(AEEventID id) {
    LoginWindow lw;
    if (!lw.ok) return Result::failure("cannot address loginwindow");
    AppleEvent ev{};
    OSStatus st = AECreateAppleEvent(kCoreEventClass, id, &lw.desc, kAutoGenerateReturnID, kAnyTransactionID, &ev);
    if (st != noErr) return Result::failure(os_status("AECreateAppleEvent", st));
    st = AESendMessage(&ev, nullptr, kAENoReply, kAEDefaultTimeout);
    AEDisposeDesc(&ev);
    if (st != noErr) return Result::failure(os_status("AESendMessage(loginwindow)", st));
    return Result::success();
}

}  // namespace

PowerCapabilities read_power_capabilities() {
    PowerCapabilities c;
    const bool console = console_session();
    c.suspend = (console && IOPMSleepEnabled()) ? Availability::Yes : Availability::No;
    c.hibernate = Availability::No;
    c.hybrid_sleep = Availability::No;
    c.reboot = c.power_off = console ? loginwindow_events() : Availability::No;
    c.lock = (console && lock_function()) ? Availability::Yes : Availability::No;
    return c;
}

Result perform_power_action(PowerAction action) {
    switch (action) {
        case PowerAction::Suspend: {
            io_connect_t pm = IOPMFindPowerManagement(MACH_PORT_NULL);
            if (!pm) return Result::failure("IOPMFindPowerManagement failed");
            IOReturn r = IOPMSleepSystem(pm);
            IOServiceClose(pm);
            if (r != kIOReturnSuccess) return Result::failure(os_status("IOPMSleepSystem", r));
            return Result::success();
        }
        case PowerAction::Hibernate:
            return Result::failure("macOS has no on-demand hibernate: hibernatemode / standby (pmset) apply to ordinary sleep");
        case PowerAction::HybridSleep:
            return Result::failure("hybrid sleep is macOS's hibernatemode 3 policy for ordinary sleep, not a separate request");
        case PowerAction::Reboot: return send_loginwindow(kAERestart);
        case PowerAction::PowerOff: return send_loginwindow(kAEShutDown);
        case PowerAction::Lock: {
            LockFn fn = lock_function();
            if (!fn) return Result::failure("login.framework SACLockScreenImmediate is unavailable");
            if (int r = fn(); r != 0) return Result::failure("SACLockScreenImmediate returned " + std::to_string(r));
            return Result::success();
        }
    }
    return Result::failure("unknown action");
}

}  // namespace brosys::mac
