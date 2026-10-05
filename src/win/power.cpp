// Windows power: a dedicated thread owns a hidden top-level window (power
// broadcasts, power-setting / battery-device notifications and session-end
// messages are only delivered to top-level windows, not to message-only
// ones), snapshots the state on every notification and on a poll timer,
// and pushes PowerChanged only when the snapshot changed.
#include "brosys/power.h"

#include "win/power_battery.h"
#include "win/util.h"

#include <windows.h>
#include <dbt.h>
#include <powrprof.h>

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace brosys {

namespace {

constexpr UINT WM_BROSYS_RUN = WM_APP + 1;   // lParam: std::function<void()>*
constexpr UINT_PTR kPollTimer = 1;
const wchar_t* kWindowClass = L"brosys.power";

// GUID_DEVICE_BATTERY (batclass.h needs INITGUID gymnastics; spell it here).
constexpr GUID kBatteryInterface = {0x72631e54, 0x78a4, 0x11d0, {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}};

bool policy_enabled(const wchar_t* subkey, const wchar_t* value) {
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        DWORD data = 0, size = sizeof data;
        if (RegGetValueW(root, subkey, value, RRF_RT_REG_DWORD, nullptr, &data, &size) == ERROR_SUCCESS && data != 0)
            return true;
    }
    return false;
}

// Whether the token holds SeShutdownPrivilege (enabled or not).
bool holds_shutdown_privilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    LUID luid{};
    bool found = false;
    if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &luid)) {
        DWORD need = 0;
        GetTokenInformation(token, TokenPrivileges, nullptr, 0, &need);
        std::vector<BYTE> buf(need ? need : 1);
        if (need && GetTokenInformation(token, TokenPrivileges, buf.data(), need, &need)) {
            auto* tp = reinterpret_cast<TOKEN_PRIVILEGES*>(buf.data());
            for (DWORD i = 0; i < tp->PrivilegeCount; ++i)
                if (tp->Privileges[i].Luid.LowPart == luid.LowPart && tp->Privileges[i].Luid.HighPart == luid.HighPart)
                    found = true;
        }
    }
    CloseHandle(token);
    return found;
}

Result enable_shutdown_privilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return Result::failure(win::win32_error("OpenProcessToken", GetLastError()));
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
        DWORD e = GetLastError();
        CloseHandle(token);
        return Result::failure(win::win32_error("LookupPrivilegeValue", e));
    }
    BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof tp, nullptr, nullptr);
    DWORD e = GetLastError();  // ERROR_NOT_ALL_ASSIGNED is reported with ok == TRUE
    CloseHandle(token);
    if (!ok) return Result::failure(win::win32_error("AdjustTokenPrivileges", e));
    if (e == ERROR_NOT_ALL_ASSIGNED) return Result::failure("the process token does not hold SeShutdownPrivilege");
    return Result::success();
}

