// Balloons -> notifications, for real: a shell-mode TrayHost on a private
// desktop is the balloon_source of a NotificationServer; a separate client
// process sends genuine Shell_NotifyIconW NIF_INFO balloons and reports the
// NIN_BALLOON* messages its window receives.
#include "brosys/notifications.h"
#include "brosys/tray.h"
#include "check.h"
#include "win/event_log.h"
#include "win/shell_fixture.h"

#include <shellapi.h>

using namespace brosys;
using namespace std::chrono_literals;
using bstest::win::cb_line;

namespace {

constexpr const char* kName = "test_win_notify_balloons";

bstest::win::ShellFixture fx;
std::unique_ptr<TrayHost> host;

using NLog = bstest::EventLog<NotificationEvent>;

std::optional<NotificationPosted> posted(NLog& log, size_t from) {
    return log.wait<NotificationPosted>([](const NotificationPosted&) { return true; }, from);
}
std::optional<NotificationClosed> closed(NLog& log, uint32_t id, size_t from,
                                         std::chrono::milliseconds timeout = 10s) {
    return log.wait<NotificationClosed>([&](const NotificationClosed& c) { return c.id == id; }, from, timeout);
}

// The next callback line on the client after `from` equals `want`.
void expect_cb(size_t from, const std::string& want) {
    auto got = fx.client.wait_prefix("cb ", from, 5s);
    if (!got)
        bstest::fail(__FILE__, __LINE__, "missing callback " + want);
    else if (*got != want)
        bstest::fail(__FILE__, __LINE__, "callback '" + *got + "' expected '" + want + "'");
}
void expect_no_cb(size_t from) {
    auto got = fx.client.wait_prefix("cb ", from, 200ms);
    if (got) bstest::fail(__FILE__, __LINE__, "unexpected callback " + *got);
}

bool has_hint(const Notification& n, const std::string& key, const std::string& value) {
    for (auto& [k, v] : n.other_hints)
        if (k == key && v == value) return true;
    return false;
}

void test_capabilities() {
    NotificationServerConfig cfg;
    cfg.balloon_source = host.get();
    std::string error;
    auto server = NotificationServer::create(cfg, &error);
    REQUIRE(server != nullptr);
    auto caps = server->capabilities();
    CHECK(caps.receives_foreign);
    CHECK_EQ(caps.source, std::string("Shell_NotifyIcon balloons"));
    CHECK(caps.detail.find("toast") != std::string::npos);
    NLog log(server->events());
    auto status = log.wait<NotificationServerStatus>([](const NotificationServerStatus&) { return true; }, 0, 1s);
    CHECK(status && status->active);

    // One server per balloon source.
    std::string err2;
    CHECK(NotificationServer::create(cfg, &err2) == nullptr);
    CHECK(!err2.empty());

    // A tray host that is not the shell gives a local-only server.
    auto alongside = fx.create_host(TrayMode::Auto, &err2);
    REQUIRE(alongside != nullptr);
    NotificationServerConfig local_cfg;
    local_cfg.balloon_source = alongside.get();
    auto local = NotificationServer::create(local_cfg, &err2);
    REQUIRE(local != nullptr);
    CHECK(!local->capabilities().receives_foreign);
    CHECK_EQ(local->capabilities().source, std::string("local only"));
    CHECK(local->capabilities().detail.find("Shell_TrayWnd") != std::string::npos);
}

void test_balloons() {
    const std::string id1 = fx.item_id(1), id2 = fx.item_id(2);
    NotificationServerConfig cfg;
    cfg.balloon_source = host.get();
    cfg.default_timeout_ms = 60000;
    std::string error;
    auto server = NotificationServer::create(cfg, &error);
    REQUIRE(server != nullptr);
    NLog log(server->events());

    // Show: NIIF_INFO | NIIF_NOSOUND.
    size_t m = log.mark(), cm = fx.client.mark();
    CHECK_EQ(fx.client.command("balloon 1 11 Title|a<b"), std::string("1"));
    auto p = posted(log, m);
    REQUIRE(p.has_value());
    const Notification& n = p->notification;
    CHECK(!p->replaced);
    CHECK(n.id != 0);
    CHECK_EQ(n.summary, std::string("Title"));
    CHECK_EQ(n.body, std::string("a&lt;b"));  // plain balloon text, escaped for body-markup
    CHECK_EQ(n.app_icon, std::string("dialog-information"));
    CHECK_EQ(n.app_name, std::string("brosys_tray_client.exe"));
    CHECK_EQ(n.sender, id1);
    CHECK_EQ(n.sender_pid, uint32_t(fx.client_pid));
    CHECK(n.suppress_sound);
    CHECK(has_hint(n, "x-windows-niif", "info"));
    CHECK(n.expires_at.has_value());
    CHECK(n.actions.size() == 1 && n.actions[0].key == "default");
    expect_cb(cm, cb_line(1, 0, 0x10000 | NIN_BALLOONSHOW));
    const uint32_t first = n.id;

    // A re-sent balloon on the same icon replaces it.
    m = log.mark();
    cm = fx.client.mark();
    CHECK_EQ(fx.client.command("balloon 1 1 Second|text"), std::string("1"));
    p = posted(log, m);
    REQUIRE(p.has_value());
    CHECK(p->replaced);
    CHECK_EQ(p->notification.id, first);
    CHECK_EQ(p->notification.summary, std::string("Second"));
    CHECK(!p->notification.suppress_sound);
    CHECK_EQ(server->active().size(), size_t(1));
    expect_cb(cm, cb_line(1, 0, 0x10000 | NIN_BALLOONSHOW));

    // Click: NIN_BALLOONUSERCLICK, then closed as Dismissed without a timeout message.
    CHECK(!server->invoke_action(first, "nope", "").ok);
    m = log.mark();
    cm = fx.client.mark();
    CHECK(server->invoke_action(first, "default", "").ok);
    auto c = closed(log, first, m);
    CHECK(c && c->reason == CloseReason::Dismissed);
    expect_cb(cm, cb_line(1, 0, 0x10000 | NIN_BALLOONUSERCLICK));
    size_t after_click = fx.client.mark();
    expect_no_cb(after_click);
    CHECK(server->active().empty());
    CHECK(!server->close(first, CloseReason::Closed).ok);

    // NIIF_USER with hBalloonIcon (+ NIIF_LARGE_ICON); user dismissal -> NIN_BALLOONTIMEOUT.
    m = log.mark();
    cm = fx.client.mark();
    CHECK_EQ(fx.client.command("balloon 1 24 U|user icon"), std::string("1"));
    p = posted(log, m);
    REQUIRE(p.has_value());
    CHECK(p->notification.id != first);
    CHECK(has_hint(p->notification, "x-windows-large-icon", "true"));
    CHECK(has_hint(p->notification, "x-windows-niif", "user"));
    REQUIRE(p->notification.image.has_value());
    const Image& img = *p->notification.image;
    REQUIRE(img.width == 32 && img.height == 32 && img.rgba.size() == 4096u);
    CHECK(img.rgba[3] == 0);
    CHECK(img.rgba[4] == 0 && img.rgba[5] == 0 && img.rgba[6] == 255 && img.rgba[7] == 255);
    expect_cb(cm, cb_line(1, 0, 0x10000 | NIN_BALLOONSHOW));
    cm = fx.client.mark();
    m = log.mark();
    CHECK(server->close(p->notification.id, CloseReason::Dismissed).ok);
    c = closed(log, p->notification.id, m);
    CHECK(c && c->reason == CloseReason::Dismissed);
    expect_cb(cm, cb_line(1, 0, 0x10000 | NIN_BALLOONTIMEOUT));

    // Shows a balloon and waits for both the notification and the icon's NIN_BALLOONSHOW.
    auto show = [&](const std::string& cmd, const std::string& show_cb) {
        size_t lm = log.mark(), ccm = fx.client.mark();
        CHECK_EQ(fx.client.command(cmd), std::string("1"));
        auto r = posted(log, lm);
        expect_cb(ccm, show_cb);
        return r;
    };
    const std::string show1 = cb_line(1, 0, 0x10000 | NIN_BALLOONSHOW);

    // Plain NIIF_USER with a small hBalloonIcon replaces nothing (the last one closed).
    p = show("balloon 1 4 U|small", show1);
    REQUIRE(p.has_value());
    CHECK(!p->replaced);
    CHECK(p->notification.image && p->notification.image->width == 16 && p->notification.image->rgba[6] == 255);
    CHECK(!has_hint(p->notification, "x-windows-large-icon", "true"));

    // The host closes it -> NIN_BALLOONHIDE.
    p = show("balloon 1 2 W|warn", show1);
    REQUIRE(p.has_value());
    CHECK(p->replaced);
    CHECK_EQ(p->notification.app_icon, std::string("dialog-warning"));
    cm = fx.client.mark();
    CHECK(server->close(p->notification.id, CloseReason::Closed).ok);
    expect_cb(cm, cb_line(1, 0, 0x10000 | NIN_BALLOONHIDE));

    // The sender clears it (empty szInfo) -> Closed, and the icon hears NIN_BALLOONHIDE.
    p = show("balloon 1 0 N|none", show1);
    REQUIRE(p.has_value());
    CHECK(has_hint(p->notification, "x-windows-niif", "none"));
    m = log.mark();
    cm = fx.client.mark();
    CHECK_EQ(fx.client.command("clearballoon 1"), std::string("1"));
    c = closed(log, p->notification.id, m);
    CHECK(c && c->reason == CloseReason::Closed);
    expect_cb(cm, cb_line(1, 0, 0x10000 | NIN_BALLOONHIDE));

    // Version 0 icon: wParam = uID, lParam = the NIN_ message. Deleting the
    // icon closes its balloon.
    p = show("balloon 2 3 E|err", cb_line(1, 2, NIN_BALLOONSHOW));
    REQUIRE(p.has_value());
    CHECK_EQ(p->notification.app_icon, std::string("dialog-error"));
    CHECK_EQ(p->notification.sender, id2);
    m = log.mark();
    cm = fx.client.mark();
    CHECK_EQ(fx.client.command("del 2"), std::string("1"));
    c = closed(log, p->notification.id, m);
    CHECK(c && c->reason == CloseReason::Closed);
    expect_cb(cm, cb_line(1, 2, NIN_BALLOONHIDE));

    // The owner window dies with a balloon up.
    CHECK_EQ(fx.client.command("window2"), std::string("1"));
    CHECK_EQ(fx.client.command("add w5"), std::string("1"));
    p = show("balloon w5 1 Gone|soon", cb_line(2, 5, NIN_BALLOONSHOW));
    REQUIRE(p.has_value());
    m = log.mark();
    CHECK_EQ(fx.client.command("killwindow2"), std::string("1"));
    c = closed(log, p->notification.id, m, 5s);
    CHECK(c && c->reason == CloseReason::Closed);

    // The host's own notification flows through the same queue.
    Notification own;
    own.summary = "local";
    own.actions = {{"default", ""}};
    m = log.mark();
    cm = fx.client.mark();
    uint32_t lid = server->post(own);
    CHECK(lid != 0);
    p = posted(log, m);
    REQUIRE(p.has_value());
    CHECK_EQ(p->notification.sender, std::string("local"));
    CHECK(server->invoke_action(lid, "default", "").ok);
    c = closed(log, lid, m);
    CHECK(c && c->reason == CloseReason::Dismissed);
    expect_no_cb(cm);
}

// Balloons shown while no server is attached are held, and delivered when one
// attaches (after its status): the latest per icon, none that was cleared.
void test_held_balloons() {
    CHECK_EQ(fx.client.command("add 2"), std::string("1"));  // (test_balloons deleted it)
    size_t cm = fx.client.mark();
    CHECK_EQ(fx.client.command("balloon 1 1 Early|first"), std::string("1"));
    CHECK_EQ(fx.client.command("balloon 2 1 Gone|cleared"), std::string("1"));
    CHECK_EQ(fx.client.command("clearballoon 2"), std::string("1"));
    CHECK_EQ(fx.client.command("balloon 1 1 Early|second"), std::string("1"));  // replaces the held one
    expect_no_cb(cm);  // nothing is showing yet, so the icon hears nothing

    NotificationServerConfig cfg;
    cfg.balloon_source = host.get();
    cfg.default_timeout_ms = 0;
    std::string error;
    cm = fx.client.mark();
    auto server = NotificationServer::create(cfg, &error);
    REQUIRE(server != nullptr);
    NLog log(server->events());
    size_t status_at = 0, posted_at = 0;
    auto st = log.wait<NotificationServerStatus>([](const NotificationServerStatus&) { return true; }, 0, 1s, &status_at);
    CHECK(st && st->active);
    auto p = log.wait<NotificationPosted>([](const NotificationPosted&) { return true; }, 0, 5s, &posted_at);
    REQUIRE(p.has_value());
    CHECK(status_at < posted_at);
    CHECK_EQ(p->notification.summary, std::string("Early"));
    CHECK_EQ(p->notification.body, std::string("second"));
    CHECK_EQ(p->notification.sender, fx.item_id(1));
    CHECK(!p->replaced);
    expect_cb(cm, cb_line(1, 0, 0x10000 | NIN_BALLOONSHOW));
    std::this_thread::sleep_for(200ms);
    CHECK_EQ(log.count<NotificationPosted>([](const NotificationPosted&) { return true; }), size_t(1));
    CHECK_EQ(server->active().size(), size_t(1));
    // Closing it tells the icon, as for any balloon.
    cm = fx.client.mark();
    CHECK(server->close(p->notification.id, CloseReason::Closed).ok);
    expect_cb(cm, cb_line(1, 0, 0x10000 | NIN_BALLOONHIDE));
    CHECK_EQ(fx.client.command("del 2"), std::string("1"));
}

void test_expiry_and_source_gone() {
    NotificationServerConfig cfg;
    cfg.balloon_source = host.get();
    cfg.default_timeout_ms = 300;
    std::string error;
    auto server = NotificationServer::create(cfg, &error);
    REQUIRE(server != nullptr);
    NLog log(server->events());

    size_t m = log.mark(), cm = fx.client.mark();
    CHECK_EQ(fx.client.command("balloon 1 1 Soon|expires"), std::string("1"));
    auto p = posted(log, m);
    REQUIRE(p.has_value());
    expect_cb(cm, cb_line(1, 0, 0x10000 | NIN_BALLOONSHOW));
    cm = fx.client.mark();
    auto c = closed(log, p->notification.id, m, 5s);
    CHECK(c && c->reason == CloseReason::Expired);
    expect_cb(cm, cb_line(1, 0, 0x10000 | NIN_BALLOONTIMEOUT));

    // The tray host goes away under a live balloon.
    cfg.default_timeout_ms = 0;  // never expires
    server.reset();
    auto server2 = NotificationServer::create(cfg, &error);
    REQUIRE(server2 != nullptr);
    NLog log2(server2->events());
    m = log2.mark();
    CHECK_EQ(fx.client.command("balloon 1 1 Stay|up"), std::string("1"));
    p = log2.wait<NotificationPosted>([](const NotificationPosted&) { return true; }, m);
    REQUIRE(p.has_value());
    CHECK(!p->notification.expires_at.has_value());
    m = log2.mark();
    host.reset();
    auto c2 = log2.wait<NotificationClosed>([&](const NotificationClosed& x) { return x.id == p->notification.id; },
                                            m, 2s);
    CHECK(c2 && c2->reason == CloseReason::Closed);
    auto st = log2.wait<NotificationServerStatus>([](const NotificationServerStatus& s) { return !s.active; }, m, 2s);
    CHECK(st.has_value());
    CHECK(!server2->capabilities().receives_foreign);
    CHECK(server2->active().empty());
}

}  // namespace

