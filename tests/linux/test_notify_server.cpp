// The org.freedesktop.Notifications server on a private bus, driven by real
// clients: notify-send (libnotify) for Notify / replace / hints / expiry /
// actions / --wait, gdbus for raw calls (image-data, CloseNotification,
// GetCapabilities, GetServerInformation) and gdbus monitor to see our
// signals as a client does. Plus host-side post / close / invoke_action and
// name ownership between several servers.
#include "check.h"
#include "brosys/notifications.h"
#include "linux/desktop/event_log.h"
#include "linux/desktop/signal_log.h"
#include "linux/support/private_bus.h"

#include <algorithm>
#include <csignal>

using namespace brosys;
using namespace std::chrono_literals;
using bstest::run;
using Log = bstest::EventLog<NotificationEvent>;

namespace {

constexpr const char* kDest = "org.freedesktop.Notifications";
constexpr const char* kPath = "/org/freedesktop/Notifications";

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r')) s.pop_back();
    return s;
}

// "7\n" (notify-send -p) or "(uint32 7,)" (gdbus).
uint32_t parse_uint(const std::string& s) {
    size_t start = s.find("uint32 ");
    size_t i = s.find_first_of("0123456789", start == std::string::npos ? 0 : start + 7);
    if (i == std::string::npos) return 0;
    return static_cast<uint32_t>(std::strtoul(s.c_str() + i, nullptr, 10));
}

std::string other_hint(const Notification& n, const std::string& key) {
    for (auto& [k, v] : n.other_hints)
        if (k == key) return v;
    return "<absent>";
}

bool is_signal(const brosys::dbus::Message& m, const char* member, uint32_t id) {
    return m.member == member && !m.args.empty() && m.args[0].as_uint() == id;
}

std::unique_ptr<NotificationServer> make_server(const bstest::PrivateBus& bus, NotificationServerConfig cfg) {
    cfg.session_bus_address = bus.address();
    std::string err;
    auto s = NotificationServer::create(cfg, &err);
    if (!s) std::fprintf(stderr, "create: %s\n", err.c_str());
    return s;
}

bstest::RunResult gdbus_call(const bstest::PrivateBus& bus, const std::string& method, std::vector<std::string> args) {
    std::vector<std::string> argv{"gdbus", "call", "--session", "--dest", kDest, "--object-path", kPath,
                                  "--method", std::string("org.freedesktop.Notifications.") + method};
    for (auto& a : args) argv.push_back(a);
    return run(argv, bus.env());
}

// ---------------------------------------------------------------- notify-send

