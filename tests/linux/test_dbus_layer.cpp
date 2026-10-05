// The sd-bus layer against a private dbus-daemon: object export (methods,
// properties, signals, introspection) checked with busctl and gdbus as
// independent clients, blocking / async / deferred calls, matches, timers,
// name ownership and replacement, fd passing, and disconnect handling.
#include "check.h"
#include "linux/dbus/connection.h"
#include "linux/support/private_bus.h"

#include <atomic>
#include <unistd.h>

using namespace brosys::dbus;
using namespace std::chrono_literals;
using bstest::run;

namespace {

constexpr const char* kService = "org.brosys.TestService";
constexpr const char* kPath = "/org/brosys/Test";
constexpr const char* kIface = "org.brosys.Test";

struct Server {
    std::atomic<uint32_t> counter{7};
    std::atomic<std::thread::id> handler_thread{};

    std::shared_ptr<Interface> make(Connection* conn) {
        auto i = std::make_shared<Interface>();
        i->name = kIface;
        i->methods.push_back({"Echo", "s", "s", {"text"}, {"echoed"}, [this](const MethodCall& c) {
                                  handler_thread = std::this_thread::get_id();
                                  return MethodResult::ok({Value::str(c.args[0].as_string())});
                              }});
        i->methods.push_back({"Add", "ii", "i", {"a", "b"}, {"sum"}, [](const MethodCall& c) {
                                  return MethodResult::ok({Value::i32(static_cast<int32_t>(c.args[0].as_int() + c.args[1].as_int()))});
                              }});
        i->methods.push_back({"Fail", "", "", {}, {}, [](const MethodCall&) {
                                  return MethodResult::error("org.brosys.Error.Nope", "nope");
                              }});
        i->methods.push_back({"Defer", "u", "u", {}, {}, [](const MethodCall& c) {
                                  auto d = c.defer();
                                  uint32_t v = static_cast<uint32_t>(c.args[0].as_uint());
                                  std::thread([d, v] {
                                      std::this_thread::sleep_for(50ms);
                                      d->reply({Value::u32(v * 2)});
                                  }).detach();
                                  return MethodResult::deferred();
                              }});
        i->methods.push_back({"Complex", "a{sv}", "(sa{sv})", {}, {}, [](const MethodCall& c) {
                                  std::vector<std::pair<std::string, Value>> out;
                                  for (auto& e : c.args[0].items())
                                      out.emplace_back("x-" + e.items()[0].as_string(), e.items()[1]);
                                  return MethodResult::ok({Value::structure({Value::str("ok"), Value::vardict(out)})});
                              }});
        i->methods.push_back({"Length", "ay", "u", {}, {}, [](const MethodCall& c) {
                                  auto* b = c.args[0].as_bytes();
                                  uint32_t sum = 0;
                                  if (b)
                                      for (auto x : *b) sum += x;
                                  return MethodResult::ok({Value::u32(sum)});
                              }});
        i->methods.push_back({"Pid", "", "u", {}, {}, [](const MethodCall& c) {
                                  return MethodResult::ok({Value::u32(c.sender_pid())});
                              }});
        i->methods.push_back({"Pipe", "", "h", {}, {}, [](const MethodCall&) {
                                  int fds[2];
                                  if (pipe(fds) != 0) return MethodResult::error(kErrorFailed, "pipe");
                                  ssize_t n = write(fds[1], "fd!", 3);
                                  (void)n;
                                  close(fds[1]);
                                  return MethodResult::ok({Value::fd(UnixFd::adopt(fds[0]))});
                              }});
        i->methods.push_back({"Bad", "", "s", {}, {}, [](const MethodCall&) {
                                  return MethodResult::ok({Value::u32(1)});  // wrong out type on purpose
                              }});
        i->properties.push_back({"Counter", "u", [this] { return Value::u32(counter.load()); },
                                 [this](const Value& v) {
                                     counter = static_cast<uint32_t>(v.as_uint());
                                     return MethodResult::ok();
                                 },
                                 "true"});
        i->properties.push_back({"Name", "s", [] { return Value::str("brosys"); }, nullptr, "const"});
        i->signals.push_back({"Ping", "su", {"text", "n"}});
        (void)conn;
        return i;
    }
};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

void test_export_and_call(const bstest::PrivateBus& bus) {
    std::string err;
    auto server = Connection::open(BusKind::Session, bus.address(), "server", &err);
    REQUIRE(server);
    auto client = Connection::open(BusKind::Session, bus.address(), "client", &err);
    REQUIRE(client);
    Server s;
    REQUIRE(server->export_interface(kPath, s.make(server.get()), &err));
    CHECK(server->request_name(kService, 0, &err) == NameRequest::PrimaryOwner);

    // Blocking calls from a foreign thread.
    Reply r = client->call(kService, kPath, kIface, "Echo", {Value::str("héllo")});
    CHECK(r.ok);
    CHECK_EQ(r.first() ? r.first()->as_string() : "", std::string("héllo"));
    CHECK(s.handler_thread.load() != std::this_thread::get_id());

    r = client->call(kService, kPath, kIface, "Add", {Value::i32(40), Value::i32(2)});
    CHECK(r.ok && r.first() && r.first()->as_int() == 42);

    r = client->call(kService, kPath, kIface, "Fail");
    CHECK(!r.ok);
    CHECK_EQ(r.error_name, std::string("org.brosys.Error.Nope"));

    r = client->call(kService, kPath, kIface, "Add", {Value::str("x")});
    CHECK(!r.ok);
    CHECK_EQ(r.error_name, std::string(kErrorInvalidArgs));

    r = client->call(kService, kPath, kIface, "Nope");
    CHECK_EQ(r.error_name, std::string(kErrorUnknownMethod));

    r = client->call(kService, kPath, kIface, "Bad");
    CHECK(!r.ok);

    r = client->call(kService, kPath, kIface, "Defer", {Value::u32(21)});
    CHECK(r.ok && r.first() && r.first()->as_uint() == 42);

    r = client->call(kService, kPath, kIface, "Complex",
                     {Value::vardict({{"a", Value::i32(1)}, {"b", Value::strings({"p", "q"})}})});
    REQUIRE(r.ok && r.first());
    auto& st = r.first()->items();
    REQUIRE(st.size() == 2);
    CHECK_EQ(st[0].as_string(), std::string("ok"));
    CHECK(st[1].lookup("x-a") && st[1].lookup("x-a")->as_int() == 1);
    CHECK(st[1].lookup("x-b") && st[1].lookup("x-b")->as_strings().size() == 2);

    std::vector<uint8_t> big(1 << 20, 1);
    r = client->call(kService, kPath, kIface, "Length", {Value::bytes(big)});
    CHECK(r.ok && r.first() && r.first()->as_uint() == (1u << 20));

    r = client->call(kService, kPath, kIface, "Pid");
    CHECK(r.ok && r.first() && r.first()->as_uint() == static_cast<uint64_t>(getpid()));

    r = client->call(kService, kPath, kIface, "Pipe");
    REQUIRE(r.ok && r.first());
    char buf[8] = {};
    CHECK(read(r.first()->as_fd(), buf, sizeof buf) == 3);
    CHECK_EQ(std::string(buf, 3), std::string("fd!"));

    // Properties.
    r = client->get_property(kService, kPath, kIface, "Counter");
    CHECK(r.ok && r.first() && r.first()->as_uint() == 7);
    auto all = client->get_all_properties(kService, kPath, kIface);
    CHECK_EQ(all.size(), size_t(2));
    CHECK_EQ(all["Name"].as_string(), std::string("brosys"));
    r = client->call(kService, kPath, "org.freedesktop.DBus.Properties", "Set",
                     {Value::str(kIface), Value::str("Name"), Value::variant(Value::str("x"))});
    CHECK_EQ(r.error_name, std::string(kErrorPropertyReadOnly));

    // Async call: the handler runs on the client's bus thread.
    std::promise<std::pair<bool, bool>> done;
    client->call_async(kService, kPath, kIface, "Echo", {Value::str("a")}, [&](Reply rr) {
        done.set_value({rr.ok, client->on_bus_thread()});
    });
    auto res = done.get_future().get();
    CHECK(res.first);
    CHECK(res.second);

    // ---- independent clients: busctl and gdbus.
    if (bstest::have_program("busctl")) {
        auto b = run({"busctl", "--address=" + bus.address(), "call", kService, kPath, kIface, "Add", "ii", "2", "3"});
        CHECK_EQ(b.exit_code, 0);
        CHECK_EQ(trim(b.out), std::string("i 5"));
        b = run({"busctl", "--address=" + bus.address(), "introspect", kService, kPath, kIface});
        CHECK_EQ(b.exit_code, 0);
        CHECK(b.out.find(".Complex") != std::string::npos && b.out.find("a{sv}") != std::string::npos);
        CHECK(b.out.find(".Counter") != std::string::npos && b.out.find("writable") != std::string::npos);
        CHECK(b.out.find(".Ping") != std::string::npos);
        b = run({"busctl", "--address=" + bus.address(), "set-property", kService, kPath, kIface, "Counter", "u", "99"});
        CHECK_EQ(b.exit_code, 0);
        CHECK_EQ(s.counter.load(), 99u);
        b = run({"busctl", "--address=" + bus.address(), "get-property", kService, kPath, kIface, "Counter"});
        CHECK_EQ(trim(b.out), std::string("u 99"));
        b = run({"busctl", "--address=" + bus.address(), "tree", kService});
        CHECK(b.out.find("/org/brosys/Test") != std::string::npos);
    } else {
        std::printf("note: busctl missing, skipped busctl checks\n");
    }
    if (bstest::have_program("gdbus")) {
        auto g = run({"gdbus", "call", "--address", bus.address(), "--dest", kService, "--object-path", kPath,
                      "--method", std::string(kIface) + ".Echo", "from-gdbus"});
        CHECK_EQ(g.exit_code, 0);
        CHECK_EQ(trim(g.out), std::string("('from-gdbus',)"));
        g = run({"gdbus", "introspect", "--address", bus.address(), "--dest", kService, "--object-path", kPath});
        CHECK_EQ(g.exit_code, 0);
        CHECK(g.out.find("Defer(in  u arg_0") != std::string::npos || g.out.find("Defer(") != std::string::npos);
        CHECK(g.out.find("readwrite u Counter") != std::string::npos);
        g = run({"gdbus", "call", "--address", bus.address(), "--dest", kService, "--object-path", kPath, "--method",
                 std::string(kIface) + ".Fail"});
        CHECK(g.exit_code != 0);
        CHECK(g.err.find("org.brosys.Error.Nope") != std::string::npos);
    } else {
        std::printf("note: gdbus missing, skipped gdbus checks\n");
    }
}

void test_signals_and_timers(const bstest::PrivateBus& bus) {
    std::string err;
    auto server = Connection::open(BusKind::Session, bus.address(), "sig-server", &err);
    auto client = Connection::open(BusKind::Session, bus.address(), "sig-client", &err);
    REQUIRE(server && client);
    Server s;
    REQUIRE(server->export_interface(kPath, s.make(server.get()), &err));

    std::mutex mu;
    std::vector<Message> got;
    uint64_t id = client->add_match("type='signal',interface='org.brosys.Test'", [&](const Message& m) {
        std::lock_guard<std::mutex> lock(mu);
        got.push_back(m);
    });
    CHECK(id != 0);
    uint64_t pc = client->add_match("type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                                    [&](const Message& m) {
                                        std::lock_guard<std::mutex> lock(mu);
                                        got.push_back(m);
                                    });
    CHECK(pc != 0);
    CHECK(server->emit_signal(kPath, kIface, "Ping", {Value::str("one"), Value::u32(1)}));
    s.counter = 5;
    CHECK(server->emit_properties_changed(kPath, kIface, {"Counter", "Name"}));
    if (bstest::have_program("gdbus")) {
        // --session (not --address): gdbus only registers with the bus (Hello) for
        // well-known buses, and an unregistered sender's broadcasts are not routed.
        auto g = run({"gdbus", "emit", "--session", "--object-path", "/x", "--signal",
                      std::string(kIface) + ".Ping", "'two'", "uint32 2"},
                     bus.env());
        CHECK_EQ(g.exit_code, 0);
    }
    size_t want = bstest::have_program("gdbus") ? 3 : 2;
    CHECK(bstest::wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return got.size() >= want;
    }, 5000ms));
    {
        std::lock_guard<std::mutex> lock(mu);
        REQUIRE(got.size() >= 2);
        CHECK_EQ(got[0].member, std::string("Ping"));
        CHECK_EQ(got[0].sender, server->unique_name());
        CHECK(got[0].args.size() == 2 && got[0].args[0].as_string() == "one" && got[0].args[1].as_uint() == 1);
        CHECK_EQ(got[1].member, std::string("PropertiesChanged"));
        REQUIRE(got[1].args.size() == 3);
        CHECK(got[1].args[1].lookup("Counter") && got[1].args[1].lookup("Counter")->as_uint() == 5);
        CHECK(!got[1].args[1].lookup("Name"));  // const: never emitted
        if (want == 3) CHECK(got.size() >= 3 && got[2].args.size() == 2 && got[2].args[0].as_string() == "two");
    }

    // Timers fire in due order on the bus thread; cancelled ones never fire.
    std::vector<int> order;
    std::atomic<int> fired{0};
    client->add_timer(60ms, [&] { order.push_back(2); ++fired; });
    client->add_timer(10ms, [&] { order.push_back(1); ++fired; });
    uint64_t cancelled = client->add_timer(30ms, [&] { order.push_back(99); ++fired; });
    client->cancel_timer(cancelled);
    CHECK(bstest::wait_until([&] { return fired.load() >= 2; }, 3000ms));
    std::this_thread::sleep_for(50ms);
    client->run_sync([&] {
        CHECK_EQ(order.size(), size_t(2));
        if (order.size() == 2) CHECK(order[0] == 1 && order[1] == 2);
    });
}

