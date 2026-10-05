// The shell-mode tray host: owns Shell_TrayWnd on one desktop.
//
// One thread per host. It switches to the host's desktop before creating any
// window, creates Shell_TrayWnd (and a TrayNotifyWnd child, which some apps
// look for), receives every Shell_NotifyIcon call as WM_COPYDATA, and pushes
// TrayItem snapshots into the queue. Queries read a mutex-guarded map;
// interaction posts the icon's callback message from the caller's thread.
#include "win/tray_host.h"
#include "win/tray_icon.h"
#include "win/tray_process.h"
#include "win/util.h"

#include <shellapi.h>

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace brosys::win::tray {

namespace {

constexpr UINT_PTR kSweepTimer = 1;
constexpr UINT kSweepIntervalMs = 1000;
constexpr wchar_t kTrayClass[] = L"Shell_TrayWnd";
constexpr wchar_t kNotifyClass[] = L"TrayNotifyWnd";

struct Icon {
    TrayItem item;
    CallbackTarget target;
    bool show_tip = false;  // NIF_SHOWTIP (version 4 icons show the standard tooltip only with it)
    std::optional<Rect32> rect;
};

LRESULT CALLBACK tray_wndproc(HWND, UINT, WPARAM, LPARAM);

bool register_classes(std::string* error) {
    static std::once_flag once;
    static std::string failure;
    std::call_once(once, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = tray_wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kTrayClass;
        if (!RegisterClassExW(&wc)) {
            failure = win32_error("RegisterClassEx(Shell_TrayWnd)", GetLastError());
            return;
        }
        WNDCLASSEXW child{};
        child.cbSize = sizeof child;
        child.lpfnWndProc = DefWindowProcW;
        child.hInstance = wc.hInstance;
        child.lpszClassName = kNotifyClass;
        if (!RegisterClassExW(&child)) failure = win32_error("RegisterClassEx(TrayNotifyWnd)", GetLastError());
    });
    if (!failure.empty() && error) *error = failure;
    return failure.empty();
}

class ShellHost final : public WinTrayHost {
public:
    ShellHost(const TrayConfig& config, HDESK desktop) : config_(config), desktop_(desktop) {
        // Hold our own reference: the caller's thread may move or exit.
        HANDLE own = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), desktop, GetCurrentProcess(), &own, 0, FALSE,
                            DUPLICATE_SAME_ACCESS))
            desktop_ = owned_desktop_ = static_cast<HDESK>(own);
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        detail_ = "owns Shell_TrayWnd on desktop '" + desktop_name(desktop) +
                  "': Shell_NotifyIcon calls of every process on it arrive here (toast / WinRT "
                  "notifications are a separate platform and do not)";
        hub_ = std::make_shared<BalloonHub>([this](const std::string& id) { return target_of(id); });
    }

    ~ShellHost() override {
        hub_->tray_destroyed();
        if (HWND w = hwnd_.load()) PostMessageW(w, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
        if (owned_desktop_) CloseDesktop(owned_desktop_);
    }

    bool start(std::string* error) {
        std::promise<std::string> ready;
        auto result = ready.get_future();
        thread_ = std::thread([this, &ready] { run(ready); });
        std::string why = result.get();
        if (why.empty()) return true;
        thread_.join();
        if (error) *error = why;
        return false;
    }

    // TrayHost
    TrayEventQueue& events() override { return events_; }
    TrayHostStatus status() const override { return {TrayRole::Shell, detail_}; }

    std::vector<TrayItem> items() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TrayItem> out;
        out.reserve(icons_.size());
        for (auto& [id, icon] : icons_) out.push_back(icon.item);
        return out;
    }

    Result activate(const std::string& id, int32_t x, int32_t y) override {
        return interact(id, x, y, WM_LBUTTONDOWN, WM_LBUTTONUP, NIN_SELECT);
    }
    Result secondary_activate(const std::string& id, int32_t x, int32_t y) override {
        return interact(id, x, y, WM_MBUTTONDOWN, WM_MBUTTONUP, 0);
    }
    Result context_menu(const std::string& id, int32_t x, int32_t y) override {
        return interact(id, x, y, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_CONTEXTMENU);
    }
    Result scroll(const std::string&, int32_t, ScrollOrientation) override {
        return Result::failure("Windows tray icons have no scroll interaction in the Shell_NotifyIcon protocol");
    }
    std::optional<MenuItem> menu(const std::string&) const override { return std::nullopt; }
    Result menu_about_to_show(const std::string&, int32_t) override { return own_menus(); }
    Result menu_event(const std::string&, int32_t, MenuEventType) override { return own_menus(); }

    Result set_item_rect(const std::string& id, const Rect32& rect) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = icons_.find(id);
        if (it == icons_.end()) return Result::failure("no tray item '" + id + "'");
        it->second.rect = rect;
        return Result::success();
    }

    std::shared_ptr<BalloonHub> balloon_hub() override { return hub_; }

    // Window procedure (tray thread).
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_COPYDATA: return on_copydata(reinterpret_cast<const COPYDATASTRUCT*>(lp));
            case WM_TIMER:
                if (wp == kSweepTimer) sweep();
                return 0;
            case WM_CLOSE: DestroyWindow(hwnd); return 0;
            case WM_DESTROY:
                KillTimer(hwnd, kSweepTimer);
                PostQuitMessage(0);
                return 0;
            default: break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