void test_notify_send(const bstest::PrivateBus& bus) {
    auto server = make_server(bus, NotificationServerConfig{});
    REQUIRE(server);
    Log log(server->events());
    auto st = log.wait_any<NotificationServerStatus>();
    REQUIRE(st);
    CHECK(st->active);
    CHECK(server->capabilities().receives_foreign);
    CHECK_EQ(server->capabilities().source, std::string(kDest));

    auto r = run({"notify-send", "-p", "-a", "Mail App", "-i", "mail-unread", "-n", "mail-unread", "-u", "critical", "-c", "email.arrived",
                  "-e", "-t", "0", "-h", "string:desktop-entry:org.test.Mail", "-h", "int:x:5", "-h", "int:y:6",
                  "-h", "boolean:resident:true", "-h", "boolean:suppress-sound:true",
                  "-h", "string:sound-name:message-new-email", "-h", "string:image-path:/tmp/pic.png",
                  "-h", "string:x-custom:hello", "Summary one", "Body <b>bold</b>"},
                 bus.env());
    CHECK_EQ(r.exit_code, 0);
    if (r.exit_code != 0) std::fprintf(stderr, "notify-send: %s\n", r.err.c_str());
    uint32_t id = parse_uint(r.out);
    CHECK(id != 0);
    auto posted = log.wait<NotificationPosted>([&](const NotificationPosted& p) { return p.notification.id == id; });
    REQUIRE(posted);
    const Notification& n = posted->notification;
    CHECK(!posted->replaced);
    CHECK_EQ(n.app_name, std::string("Mail App"));
    CHECK_EQ(n.app_icon, std::string("mail-unread"));
    CHECK_EQ(n.summary, std::string("Summary one"));
    CHECK_EQ(n.body, std::string("Body <b>bold</b>"));
    CHECK(n.urgency == Urgency::Critical);
    CHECK_EQ(n.category, std::string("email.arrived"));
    CHECK_EQ(n.desktop_entry, std::string("org.test.Mail"));
    CHECK(n.transient);
    CHECK(n.resident);
    CHECK(n.suppress_sound);
    CHECK_EQ(n.sound_name, std::string("message-new-email"));
    CHECK_EQ(n.image_path, std::string("/tmp/pic.png"));
    CHECK(n.x == 5 && n.y == 6);
    CHECK_EQ(other_hint(n, "x-custom"), std::string("'hello'"));
    CHECK_EQ(n.expire_timeout_ms, 0);
    CHECK(!n.expires_at.has_value());
    CHECK(!n.sender.empty() && n.sender[0] == ':');
    CHECK(n.sender_pid != 0);
    auto act = server->active();
    CHECK(std::any_of(act.begin(), act.end(), [&](const Notification& a) { return a.id == id && a.summary == "Summary one"; }));

    // Replace in place.
    r = run({"notify-send", "-p", "-r", std::to_string(id), "Summary two"}, bus.env());
    CHECK_EQ(r.exit_code, 0);
    CHECK_EQ(parse_uint(r.out), id);
    posted = log.wait<NotificationPosted>([&](const NotificationPosted& p) { return p.notification.id == id; });
    REQUIRE(posted);
    CHECK(posted->replaced);
    CHECK_EQ(posted->notification.summary, std::string("Summary two"));
    CHECK(posted->notification.urgency == Urgency::Normal);
    CHECK_EQ(server->active().size(), size_t(1));

    // Replacing an unknown id makes a new notification.
    r = run({"notify-send", "-p", "-r", "4000", "Fresh"}, bus.env());
    uint32_t fresh = parse_uint(r.out);
    CHECK(fresh != 0 && fresh != id && fresh != 4000);
    posted = log.wait<NotificationPosted>([&](const NotificationPosted& p) { return p.notification.summary == "Fresh"; });
    REQUIRE(posted);
    CHECK(!posted->replaced);
    CHECK_EQ(posted->notification.id, fresh);
}

void test_expiry(const bstest::PrivateBus& bus) {
    NotificationServerConfig cfg;
    cfg.default_timeout_ms = 300;
    auto server = make_server(bus, cfg);
    REQUIRE(server);
    Log log(server->events());
    bstest::SignalLog sig(bus.address(), "type='signal',interface='org.freedesktop.Notifications'");
    REQUIRE(sig.ok());
    // gdbus monitor shows the signals to an ordinary GDBus client.
    std::unique_ptr<bstest::Daemon> monitor;
    if (bstest::have_program("stdbuf")) {
        monitor = std::make_unique<bstest::Daemon>(
            std::vector<std::string>{"stdbuf", "-oL", "gdbus", "monitor", "--session", "--dest", kDest}, bus.env());
        CHECK(monitor->wait_for_line("Monitoring signals", 5000ms));
    }

    auto start = std::chrono::steady_clock::now();
    auto r = run({"notify-send", "-p", "default timeout"}, bus.env());
    uint32_t id_default = parse_uint(r.out);
    r = run({"notify-send", "-p", "-t", "200", "explicit timeout"}, bus.env());
    uint32_t id_explicit = parse_uint(r.out);
    r = run({"notify-send", "-p", "-u", "critical", "critical default"}, bus.env());
    uint32_t id_critical = parse_uint(r.out);
    r = run({"notify-send", "-p", "-t", "0", "never"}, bus.env());
    uint32_t id_never = parse_uint(r.out);
    CHECK(id_default && id_explicit && id_critical && id_never);

    for (uint32_t id : {id_default, id_explicit, id_critical, id_never}) {
        auto p = log.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.id == id; });
        REQUIRE(p);
        bool should_expire = id == id_default || id == id_explicit;
        CHECK_EQ(p->notification.expires_at.has_value(), should_expire);
        if (id == id_default) CHECK_EQ(p->notification.expire_timeout_ms, -1);
        if (id == id_explicit) CHECK_EQ(p->notification.expire_timeout_ms, 200);
    }
    for (uint32_t id : {id_default, id_explicit}) {
        auto c = log.wait<NotificationClosed>([&](const NotificationClosed& e) { return e.id == id; }, 5000ms);
        REQUIRE(c);
        CHECK(c->reason == CloseReason::Expired);
        CHECK(sig.wait([&](const auto& m) { return is_signal(m, "NotificationClosed", id) && m.args[1].as_uint() == 1; }) >= 0);
        if (monitor) {
            // The two expiries may print in either order.
            std::string needle = "NotificationClosed (uint32 " + std::to_string(id) + ", uint32 1)";
            if (monitor->transcript().find(needle) == std::string::npos && !monitor->wait_for_line(needle, 5000ms))
                bstest::fail(__FILE__, __LINE__, "gdbus monitor did not show the expiry:\n" + monitor->transcript());
        }
    }
    CHECK(std::chrono::steady_clock::now() - start >= 200ms);
    // Critical (-1) and 0 never expire.
    CHECK(!log.seen<NotificationClosed>([](const NotificationClosed&) { return true; }, 700ms));
    CHECK_EQ(server->active().size(), size_t(2));
}