PowerCapabilities read_capabilities() {
    PowerCapabilities c;
    SYSTEM_POWER_CAPABILITIES spc{};
    bool have_spc = GetPwrCapabilities(&spc) != FALSE;
    bool priv = holds_shutdown_privilege();
    bool no_close = policy_enabled(L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoClose");
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);

    if (!have_spc) {
        c.suspend = c.hibernate = Availability::Unknown;
    } else {
        bool s123 = spc.SystemS1 || spc.SystemS2 || spc.SystemS3;
        c.suspend = (s123 && IsPwrSuspendAllowed() && priv) ? Availability::Yes : Availability::No;
        c.hibernate =
            (spc.SystemS4 && spc.HiberFilePresent && IsPwrHibernateAllowed() && priv) ? Availability::Yes : Availability::No;
    }
    // Hybrid sleep is a power-plan setting applied to ordinary sleep, not a
    // separately requestable state.
    c.hybrid_sleep = Availability::No;
    c.reboot = c.power_off = (priv && !no_close) ? Availability::Yes : Availability::No;
    bool lock_disabled =
        policy_enabled(L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableLockWorkstation");
    c.lock = (session != 0 && !lock_disabled) ? Availability::Yes : Availability::No;
    return c;
}

struct InhibitorShared {
    std::mutex mutex;
    HWND hwnd = nullptr;  // null once the service is gone
    // Atomic: the window thread reads it in WM_QUERYENDSESSION without the
    // mutex (inhibit() may hold the mutex while it SendMessage()s that thread).
    std::atomic<int> shutdown_blocks{0};
};

class WinInhibitor final : public Inhibitor {
public:
    HANDLE request = nullptr;
    bool system_required = false;
    bool display_required = false;
    std::shared_ptr<InhibitorShared> shutdown;  // set when blocking shutdown

    ~WinInhibitor() override {
        if (request) {
            if (system_required) PowerClearRequest(request, PowerRequestSystemRequired);
            if (display_required) PowerClearRequest(request, PowerRequestDisplayRequired);
            CloseHandle(request);
        }
        if (shutdown) {
            std::lock_guard<std::mutex> lock(shutdown->mutex);
            if (--shutdown->shutdown_blocks == 0 && shutdown->hwnd) {
                auto fn = new std::function<void()>([h = shutdown->hwnd] { ShutdownBlockReasonDestroy(h); });
                if (!PostMessageW(shutdown->hwnd, WM_BROSYS_RUN, 0, reinterpret_cast<LPARAM>(fn))) delete fn;
            }
        }
    }
};

class WinPowerService final : public PowerService {
public:
    explicit WinPowerService(const PowerConfig& cfg) : config_(cfg), shared_(std::make_shared<InhibitorShared>()) {}

    ~WinPowerService() override {
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            shared_->hwnd = nullptr;
        }
        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
    }

    bool start(std::string* error) {
        std::promise<std::string> ready;
        auto fut = ready.get_future();
        thread_ = std::thread([this, &ready] { thread_main(ready); });
        std::string err = fut.get();
        if (!err.empty()) {
            if (thread_.joinable()) thread_.join();
            if (error) *error = err;
            return false;
        }
        return true;
    }

    PowerEventQueue& events() override { return queue_; }

    PowerState state() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    PowerCapabilities capabilities() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return caps_;
    }

    Result request(PowerAction action) override {
        Availability a = read_capabilities().of(action);
        if (a != Availability::Yes)
            return Result::failure(std::string(to_string(action)) + " is not available (" + to_string(a) + ")");
        switch (action) {
            case PowerAction::Suspend:
            case PowerAction::Hibernate: {
                Result r = enable_shutdown_privilege();
                if (!r) return r;
                if (!SetSuspendState(action == PowerAction::Hibernate, FALSE, FALSE))
                    return Result::failure(win::win32_error("SetSuspendState", GetLastError()));
                return Result::success();
            }
            case PowerAction::HybridSleep:
                return Result::failure("hybrid sleep is not separately requestable on Windows");
            case PowerAction::Reboot:
            case PowerAction::PowerOff: {
                Result r = enable_shutdown_privilege();
                if (!r) return r;
                UINT how = action == PowerAction::Reboot ? EWX_REBOOT : EWX_POWEROFF;
                if (!ExitWindowsEx(how, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
                    return Result::failure(win::win32_error("ExitWindowsEx", GetLastError()));
                return Result::success();
            }
            case PowerAction::Lock:
                if (!LockWorkStation()) return Result::failure(win::win32_error("LockWorkStation", GetLastError()));
                return Result::success();
        }
        return Result::failure("unknown action");
    }

    std::unique_ptr<Inhibitor> inhibit(const InhibitRequest& req, std::string* error) override {
        auto fail = [&](std::string why) -> std::unique_ptr<Inhibitor> {
            if (error) *error = std::move(why);
            return nullptr;
        };
        if (req.what == 0) return fail("nothing to inhibit");
        if (req.what & (inhibit::LidSwitch | inhibit::PowerKey))
            return fail("lid-switch / power-key inhibitors do not exist on Windows");
        if (req.mode == InhibitMode::Delay)
            return fail("Windows has no delay inhibitors (suspend gives no grace period to extend)");
        auto inh = std::make_unique<WinInhibitor>();
        std::wstring why = win::to_wide(req.who.empty() ? req.why : req.who + ": " + req.why);
        if (req.what & (inhibit::Sleep | inhibit::Idle)) {
            REASON_CONTEXT ctx{};
            ctx.Version = POWER_REQUEST_CONTEXT_VERSION;
            ctx.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
            ctx.Reason.SimpleReasonString = why.data();
            inh->request = PowerCreateRequest(&ctx);
            if (inh->request == INVALID_HANDLE_VALUE || !inh->request) {
                inh->request = nullptr;
                return fail(win::win32_error("PowerCreateRequest", GetLastError()));
            }
            if (!PowerSetRequest(inh->request, PowerRequestSystemRequired))
                return fail(win::win32_error("PowerSetRequest(SystemRequired)", GetLastError()));
            inh->system_required = true;
            if (req.what & inhibit::Idle) {
                if (!PowerSetRequest(inh->request, PowerRequestDisplayRequired))
                    return fail(win::win32_error("PowerSetRequest(DisplayRequired)", GetLastError()));
                inh->display_required = true;
            }
        }
        if (req.what & inhibit::Shutdown) {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            if (!shared_->hwnd) return fail("the power service is shutting down");
            bool ok = true;
            if (shared_->shutdown_blocks == 0) {
                std::function<void()> fn = [&] { ok = ShutdownBlockReasonCreate(shared_->hwnd, why.c_str()) != FALSE; };
                SendMessageW(shared_->hwnd, WM_BROSYS_RUN, 1, reinterpret_cast<LPARAM>(&fn));
            }
            if (!ok) return fail(win::win32_error("ShutdownBlockReasonCreate", GetLastError()));
            ++shared_->shutdown_blocks;
            inh->shutdown = shared_;
        }
        return inh;
    }

private:
    PowerState snapshot() {
        PowerState s;
        SYSTEM_POWER_STATUS sps{};
        bool have = GetSystemPowerStatus(&sps) != FALSE;
        if (have) {
            if (sps.ACLineStatus == 1) s.source = PowerSource::AC;
            else if (sps.ACLineStatus == 0) s.source = PowerSource::Battery;
        }
        s.devices = win::enumerate_batteries();
        bool system_battery = s.has_system_battery();
        if (system_battery && have) {
            if (sps.BatteryLifePercent <= 100) s.percent = static_cast<double>(sps.BatteryLifePercent);
            if (s.source == PowerSource::Battery && sps.BatteryLifeTime != static_cast<DWORD>(-1))
                s.time_to_empty_s = static_cast<int64_t>(sps.BatteryLifeTime);
        }
        if (system_battery) {
            for (auto& d : s.devices)
                if (d.power_supply && d.time_to_full_s) s.time_to_full_s = std::max(s.time_to_full_s.value_or(0), *d.time_to_full_s);
            if (!s.percent) {
                double sum = 0;
                int n = 0;
                for (auto& d : s.devices)
                    if (d.power_supply && d.percent) {
                        sum += *d.percent;
                        ++n;
                    }
                if (n) s.percent = sum / n;
            }
        }
        SYSTEM_POWER_CAPABILITIES spc{};
        if (GetPwrCapabilities(&spc)) s.lid_present = spc.LidPresent != FALSE;
        s.lid_closed = s.lid_present && lid_closed_;
        return s;
    }

    void refresh() {
        PowerState s = snapshot();
        PowerCapabilities c = read_capabilities();
        bool state_changed, caps_changed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_changed = !have_state_ || !(s == state_);
            caps_changed = !have_state_ || !(c == caps_);
            state_ = s;
            caps_ = c;
            have_state_ = true;
        }
        if (state_changed) queue_.push(PowerChanged{std::move(s)});
        if (caps_changed) queue_.push(PowerCapabilitiesChanged{c});
    }

    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<WinPowerService*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) return self->handle(hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_POWERBROADCAST:
                switch (wp) {
                    case PBT_APMSUSPEND:
                        suspended_ = true;
                        queue_.push(SleepPrepare{true});
                        break;
                    case PBT_APMRESUMEAUTOMATIC:
                    case PBT_APMRESUMESUSPEND:
                        if (suspended_) {
                            suspended_ = false;
                            queue_.push(SleepPrepare{false});
                        }
                        refresh();
                        break;
                    case PBT_APMPOWERSTATUSCHANGE: refresh(); break;
                    case PBT_POWERSETTINGCHANGE: {
                        auto* ps = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
                        if (ps && ps->PowerSetting == GUID_LIDSWITCH_STATE_CHANGE && ps->DataLength >= sizeof(DWORD))
                            lid_closed_ = *reinterpret_cast<const DWORD*>(ps->Data) == 0;
                        refresh();
                        break;
                    }
                    default: break;
                }
                return TRUE;
            case WM_DEVICECHANGE:
                if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) refresh();
                return TRUE;
            case WM_TIMER:
                if (wp == kPollTimer) refresh();
                return 0;
            case WM_QUERYENDSESSION: {
                queue_.push(ShutdownPrepare{true});
                return shared_->shutdown_blocks.load() > 0 ? FALSE : TRUE;
            }
            case WM_ENDSESSION:
                if (!wp) queue_.push(ShutdownPrepare{false});  // the shutdown was cancelled
                return 0;
            case WM_BROSYS_RUN: {
                auto* fn = reinterpret_cast<std::function<void()>*>(lp);
                if (fn) (*fn)();
                if (wp == 0) delete fn;  // posted (owned); 1 = sent (caller-owned)
                return 0;
            }
            case WM_CLOSE: DestroyWindow(hwnd); return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
            default: break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void thread_main(std::promise<std::string>& ready) {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = wndproc;
        wc.hInstance = inst;
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            ready.set_value(win::win32_error("RegisterClassEx", GetLastError()));
            return;
        }
        // A never-shown top-level window: WM_POWERBROADCAST, power-setting
        // and session-end messages are not sent to message-only windows.
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass, L"brosys power", WS_POPUP, 0, 0, 0,
                                    0, nullptr, nullptr, inst, nullptr);
        if (!hwnd) {
            ready.set_value(win::win32_error("CreateWindowEx", GetLastError()));
            return;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        hwnd_ = hwnd;
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            shared_->hwnd = hwnd;
        }
        std::vector<HPOWERNOTIFY> notes;
        for (const GUID* g : {&GUID_ACDC_POWER_SOURCE, &GUID_BATTERY_PERCENTAGE_REMAINING, &GUID_LIDSWITCH_STATE_CHANGE})
            if (HPOWERNOTIFY n = RegisterPowerSettingNotification(hwnd, g, DEVICE_NOTIFY_WINDOW_HANDLE)) notes.push_back(n);
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size = sizeof filter;
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = kBatteryInterface;
        HDEVNOTIFY devnote = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (config_.poll_interval_ms) SetTimer(hwnd, kPollTimer, config_.poll_interval_ms, nullptr);

        refresh();  // the first PowerChanged is queued before create() returns
        ready.set_value(std::string());

        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        for (auto n : notes) UnregisterPowerSettingNotification(n);
        if (devnote) UnregisterDeviceNotification(devnote);
        // Posted closures that never ran own their std::function.
        while (PeekMessageW(&m, nullptr, WM_BROSYS_RUN, WM_BROSYS_RUN, PM_REMOVE))
            if (m.wParam == 0) delete reinterpret_cast<std::function<void()>*>(m.lParam);
        hwnd_ = nullptr;
    }

    PowerConfig config_;
    PowerEventQueue queue_;
    mutable std::mutex mutex_;
    PowerState state_;
    PowerCapabilities caps_;
    bool have_state_ = false;
    bool lid_closed_ = false;
    bool suspended_ = false;
    std::shared_ptr<InhibitorShared> shared_;
    std::atomic<HWND> hwnd_{nullptr};
    std::thread thread_;
};

}  // namespace

std::unique_ptr<PowerService> PowerService::create(const PowerConfig& config, std::string* error) {
    auto s = std::make_unique<WinPowerService>(config);
    if (!s->start(error)) return nullptr;
    return s;
}

}  // namespace brosys
