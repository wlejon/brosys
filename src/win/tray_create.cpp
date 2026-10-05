// TrayHost::create on Windows: decide between shell and alongside mode.
//
// Shell_NotifyIcon finds the tray with a per-desktop window lookup
// (FindWindow "Shell_TrayWnd"), so the decision is made for the desktop of
// the calling thread, and a shell-mode host runs its window on that desktop.
#include "win/tray_host.h"
#include "win/tray_process.h"
#include "win/util.h"

namespace brosys::win::tray {

namespace {

class AlongsideHost final : public WinTrayHost {
public:
    explicit AlongsideHost(std::string detail) : detail_(std::move(detail)) {
        events_.push(TrayHostStatus{TrayRole::None, detail_});
    }

    TrayEventQueue& events() override { return events_; }
    TrayHostStatus status() const override { return {TrayRole::None, detail_}; }
    std::vector<TrayItem> items() const override { return {}; }

    Result activate(const std::string&, int32_t, int32_t) override { return none(); }
    Result secondary_activate(const std::string&, int32_t, int32_t) override { return none(); }
    Result context_menu(const std::string&, int32_t, int32_t) override { return none(); }
    Result scroll(const std::string&, int32_t, ScrollOrientation) override { return none(); }
    std::optional<MenuItem> menu(const std::string&) const override { return std::nullopt; }
    Result menu_about_to_show(const std::string&, int32_t) override { return none(); }
    Result menu_event(const std::string&, int32_t, MenuEventType) override { return none(); }
    Result set_item_rect(const std::string&, const Rect32&) override { return none(); }

    std::shared_ptr<BalloonHub> balloon_hub() override { return nullptr; }

private:
    Result none() const { return Result::failure("not hosting the tray: " + detail_); }

    TrayEventQueue events_;
    std::string detail_;
};

// Who owns the Shell_TrayWnd on the calling thread's desktop ("" if nobody).
std::string existing_shell_owner() {
    HWND existing = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!existing) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(existing, &pid);
    std::string exe = process_exe_name(pid);
    return (exe.empty() ? std::string("an unknown process") : exe) + " (pid " + std::to_string(pid) + ")";
}

}  // namespace

std::unique_ptr<TrayHost> create_alongside_host(std::string detail) {
    return std::make_unique<AlongsideHost>(std::move(detail));
}

std::string desktop_name(HDESK desktop) {
    wchar_t name[256] = {};
    DWORD needed = 0;
    if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof name - sizeof(wchar_t), &needed))
        return "?";
    return to_utf8(name);
}

}  // namespace brosys::win::tray

namespace brosys {

std::unique_ptr<TrayHost> TrayHost::create(const TrayConfig& config, std::string* error) {
    using namespace win::tray;
    HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    const std::string where = "desktop '" + desktop_name(desktop) + "'";
    const std::string toasts =
        "; toast (WinRT) notifications are not tray icons and never reach Shell_TrayWnd";

    if (config.mode == TrayMode::Alongside) {
        std::string owner = existing_shell_owner();
        return create_alongside_host("TrayMode::Alongside: not claiming the tray on " + where +
                                     (owner.empty() ? std::string(" (no shell owns it)")
                                                    : "; Shell_TrayWnd belongs to " + owner) +
                                     toasts);
    }

    std::string owner = existing_shell_owner();
    if (!owner.empty()) {
        std::string why = "Shell_TrayWnd on " + where + " belongs to " + owner +
                          "; Shell_NotifyIcon delivers icons only to that window, so they cannot be "
                          "hosted alongside it" + toasts;
        if (config.mode == TrayMode::Shell) {
            if (error) *error = "TrayMode::Shell: " + why;
            return nullptr;
        }
        return create_alongside_host(why);
    }
    return create_shell_host(config, desktop, error);
}

}  // namespace brosys
