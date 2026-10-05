// macOS shell services report what macOS allows, honestly: the tray host is
// role None in every mode (menu-bar extras cannot be hosted by another
// process) and the notification server is local only (Notification Center
// has no server role), while the host's own notifications work fully.
#include "brosys/notifications.h"
#include "brosys/tray.h"
#include "check.h"
#include "win/event_log.h"  // portable despite the directory

#include <unistd.h>

using namespace brosys;
using namespace std::chrono_literals;

namespace {

constexpr const char* kName = "test_mac_shell";

void test_tray() {
    std::string error;
    for (TrayMode mode : {TrayMode::Auto, TrayMode::Alongside}) {
        auto host = TrayHost::create(TrayConfig{.mode = mode}, &error);
        REQUIRE(host != nullptr);
        CHECK(host->status().role == TrayRole::None);
        CHECK(host->status().detail.find("NSStatusItem") != std::string::npos);
        bstest::EventLog<TrayEvent> log(host->events());
        auto st = log.wait<TrayHostStatus>([](const TrayHostStatus&) { return true; }, 0, 1s);
        CHECK(st && st->role == TrayRole::None);
        CHECK(host->items().empty());
        CHECK(!host->menu("any").has_value());
        Result r = host->activate("any", 0, 0);
        CHECK(!r.ok);
        CHECK(r.error.find("not hosting") != std::string::npos);
        CHECK(!host->secondary_activate("any", 0, 0).ok);
        CHECK(!host->context_menu("any", 0, 0).ok);
        CHECK(!host->scroll("any", 1, ScrollOrientation::Vertical).ok);
        CHECK(!host->menu_about_to_show("any", 0).ok);
        CHECK(!host->menu_event("any", 0, MenuEventType::Clicked).ok);
        CHECK(!host->set_item_rect("any", Rect32{}).ok);
    }
    // Shell mode cannot be had, and says why.
    error.clear();
    CHECK(TrayHost::create(TrayConfig{.mode = TrayMode::Shell}, &error) == nullptr);
    CHECK(error.find("TrayMode::Shell") != std::string::npos);
}

void test_notifications() {
    std::string error;
    NotificationServerConfig cfg;
    cfg.default_timeout_ms = 60000;
    auto server = NotificationServer::create(cfg, &error);
    REQUIRE(server != nullptr);
    auto caps = server->capabilities();
    CHECK(!caps.receives_foreign);
    CHECK_EQ(caps.source, std::string("local only"));
    CHECK(caps.detail.find("Notification Center") != std::string::npos);

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
    CHECK_EQ(p->notification.sender, std::string("local"));
    CHECK_EQ(p->notification.sender_pid, uint32_t(getpid()));
    CHECK(p->notification.expires_at.has_value());

    n.id = id;
    n.summary = "hello again";
    m = log.mark();
    CHECK_EQ(server->post(n), id);
    p = posted(m);
    CHECK(p && p->replaced && p->notification.summary == "hello again");
    CHECK_EQ(server->active().size(), size_t(1));

    CHECK(!server->invoke_action(id, "nope", "").ok);
    m = log.mark();
    CHECK(server->invoke_action(id, "reply", "").ok);
    auto c = closed(id, m);
    CHECK(c && c->reason == CloseReason::Dismissed);

    Notification res;
    res.resident = true;
    res.actions = {{"default", ""}};
    uint32_t rid = server->post(res);
    CHECK(server->invoke_action(rid, "default", "").ok);
    CHECK_EQ(server->active().size(), size_t(1));
    m = log.mark();
    CHECK(server->close(rid, CloseReason::Closed).ok);
    c = closed(rid, m);
    CHECK(c && c->reason == CloseReason::Closed);
    CHECK(!server->close(rid, CloseReason::Closed).ok);

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
}

}  // namespace

int main() {
    test_tray();
    test_notifications();
    return bstest::finish(kName);
}