void test_names(const bstest::PrivateBus& bus) {
    std::string err;
    auto a = Connection::open(BusKind::Session, bus.address(), "name-a", &err);
    auto b = Connection::open(BusKind::Session, bus.address(), "name-b", &err);
    REQUIRE(a && b);
    std::mutex mu;
    std::vector<std::pair<std::string, bool>> a_events, b_events;
    a->set_name_handler([&](const std::string& n, bool acq) {
        std::lock_guard<std::mutex> lock(mu);
        a_events.emplace_back(n, acq);
    });
    b->set_name_handler([&](const std::string& n, bool acq) {
        std::lock_guard<std::mutex> lock(mu);
        b_events.emplace_back(n, acq);
    });
    const std::string name = "org.brosys.Names";
    CHECK(a->request_name(name, name_flags::AllowReplacement, &err) == NameRequest::PrimaryOwner);
    // Re-requesting updates the flags, so keep AllowReplacement.
    CHECK(a->request_name(name, name_flags::AllowReplacement, &err) == NameRequest::AlreadyOwner);
    CHECK(b->request_name(name, 0, &err) == NameRequest::Exists);
    CHECK_EQ(b->get_name_owner(name), a->unique_name());
    auto names = b->list_names();
    CHECK(std::find(names.begin(), names.end(), name) != names.end());

    std::mutex omu;
    std::vector<std::string> owners;
    CHECK(b->watch_name_owner(name, [&](const std::string&, const std::string&, const std::string& now) {
        std::lock_guard<std::mutex> lock(omu);
        owners.push_back(now);
    }) != 0);
    // Replacement: a allowed it, b asks for it.
    CHECK(b->request_name(name, name_flags::ReplaceExisting, &err) == NameRequest::PrimaryOwner);
    CHECK(bstest::wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        for (auto& [n, acq] : a_events)
            if (n == name && !acq) return true;
        return false;
    }, 5000ms));
    CHECK(bstest::wait_until([&] {
        std::lock_guard<std::mutex> lock(omu);
        return !owners.empty() && owners.back() == b->unique_name();
    }, 5000ms));
    // a queues, b releases, a gets it back.
    CHECK(a->request_name(name, name_flags::Queue, &err) == NameRequest::InQueue);
    CHECK(b->release_name(name));
    CHECK(bstest::wait_until([&] { return a->get_name_owner(name) == a->unique_name(); }, 5000ms));
    CHECK(bstest::wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return !a_events.empty() && a_events.back().first == name && a_events.back().second;
    }, 5000ms));
}