void test_actions(const bstest::PrivateBus& bus) {
    auto server = make_server(bus, NotificationServerConfig{});
    REQUIRE(server);
    Log log(server->events());
    bstest::SignalLog sig(bus.address(), "type='signal',interface='org.freedesktop.Notifications'");
    REQUIRE(sig.ok());

    // stdbuf -oL: notify-send prints the id with stdio, which a pipe block-buffers, and older
    // libnotify (0.8.3, Ubuntu 24.04) never flushes it while it waits for the action. Without
    // line buffering the id only arrives when the notification expires and notify-send exits.
    bstest::Daemon client({"stdbuf", "-oL", "notify-send", "-p", "--action=ok=OK", "--action=cancel=Cancel", "Act", "choose"},
                          bus.env());
    std::string line;
    REQUIRE(client.read_line(line, 10000ms));
    uint32_t id = parse_uint(line);
    REQUIRE(id != 0);
    auto p = log.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.id == id; });
    REQUIRE(p);
    CHECK(p->notification.actions ==
          (std::vector<NotificationAction>{{"ok", "OK"}, {"cancel", "Cancel"}}));
    CHECK_EQ(p->notification.sender_pid, static_cast<uint32_t>(client.pid()));
    CHECK(!server->invoke_action(id, "nope", "").ok);
    CHECK(!server->invoke_action(999, "ok", "").ok);

    size_t base = sig.snapshot().size();
    Result res = server->invoke_action(id, "ok", "token-123");
    CHECK(res.ok);
    // notify-send prints the invoked action's key and exits.
    CHECK(client.wait_for_line("ok", 5000ms));
    CHECK_EQ(client.wait_exit(5000ms), 0);
    int tok = sig.wait([&](const auto& m) { return is_signal(m, "ActivationToken", id); }, 5000ms, base);
    int inv = sig.wait([&](const auto& m) { return is_signal(m, "ActionInvoked", id); }, 5000ms, base);
    int closed = sig.wait([&](const auto& m) { return is_signal(m, "NotificationClosed", id); }, 5000ms, base);
    CHECK(tok >= 0 && inv > tok && closed > inv);
    auto msgs = sig.snapshot();
    if (tok >= 0) CHECK_EQ(msgs[static_cast<size_t>(tok)].args[1].as_string(), std::string("token-123"));
    if (inv >= 0) CHECK_EQ(msgs[static_cast<size_t>(inv)].args[1].as_string(), std::string("ok"));
    if (closed >= 0) CHECK_EQ(msgs[static_cast<size_t>(closed)].args[1].as_uint(), uint64_t(2));
    auto c = log.wait<NotificationClosed>([&](const NotificationClosed& e) { return e.id == id; });
    CHECK(c && c->reason == CloseReason::Dismissed);

    // --wait: the client waits until the host closes it.
    bstest::Daemon waiter({"stdbuf", "-oL", "notify-send", "-p", "-w", "Waiting"}, bus.env());
    REQUIRE(waiter.read_line(line, 10000ms));
    uint32_t wid = parse_uint(line);
    REQUIRE(wid != 0);
    REQUIRE(log.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.id == wid; }));
    CHECK(waiter.running());
    CHECK(server->close(wid, CloseReason::Dismissed).ok);
    CHECK_EQ(waiter.wait_exit(5000ms), 0);
    CHECK(!server->close(wid, CloseReason::Dismissed).ok);

    // Resident: the action does not close it; no token -> no ActivationToken.
    auto g = gdbus_call(bus, "Notify", {"res", "0", "''", "Resident", "''", "['default', 'Open']",
                                        "{'resident': <true>}", "int32 -1"});  // a bare -1 reads as an option
    CHECK_EQ(g.exit_code, 0);
    if (g.exit_code != 0) std::fprintf(stderr, "gdbus: %s\n", g.err.c_str());
    uint32_t rid = parse_uint(g.out);
    REQUIRE(log.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.id == rid; }));
    base = sig.snapshot().size();
    CHECK(server->invoke_action(rid, "default", "").ok);
    CHECK(sig.wait([&](const auto& m) { return is_signal(m, "ActionInvoked", rid); }, 5000ms, base) >= 0);
    CHECK(sig.wait([&](const auto& m) { return is_signal(m, "ActivationToken", rid); }, 300ms, base) < 0);
    auto act = server->active();
    CHECK(std::any_of(act.begin(), act.end(), [&](const Notification& a) { return a.id == rid; }));
}