private:
    static Result own_menus() {
        return Result::failure("Windows tray icons draw their own menus; use context_menu()");
    }

    void run(std::promise<std::string>& ready) {
        if (!SetThreadDesktop(desktop_)) {
            ready.set_value(win32_error("SetThreadDesktop", GetLastError()));
            return;
        }
        // Re-check on this thread's desktop: another shell may have appeared.
        if (FindWindowW(kTrayClass, nullptr)) {
            ready.set_value("Shell_TrayWnd appeared on desktop '" + desktop_name(desktop_) + "' while starting");
            return;
        }
        std::string error;
        if (!register_classes(&error)) {
            ready.set_value(error);
            return;
        }
        HINSTANCE inst = GetModuleHandleW(nullptr);
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                    inst, nullptr);
        if (!hwnd) {
            ready.set_value(win32_error("CreateWindowEx(Shell_TrayWnd)", GetLastError()));
            return;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        CreateWindowExW(0, kNotifyClass, L"", WS_CHILD, 0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
        // Elevated apps must still reach a non-elevated shell (Explorer does the same).
        ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        SetTimer(hwnd, kSweepTimer, kSweepIntervalMs, nullptr);
        hwnd_.store(hwnd);
        events_.push(TrayHostStatus{TrayRole::Shell, detail_});
        ready.set_value({});

        if (config_.announce_taskbar_created) announce(hwnd);

        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        hwnd_.store(nullptr);
    }

    // "TaskbarCreated" to the top-level windows of this desktop only.
    void announce(HWND self) {
        struct Ctx {
            HWND self;
            UINT msg;
        } ctx{self, taskbar_created_};
        EnumDesktopWindows(
            desktop_,
            [](HWND w, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(lp);
                if (w != c->self) PostMessageW(w, c->msg, 0, 0);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
    }

    LRESULT on_copydata(const COPYDATASTRUCT* cds) {
        if (!cds) return FALSE;
        switch (cds->dwData) {
            case kCopyDataTray: {
                auto req = parse_notify(cds->lpData, cds->cbData, nullptr);
                return req ? on_notify(*req) : FALSE;
            }
            case kCopyDataIconRect: {
                auto q = parse_icon_rect(cds->lpData, cds->cbData, nullptr);
                return q ? on_icon_rect(*q) : 0;
            }
            case kCopyDataAppBar:
                // SHAppBarMessage: this shell has no taskbar of its own to report.
                return 0;
            default: return 0;
        }
    }

    static std::string id_of(const NotifyRequest& r) {
        return (r.flags & NIF_GUID) ? guid_item_id(r.guid) : hwnd_item_id(r.hwnd, r.uid);
    }

    // Rebuilds the tooltip view from the icon's state.
    static void refresh_tooltip(Icon& icon) {
        bool standard = icon.target.version < NOTIFYICON_VERSION_4 || icon.show_tip;
        icon.item.tooltip.title = standard ? icon.item.title : std::string();
    }

    static uint32_t diff(const TrayItem& a, const TrayItem& b) {
        uint32_t bits = 0;
        if (a.title != b.title) bits |= tray_change::Title;
        if (a.icon != b.icon) bits |= tray_change::Icon;
        if (a.tooltip != b.tooltip) bits |= tray_change::ToolTip;
        if (a.hidden != b.hidden || a.window_id != b.window_id || a.pid != b.pid || a.app_id != b.app_id)
            bits |= tray_change::Other;
        return bits;
    }

    static void apply(const NotifyRequest& r, const std::optional<Image>& image, Icon& icon) {
        if (r.hwnd && r.hwnd != icon.target.hwnd) {
            icon.target.hwnd = r.hwnd;
            icon.target.uid = r.uid;
            icon.item.window_id = reinterpret_cast<uintptr_t>(r.hwnd);
            DWORD pid = 0;
            GetWindowThreadProcessId(r.hwnd, &pid);
            icon.target.pid = pid;
            icon.item.pid = pid;
            icon.item.app_id = process_exe_name(pid);
        }
        if (r.flags & NIF_MESSAGE) icon.target.message = r.callback_message;
        if (r.flags & NIF_ICON) {
            icon.item.icon.pixmaps.clear();
            if (image) icon.item.icon.pixmaps.push_back(*image);
        }
        if (r.flags & NIF_TIP) {
            icon.item.title = to_utf8(r.tip);
            icon.show_tip = (r.flags & NIF_SHOWTIP) != 0;
        } else if (r.flags & NIF_SHOWTIP) {
            icon.show_tip = true;
        }
        if ((r.flags & NIF_STATE) && (r.state_mask & NIS_HIDDEN)) icon.item.hidden = (r.state & NIS_HIDDEN) != 0;
        refresh_tooltip(icon);
    }

    LRESULT on_notify(const NotifyRequest& r) {
        const std::string id = id_of(r);
        switch (r.message) {
            case NIM_ADD:
            case NIM_MODIFY: {
                if (r.message == NIM_ADD && !IsWindow(r.hwnd)) return FALSE;
                // Read the sender's icons now, while it is blocked in Shell_NotifyIcon.
                std::optional<Image> image;
                if (r.flags & NIF_ICON) image = icon_to_image(r.icon);
                TrayItem snapshot;
                uint32_t bits = 0;
                CallbackTarget target;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = icons_.find(id);
                    if (r.message == NIM_ADD) {
                        if (it != icons_.end()) return FALSE;
                        Icon icon;
                        icon.item.id = id;
                        apply(r, image, icon);
                        it = icons_.emplace(id, std::move(icon)).first;
                    } else {
                        if (it == icons_.end()) return FALSE;
                        TrayItem before = it->second.item;
                        apply(r, image, it->second);
                        bits = diff(before, it->second.item);
                    }
                    snapshot = it->second.item;
                    target = it->second.target;
                }
                if (r.message == NIM_ADD)
                    events_.push(TrayItemAdded{snapshot});
                else if (bits)
                    events_.push(TrayItemChanged{snapshot, bits});
                if (r.flags & NIF_INFO) balloon(r, snapshot, target);
                return TRUE;
            }
            case NIM_DELETE: {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!icons_.erase(id)) return FALSE;
                }
                events_.push(TrayItemRemoved{id});
                hub_->gone(id, BalloonGone::IconDeleted);
                return TRUE;
            }
            case NIM_SETVERSION: {
                if (r.timeout_or_version > NOTIFYICON_VERSION_4) return FALSE;
                TrayItem snapshot;
                uint32_t bits = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = icons_.find(id);
                    if (it == icons_.end()) return FALSE;
                    TrayItem before = it->second.item;
                    it->second.target.version = r.timeout_or_version;
                    refresh_tooltip(it->second);
                    bits = diff(before, it->second.item);
                    snapshot = it->second.item;
                }
                if (bits) events_.push(TrayItemChanged{snapshot, bits});
                return TRUE;
            }
            case NIM_SETFOCUS: {
                // The icon's menu closed and focus returns to the tray; the
                // host owns its own focus, nothing to record.
                std::lock_guard<std::mutex> lock(mutex_);
                return icons_.count(id) ? TRUE : FALSE;
            }
            default: return FALSE;
        }
    }

    void balloon(const NotifyRequest& r, const TrayItem& item, const CallbackTarget& target) {
        if (r.info.empty()) {
            hub_->gone(item.id, BalloonGone::SenderCleared);
            return;
        }
        BalloonData b;
        b.item_id = item.id;
        b.app_id = item.app_id;
        b.pid = item.pid;
        b.title = r.info_title;
        b.text = r.info;
        b.info_flags = r.info_flags;
        b.target = target;
        if ((r.info_flags & NIIF_ICON_MASK) == NIIF_USER) {
            if (r.balloon_icon) b.image = icon_to_image(r.balloon_icon);
            if (!b.image && !item.icon.pixmaps.empty()) b.image = item.icon.pixmaps.front();
        }
        hub_->shown(b);
    }

    LRESULT on_icon_rect(const IconRectQuery& q) {
        const std::string id = q.by_guid ? guid_item_id(q.guid) : hwnd_item_id(q.hwnd, q.uid);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = icons_.find(id);
        if (it == icons_.end() || !it->second.rect) return 0;
        // shell32 reads 0 as failure, so an icon at exactly (0, 0) cannot be
        // reported (the protocol's limit, Explorer's too).
        const Rect32& r = *it->second.rect;
        if (q.part == 2) return MAKELONG(static_cast<WORD>(r.width), static_cast<WORD>(r.height));
        return MAKELONG(static_cast<WORD>(r.x), static_cast<WORD>(r.y));
    }

    std::optional<CallbackTarget> target_of(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = icons_.find(id);
        if (it == icons_.end()) return std::nullopt;
        return it->second.target;
    }

    // Removes icons whose owner window no longer exists.
    void sweep() {
        std::vector<std::string> dead;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = icons_.begin(); it != icons_.end();) {
                if (!IsWindow(it->second.target.hwnd)) {
                    dead.push_back(it->first);
                    it = icons_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& id : dead) {
            events_.push(TrayItemRemoved{id});
            hub_->gone(id, BalloonGone::OwnerDied);
        }
    }

    Result interact(const std::string& id, int32_t x, int32_t y, UINT down, UINT up, UINT v3_event) {
        std::optional<CallbackTarget> t = target_of(id);
        if (!t) return Result::failure("no tray item '" + id + "'");
        if (!IsWindow(t->hwnd)) {
            sweep_one(id);
            return Result::failure("tray item '" + id + "' lost its window");
        }
        if (!t->message) return Result::failure("tray item '" + id + "' has no callback message (NIF_MESSAGE)");
        // Let the icon's process take the foreground for its own menu / window.
        AllowSetForegroundWindow(t->pid);
        bool ok = post_callback(*t, down, x, y) && post_callback(*t, up, x, y);
        if (ok && v3_event && t->version >= NOTIFYICON_VERSION) ok = post_callback(*t, v3_event, x, y);
        if (!ok) return Result::failure(win32_error("PostMessage", GetLastError()));
        return Result::success();
    }

    void sweep_one(const std::string& id) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!icons_.erase(id)) return;
        }
        events_.push(TrayItemRemoved{id});
        hub_->gone(id, BalloonGone::OwnerDied);
    }

    TrayConfig config_;
    HDESK desktop_;
    HDESK owned_desktop_ = nullptr;
    UINT taskbar_created_ = 0;
    std::string detail_;
    TrayEventQueue events_;
    mutable std::mutex mutex_;
    std::map<std::string, Icon> icons_;
    std::shared_ptr<BalloonHub> hub_;
    std::atomic<HWND> hwnd_{nullptr};
    std::thread thread_;
};

LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* host = reinterpret_cast<ShellHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (host) return host->handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

std::unique_ptr<TrayHost> create_shell_host(const TrayConfig& config, HDESK desktop, std::string* error) {
    auto host = std::make_unique<ShellHost>(config, desktop);
    if (!host->start(error)) return nullptr;
    return host;
}

}  // namespace brosys::win::tray