void test_disconnect() {
    bstest::PrivateBus bus;
    REQUIRE(bus.ok());
    std::string err;
    auto c = Connection::open(BusKind::Session, bus.address(), "doomed", &err);
    REQUIRE(c);
    std::atomic<bool> dropped{false};
    c->set_disconnect_handler([&] { dropped = true; });
    bus.kill();
    CHECK(bstest::wait_until([&] { return dropped.load(); }, 5000ms));
    CHECK(!c->connected());
    auto t0 = std::chrono::steady_clock::now();
    Reply r = c->call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames");
    CHECK(!r.ok);
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
}

void test_values() {
    Value v = Value::vardict({{"k", Value::u32(3)}, {"img", Value::structure({Value::i32(1), Value::bytes({1, 2})})}});
    CHECK_EQ(v.sig, std::string("a{sv}"));
    CHECK(v.lookup("k") && v.lookup("k")->as_uint() == 3);
    CHECK(v.lookup("img") && v.lookup("img")->sig == "(iay)");
    CHECK(!v.lookup("nope"));
    auto parts = split_signature("sa{sv}(iiay)u");
    CHECK_EQ(parts.size(), size_t(4));
    if (parts.size() == 4) CHECK(parts[1] == "a{sv}" && parts[2] == "(iiay)");
    CHECK_EQ(Value::i32(-1).to_uint().has_value(), false);
    CHECK_EQ(Value::u64(5).as_int(), int64_t(5));
    CHECK_EQ(Value::array("y", {Value::byte(1), Value::byte(2)}).sig, std::string("ay"));
    CHECK_EQ(Value::array("s", {}).sig, std::string("as"));
}

}  // namespace

int main() {
    test_values();
    bstest::PrivateBus bus;
    if (!bus.ok()) bstest::skip("test_dbus_layer", bus.error());
    test_export_and_call(bus);
    test_signals_and_timers(bus);
    test_names(bus);
    test_disconnect();
    return bstest::finish("test_dbus_layer");
}