// ---------------------------------------------------------------- gdbus

void test_gdbus(const bstest::PrivateBus& bus) {
    NotificationServerConfig cfg;
    cfg.name = "brosys-test";
    cfg.vendor = "bro";
    cfg.version = "9.9";
    cfg.capabilities = {"actions", "body", "body-markup", "icon-static"};
    auto server = make_server(bus, cfg);
    REQUIRE(server);
    Log log(server->events());
    bstest::SignalLog sig(bus.address(), "type='signal',interface='org.freedesktop.Notifications'");

    auto g = gdbus_call(bus, "GetCapabilities", {});
    CHECK_EQ(g.exit_code, 0);
    CHECK_EQ(trim(g.out), std::string("(['actions', 'body', 'body-markup', 'icon-static'],)"));
    g = gdbus_call(bus, "GetServerInformation", {});
    CHECK_EQ(g.exit_code, 0);
    CHECK_EQ(trim(g.out), std::string("('brosys-test', 'bro', '9.9', '1.2')"));

    // Raw image-data: RGB with rowstride padding; image-path alongside.
    g = gdbus_call(bus, "Notify",
                   {"gapp", "0", "dialog-information", "Picture", "with image", "['default', 'Open', 'later', 'Later']",
                    "{'image-data': <(2, 2, 8, false, 8, 3, [byte 1, 2, 3, 4, 5, 6, 0xee, 0xee, 7, 8, 9, 10, 11, 12])>,"
                    " 'icon_data': <(1, 1, 4, true, 8, 4, [byte 9, 9, 9, 9])>,"
                    " 'image-path': <'/tmp/x.png'>, 'urgency': <byte 0>, 'x-number': <uint32 7>}",
                    "1500"});
    CHECK_EQ(g.exit_code, 0);
    uint32_t id = parse_uint(g.out);
    REQUIRE(id != 0);
    auto p = log.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.id == id; });
    REQUIRE(p);
    const Notification& n = p->notification;
    REQUIRE(n.image.has_value());
    CHECK(n.image->width == 2 && n.image->height == 2);
    CHECK(n.image->rgba == (std::vector<uint8_t>{1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255}));
    CHECK_EQ(n.image_path, std::string("/tmp/x.png"));
    CHECK(n.urgency == Urgency::Low);
    CHECK_EQ(other_hint(n, "x-number"), std::string("7"));
    CHECK(other_hint(n, "icon_data") != "<absent>");
    CHECK_EQ(n.actions.size(), size_t(2));
    CHECK_EQ(n.expire_timeout_ms, 1500);
    CHECK(n.expires_at.has_value());
    CHECK_EQ(n.app_icon, std::string("dialog-information"));

    // CloseNotification -> reason 3; unknown id -> error reply.
    g = gdbus_call(bus, "CloseNotification", {std::to_string(id)});
    CHECK_EQ(g.exit_code, 0);
    auto c = log.wait<NotificationClosed>([&](const NotificationClosed& e) { return e.id == id; });
    CHECK(c && c->reason == CloseReason::Closed);
    CHECK(sig.wait([&](const auto& m) { return is_signal(m, "NotificationClosed", id) && m.args[1].as_uint() == 3; }) >= 0);
    g = gdbus_call(bus, "CloseNotification", {std::to_string(id)});
    CHECK(g.exit_code != 0);
    CHECK(server->active().empty());

    // A wrong signature is refused by the layer.
    g = gdbus_call(bus, "CloseNotification", {"'x'"});
    CHECK(g.exit_code != 0);
}

// ---------------------------------------------------------------- host side

