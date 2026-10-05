// Alongside Explorer, against the user's real desktop, read-only: the tray
// host resolves to role None and creates nothing; the notification server is
// local only and the host's own notifications work fully.
#include "brosys/notifications.h"
#include "brosys/tray.h"
#include "check.h"
#include "win/event_log.h"

#include <windows.h>

using namespace brosys;
using namespace std::chrono_literals;

namespace {

constexpr const char* kName = "test_win_shell_alongside";

// Shell_TrayWnd windows owned by this process on this desktop.
int own_tray_windows() {
    struct Ctx {
        int n = 0;
    } ctx;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            wchar_t cls[64] = {};
            GetClassNameW(w, cls, 64);
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() && wcscmp(cls, L"Shell_TrayWnd") == 0) ++reinterpret_cast<Ctx*>(lp)->n;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.n;
}

void test_tray(HWND user_tray, DWORD owner) {
    std::string error;
    TrayConfig cfg;  // Auto
    auto host = TrayHost::create(cfg, &error);
    REQUIRE(host != nullptr);
    CHECK(host->status().role == TrayRole::None);
    const std::string& detail = host->status().detail;
    CHECK(detail.find("belongs to") != std::string::npos);
    CHECK(detail.find("pid " + std::to_string(owner)) != std::string::npos);
    CHECK(detail.find("toast") != std::string::npos);
    bstest::EventLog<TrayEvent> log(host->events());
    auto st = log.wait<TrayHostStatus>([](const TrayHostStatus&) { return true; }, 0, 1s);
    CHECK(st && st->role == TrayRole::None);
    CHECK(host->items().empty());
    CHECK(!host->activate("hwnd:0:0", 0, 0).ok);
    CHECK(!host->context_menu("hwnd:0:0", 0, 0).ok);
    CHECK(!host->set_item_rect("hwnd:0:0", Rect32{}).ok);

    // Explicit shell mode refuses; explicit alongside mode says so.
    std::string err2;
    CHECK(TrayHost::create(TrayConfig{.mode = TrayMode::Shell}, &err2) == nullptr);
    CHECK(err2.find("Shell_TrayWnd") != std::string::npos);
    auto along = TrayHost::create(TrayConfig{.mode = TrayMode::Alongside}, &err2);
    REQUIRE(along != nullptr);
    CHECK(along->status().role == TrayRole::None);
    CHECK(along->status().detail.find("TrayMode::Alongside") != std::string::npos);

    // Nothing was created or claimed.
    CHECK_EQ(own_tray_windows(), 0);
    CHECK(FindWindowW(L"Shell_TrayWnd", nullptr) == user_tray);

    // A notification server fed by this host is local only.
    NotificationServerConfig ncfg;
    ncfg.balloon_source = host.get();
    auto server = NotificationServer::create(ncfg, &err2);
    REQUIRE(server != nullptr);
    auto caps = server->capabilities();
    CHECK(!caps.receives_foreign);
    CHECK_EQ(caps.source, std::string("local only"));
    CHECK(caps.detail.find("none") != std::string::npos);
    CHECK(caps.detail.find("toast") != std::string::npos);
}

void test_local_notifications() {
    std::string error;
    NotificationServerConfig cfg;
    cfg.default_timeout_ms = 60000;
    auto server = NotificationServer::create(cfg, &error);
    REQUIRE(server != nullptr);
    CHECK(!server->capabilities().receives_foreign);
    CHECK_EQ(server->capabilities().source, std::string("local only"));
    int wakes = 0;
    std::mutex wm;
    server->events().set_wake([&] {
        std::lock_guard<std::mutex> lock(wm);
        ++wakes;
    });
    bstest::EventLog<NotificationEvent> log(server->events());
    auto st = log.wait<NotificationServerStatus>([](const NotificationServerStatus&) { return true; }, 0, 1s);
    CHECK(st && !st->active);

    auto posted = [&](size_t from) {
        return log.wait<NotificationPosted>([](const NotificationPosted&) { return true; }, from);
    };
    auto closed = [&](uint32_t id, size_t from, std::chrono::milliseconds t = 5s) {
        return log.wait<NotificationClosed>([&](const NotificationClosed& c) { return c.id == id; }, from, t);
    };

    Notification n;
    n.app_name = "host";
    n.summary = "hello";
    n.actions = {{"default", ""}, {"reply", "Reply"}};
    size_t m = log.mark();
    uint32_t id = server->post(n);
    CHECK(id != 0);
    auto p = posted(m);
    REQUIRE(p.has_value());
    CHECK(!p->replaced);
    CHECK_EQ(p->notification.id, id);
    CHECK_EQ(p->notification.sender, std::string("local"));
    CHECK_EQ(p->notification.sender_pid, uint32_t(GetCurrentProcessId()));
    CHECK(p->notification.expires_at.has_value());
    {
        std::lock_guard<std::mutex> lock(wm);
        CHECK(wakes > 0);
    }

    // Replace in place.
    n.id = id;
    n.summary = "hello again";
    m = log.mark();
    CHECK_EQ(server->post(n), id);
    p = posted(m);
    CHECK(p && p->replaced && p->notification.summary == "hello again");
    CHECK_EQ(server->active().size(), size_t(1));

    // An unknown id is a new notification.
    Notification other;
    other.id = 9999;
    m = log.mark();
    uint32_t other_id = server->post(other);
    p = posted(m);
    CHECK(p && !p->replaced && other_id != id);

    // Actions: unknown key refused; a known one dismisses a non-resident one.
    CHECK(!server->invoke_action(id, "nope", "").ok);
    m = log.mark();
    CHECK(server->invoke_action(id, "reply", "token").ok);
    auto c = closed(id, m);
    CHECK(c && c->reason == CloseReason::Dismissed);

    // Resident notifications stay after an action.
    Notification res;
    res.resident = true;
    res.actions = {{"default", ""}};
    uint32_t rid = server->post(res);
    CHECK(server->invoke_action(rid, "default", "").ok);
    bool still = false;
    for (auto& a : server->active()) still |= a.id == rid;
    CHECK(still);
    m = log.mark();
    CHECK(server->close(rid, CloseReason::Closed).ok);
    c = closed(rid, m);
    CHECK(c && c->reason == CloseReason::Closed);
    CHECK(!server->close(rid, CloseReason::Closed).ok);

    // Expiry: explicit timeout, never (0), critical with the default.
    Notification quick;
    quick.expire_timeout_ms = 200;
    m = log.mark();
    uint32_t qid = server->post(quick);
    c = closed(qid, m);
    CHECK(c && c->reason == CloseReason::Expired);
    Notification never;
    never.expire_timeout_ms = 0;
    m = log.mark();
    server->post(never);
    p = posted(m);
    CHECK(p && !p->notification.expires_at.has_value());
    Notification critical;
    critical.urgency = Urgency::Critical;
    m = log.mark();
    server->post(critical);
    p = posted(m);
    CHECK(p && !p->notification.expires_at.has_value());
    server->events().set_wake(nullptr);
}

}  // namespace

int main() {
    HWND user_tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!user_tray)
        bstest::skip(kName, "no shell owns Shell_TrayWnd on this desktop, so there is nothing to run alongside "
                            "(and Auto would make this test the shell)");
    DWORD owner = 0;
    GetWindowThreadProcessId(user_tray, &owner);
    test_tray(user_tray, owner);
    test_local_notifications();
    CHECK_EQ(own_tray_windows(), 0);
    return bstest::finish(kName);
}