int main() {
    fx.require_desktop(kName);
    HWND user_tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    bstest::win::TaskbarCreatedProbe probe;

    std::string why;
    if (!fx.start_client(&why)) {
        bstest::fail(__FILE__, __LINE__, "client: " + why);
        return bstest::finish(kName);
    }
    host = fx.create_host(TrayMode::Auto, &why);
    if (!host || host->status().role != TrayRole::Shell) {
        bstest::fail(__FILE__, __LINE__, "no shell-mode host on the private desktop: " + why);
        return bstest::finish(kName);
    }
    if (!fx.client_sees_this_process()) {
        bstest::fail(__FILE__, __LINE__, "the client's Shell_TrayWnd lookup does not resolve to the host");
        return bstest::finish(kName);
    }
    CHECK_EQ(fx.client.command("add 1"), std::string("1"));
    CHECK_EQ(fx.client.command("version 1 4"), std::string("1"));
    CHECK_EQ(fx.client.command("add 2"), std::string("1"));

    test_capabilities();
    test_balloons();
    test_held_balloons();
    test_expiry_and_source_gone();

    host.reset();
    fx.client.stop();
    CHECK_EQ(probe.count(), 0);
    CHECK(FindWindowW(L"Shell_TrayWnd", nullptr) == user_tray);
    return bstest::finish(kName);
}