void test_post(const bstest::PrivateBus& bus) {
    NotificationServerConfig cfg;
    cfg.default_timeout_ms = 0;
    auto server = make_server(bus, cfg);
    REQUIRE(server);
    Log log(server->events());
    bstest::SignalLog sig(bus.address(), "type='signal',interface='org.freedesktop.Notifications'");

    Notification n;
    n.app_name = "host";
    n.summary = "Battery low";
    n.urgency = Urgency::Critical;
    n.actions = {{"default", "Open"}};
    uint32_t id = server->post(n);
    CHECK(id != 0);
    auto p = log.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.id == id; });
    REQUIRE(p);
    CHECK(!p->replaced);
    CHECK_EQ(p->notification.sender, std::string("local"));
    CHECK_EQ(p->notification.sender_pid, static_cast<uint32_t>(getpid()));
    CHECK(!p->notification.expires_at.has_value());

    n.id = id;
    n.summary = "Battery very low";
    CHECK_EQ(server->post(n), id);
    p = log.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.id == id; });
    CHECK(p && p->replaced && p->notification.summary == "Battery very low");

    // A foreign notification's id space is shared with ours.
    auto r = run({"notify-send", "-p", "foreign"}, bus.env());
    uint32_t fid = parse_uint(r.out);
    CHECK(fid != 0 && fid != id);

    n.id = 0;
    n.expire_timeout_ms = 150;
    n.urgency = Urgency::Normal;
    uint32_t short_id = server->post(n);
    auto c = log.wait<NotificationClosed>([&](const NotificationClosed& e) { return e.id == short_id; });
    CHECK(c && c->reason == CloseReason::Expired);

    CHECK(server->close(id, CloseReason::Dismissed).ok);
    CHECK(sig.wait([&](const auto& m) { return is_signal(m, "NotificationClosed", id) && m.args[1].as_uint() == 2; }) >= 0);
    CHECK(!server->close(id, CloseReason::Dismissed).ok);
    c = log.wait<NotificationClosed>([&](const NotificationClosed& e) { return e.id == id; });
    CHECK(c && c->reason == CloseReason::Dismissed);
}

// ---------------------------------------------------------------- name ownership

void test_names() {
    bstest::PrivateBus bus;
    REQUIRE(bus.ok());
    auto a = make_server(bus, NotificationServerConfig{});
    REQUIRE(a);
    Log la(a->events());
    auto st = la.wait_any<NotificationServerStatus>();
    CHECK(st && st->active);

    // B queues behind A.
    auto b = make_server(bus, NotificationServerConfig{});
    REQUIRE(b);
    Log lb(b->events());
    st = lb.wait_any<NotificationServerStatus>();
    REQUIRE(st);
    CHECK(!st->active);
    CHECK(st->detail.find("queued") != std::string::npos);
    CHECK(!b->capabilities().receives_foreign);
    CHECK(!b->capabilities().detail.empty());
    auto r = run({"notify-send", "-p", "to A"}, bus.env());
    CHECK_EQ(r.exit_code, 0);
    CHECK(la.wait<NotificationPosted>([](const NotificationPosted& e) { return e.notification.summary == "to A"; }).has_value());
    CHECK(!lb.seen<NotificationPosted>([](const NotificationPosted&) { return true; }, 200ms));

    // C replaces A (A allows replacement); A loses the name.
    NotificationServerConfig cc;
    cc.replace_existing = true;
    auto c = make_server(bus, cc);
    REQUIRE(c);
    Log lc(c->events());
    st = lc.wait_any<NotificationServerStatus>();
    CHECK(st && st->active);
    st = la.wait_any<NotificationServerStatus>();
    CHECK(st && !st->active);
    CHECK(!a->capabilities().receives_foreign);
    r = run({"notify-send", "-p", "to C"}, bus.env());
    CHECK(lc.wait<NotificationPosted>([](const NotificationPosted& e) { return e.notification.summary == "to C"; }).has_value());

    // C goes away: one of the queued servers (A or B) takes over, then the other.
    c.reset();
    auto became_active = [](const NotificationServerStatus& s) { return s.active; };
    CHECK(bstest::wait_until([&] {
        return la.seen<NotificationServerStatus>(became_active, 0ms) || lb.seen<NotificationServerStatus>(became_active, 0ms);
    }, 5000ms));
    std::this_thread::sleep_for(200ms);  // a wrong second acquisition would show up now
    bool a_got = la.seen<NotificationServerStatus>(became_active, 0ms);
    bool b_got = lb.seen<NotificationServerStatus>(became_active, 0ms);
    CHECK(a_got != b_got);
    CHECK(a->capabilities().receives_foreign == a_got);
    CHECK(b->capabilities().receives_foreign == b_got);
    if (a_got) a.reset();
    else b.reset();
    auto& survivor = a ? a : b;
    Log& ls = a ? la : lb;
    // The survivor's log holds no earlier activation (it never had the name since its first status).
    CHECK(ls.seen<NotificationServerStatus>(became_active, 5000ms));
    CHECK(bstest::wait_until([&] { return survivor->capabilities().receives_foreign; }, 5000ms));
    r = run({"notify-send", "-p", "to survivor"}, bus.env());
    CHECK(ls.wait<NotificationPosted>([](const NotificationPosted& e) { return e.notification.summary == "to survivor"; }).has_value());

    // The bus going away is reported.
    bus.kill();
    CHECK(ls.seen<NotificationServerStatus>([](const NotificationServerStatus& s) {
        return !s.active && s.detail.find("lost") != std::string::npos;
    }, 5000ms));
    CHECK(!survivor->capabilities().receives_foreign);
}

// The session bus daemon restarts (same address, every connection dropped,
// every name released) under a server that owns the name and a second one
// queued behind it: both report the loss, reconnect, ask again, and exactly
// one ends up owning the name; notify-send on the new daemon reaches it.
void test_bus_restart() {
    bstest::PrivateBus bus;
    REQUIRE(bus.ok());
    NotificationServerConfig cfg;
    auto a = make_server(bus, cfg);
    REQUIRE(a);
    Log la(a->events());
    CHECK(la.wait<NotificationServerStatus>([](const NotificationServerStatus& s) { return s.active; }).has_value());
    auto b = make_server(bus, cfg);
    REQUIRE(b);
    Log lb(b->events());
    CHECK(lb.wait<NotificationServerStatus>([](const NotificationServerStatus& s) { return !s.active; }).has_value());
    auto r = run({"notify-send", "-p", "-t", "0", "before"}, bus.env());
    CHECK_EQ(r.exit_code, 0);
    CHECK(la.wait<NotificationPosted>([](const NotificationPosted& e) { return e.notification.summary == "before"; }));

    for (int round = 0; round < 2; ++round) {
        REQUIRE(bus.restart());
        auto lost = [](const NotificationServerStatus& s) { return !s.active && s.detail.find("lost") != std::string::npos; };
        // The owner reports the loss; the queued one was inactive already.
        CHECK(la.seen<NotificationServerStatus>(lost, 5000ms) || lb.seen<NotificationServerStatus>(lost, 0ms));
        CHECK(bstest::wait_until([&] {
            return a->capabilities().receives_foreign != b->capabilities().receives_foreign;
        }, 10000ms));
        std::this_thread::sleep_for(200ms);  // a second owner would show up now
        CHECK(a->capabilities().receives_foreign != b->capabilities().receives_foreign);
        NotificationServer& owner = a->capabilities().receives_foreign ? *a : *b;
        Log& lo = a->capabilities().receives_foreign ? la : lb;
        std::string summary = "after-" + std::to_string(round);
        r = run({"notify-send", "-p", summary}, bus.env());
        CHECK_EQ(r.exit_code, 0);
        auto p = lo.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.summary == summary; });
        REQUIRE(p.has_value());
        // CloseNotification over the new daemon closes it and signals.
        r = gdbus_call(bus, "CloseNotification", {std::to_string(p->notification.id)});
        CHECK_EQ(r.exit_code, 0);
        CHECK(lo.wait<NotificationClosed>([&](const NotificationClosed& c) { return c.id == p->notification.id; }));
        CHECK(owner.capabilities().receives_foreign);
        la.clear();
        lb.clear();
    }
    // Shown before the restarts, never expiring: still listed.
    auto act = a->active();
    CHECK(std::any_of(act.begin(), act.end(), [](const Notification& n) { return n.summary == "before"; }));
}

void test_history_and_dnd(const bstest::PrivateBus& bus) {
    NotificationServerConfig cfg;
    cfg.default_timeout_ms = 0;
    auto server = make_server(bus, cfg);
    REQUIRE(server);
    Log log(server->events());

    // Initially history is empty and DND is off
    CHECK(!server->is_do_not_disturb());
    CHECK(server->history().empty());

    // Post notification 1
    Notification n1;
    n1.app_name = "app1";
    n1.summary = "Meeting reminder";
    uint32_t id1 = server->post(n1);
    CHECK(id1 != 0);

    auto p1 = log.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.id == id1; });
    REQUIRE(p1);
    CHECK(!p1->popup_suppressed);

    // Check history has notification 1
    auto hist = server->history();
    REQUIRE(hist.size() == 1);
    CHECK_EQ(hist[0].id, id1);
    CHECK_EQ(hist[0].summary, std::string("Meeting reminder"));

    // Close notification 1
    CHECK(server->close(id1, CloseReason::Dismissed).ok);
    CHECK(server->active().empty());

    // In history, notification 1 is retained!
    hist = server->history();
    REQUIRE(hist.size() == 1);
    CHECK_EQ(hist[0].id, id1);

    // Post notification 2 via foreign client (notify-send)
    auto r = run({"notify-send", "-p", "Foreign alert"}, bus.env());
    CHECK_EQ(r.exit_code, 0);
    uint32_t id2 = parse_uint(r.out);
    CHECK(id2 != 0);
    auto p2 = log.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.id == id2; });
    REQUIRE(p2);
    CHECK(!p2->popup_suppressed);

    hist = server->history();
    REQUIRE(hist.size() == 2);

    // Remove notification 1 from history
    CHECK(server->remove_from_history(id1));
    hist = server->history();
    REQUIRE(hist.size() == 1);
    CHECK_EQ(hist[0].id, id2);

    // Remove non-existent ID
    CHECK(!server->remove_from_history(999999));

    // Clear history
    server->clear_history();
    CHECK(server->history().empty());

    // Test Do-Not-Disturb (DND)
    server->set_do_not_disturb(true);
    auto dnd_ev1 = log.wait_any<DoNotDisturbChanged>();
    REQUIRE(dnd_ev1);
    CHECK(dnd_ev1->enabled);
    CHECK(server->is_do_not_disturb());

    // Post notification while DND is active
    Notification n3;
    n3.app_name = "app3";
    n3.summary = "Quiet notification";
    uint32_t id3 = server->post(n3);
    CHECK(id3 != 0);

    auto p3 = log.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.id == id3; });
    REQUIRE(p3);
    CHECK(p3->popup_suppressed);

    // It is retained in history
    hist = server->history();
    REQUIRE(hist.size() == 1);
    CHECK_EQ(hist[0].id, id3);
    CHECK_EQ(hist[0].summary, std::string("Quiet notification"));

    // Foreign notification under DND
    r = run({"notify-send", "-p", "Foreign DND alert"}, bus.env());
    CHECK_EQ(r.exit_code, 0);
    uint32_t id4 = parse_uint(r.out);
    CHECK(id4 != 0);
    auto p4 = log.wait<NotificationPosted>([&](const NotificationPosted& e) { return e.notification.id == id4; });
    REQUIRE(p4);
    CHECK(p4->popup_suppressed);

    hist = server->history();
    REQUIRE(hist.size() == 2);

    // Turn DND off
    server->set_do_not_disturb(false);
    auto dnd_ev2 = log.wait_any<DoNotDisturbChanged>();
    REQUIRE(dnd_ev2);
    CHECK(!dnd_ev2->enabled);
    CHECK(!server->is_do_not_disturb());
}

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    for (const char* tool : {"notify-send", "gdbus", "stdbuf"})
        if (!bstest::have_program(tool))
            bstest::skip("test_notify_server", std::string(tool) + " is not installed (libnotify-bin / libglib2.0-bin)");
    {
        bstest::PrivateBus bus;
        if (!bus.ok()) bstest::skip("test_notify_server", bus.error());
    }
    // A fresh bus per area: every server owns the name from scratch.
    auto with_bus = [](void (*fn)(const bstest::PrivateBus&)) {
        bstest::PrivateBus bus;
        if (bus.ok()) fn(bus);
        else bstest::fail(__FILE__, __LINE__, "private bus: " + bus.error());
    };
    with_bus(test_notify_send);
    with_bus(test_expiry);
    with_bus(test_actions);
    with_bus(test_gdbus);
    with_bus(test_post);
    with_bus(test_history_and_dnd);
    test_names();
    test_bus_restart();
    return bstest::finish("test_notify_server");
}
