// Test for ScreenSaver / Idle Inhibit Provider (org.freedesktop.ScreenSaver).
//
// Tests session bus export of org.freedesktop.ScreenSaver methods
// (Inhibit, UnInhibit, GetActive, Throttle, UnThrottle, SimulateUserActivity)
// and their mapping to logind's idle inhibitor on the system bus.
#include "check.h"
#include "brosys/power.h"
#include "linux/dbus/connection.h"
#include "linux/fakes/fake_logind.h"
#include "linux/support/private_bus.h"
#include "linux/support/proc.h"

#include <csignal>
#include <thread>

using namespace brosys;
using namespace std::chrono_literals;

namespace {

constexpr const char* kDest = "org.freedesktop.ScreenSaver";
constexpr const char* kPath = "/org/freedesktop/ScreenSaver";
constexpr const char* kAltPath = "/ScreenSaver";

bstest::RunResult gdbus_call(const bstest::PrivateBus& bus, const std::string& path,
                             const std::string& method, std::vector<std::string> args) {
    std::vector<std::string> argv{"gdbus", "call", "--session", "--dest", kDest, "--object-path", path,
                                  "--method", std::string("org.freedesktop.ScreenSaver.") + method};
    for (auto& a : args) argv.push_back(a);
    return bstest::run(argv, bus.env());
}

void test_screensaver_lifecycle() {
    bstest::PrivateBus session_bus;
    REQUIRE(session_bus.ok());
    bstest::PrivateBus system_bus;
    REQUIRE(system_bus.ok());

    bstest::FakeLogind fake_logind(system_bus.address(), bstest::FakeLogindConfig{});
    REQUIRE(fake_logind.ok());

    ScreenSaverConfig cfg;
    cfg.session_bus_address = session_bus.address();
    cfg.system_bus_address = system_bus.address();

    std::string err;
    auto server = ScreenSaverServer::create(cfg, &err);
    REQUIRE(server);
    CHECK_EQ(server->active_inhibitions(), 0u);

    // Client connection to hold open during inhibition tests
    std::string client_err;
    auto client = dbus::Connection::open(dbus::BusKind::Session, session_bus.address(), "screensaver-test-client", &client_err);
    REQUIRE(client);

    // 1. GetActive
    auto r = gdbus_call(session_bus, kPath, "GetActive", {});
    CHECK_EQ(r.exit_code, 0);
    CHECK(r.out.find("false") != std::string::npos);

    server->set_active(true);
    CHECK(server->is_active());
    r = gdbus_call(session_bus, kPath, "GetActive", {});
    CHECK_EQ(r.exit_code, 0);
    CHECK(r.out.find("true") != std::string::npos);

    server->set_active(false);
    CHECK(!server->is_active());
    r = gdbus_call(session_bus, kPath, "GetActive", {});
    CHECK(r.out.find("false") != std::string::npos);

    // 2. Inhibit and UnInhibit
    auto reply = client->call(kDest, kPath, "org.freedesktop.ScreenSaver", "Inhibit",
                              {dbus::Value::str("vlc"), dbus::Value::str("playing movie")});
    REQUIRE(reply.ok);
    REQUIRE(!reply.values.empty());
    uint32_t cookie = static_cast<uint32_t>(reply.values[0].as_uint());
    CHECK(cookie != 0);
    CHECK_EQ(server->active_inhibitions(), 1u);

    // Verify logind received Inhibit("idle", "vlc", "playing movie", "block")
    CHECK(fake_logind.open_inhibitors("vlc") == 1);
    bool called_inhibit = false;
    for (auto& c : fake_logind.calls()) {
        if (c.find("Inhibit(idle,vlc,playing movie,block)") != std::string::npos)
            called_inhibit = true;
    }
    CHECK(called_inhibit);

    // UnInhibit
    reply = client->call(kDest, kPath, "org.freedesktop.ScreenSaver", "UnInhibit", {dbus::Value::u32(cookie)});
    CHECK(reply.ok);
    CHECK_EQ(server->active_inhibitions(), 0u);
    CHECK(bstest::wait_until([&] { return fake_logind.open_inhibitors("vlc") == 0; }, 3000ms));

    // 3. Throttle and UnThrottle
    reply = client->call(kDest, kPath, "org.freedesktop.ScreenSaver", "Throttle",
                         {dbus::Value::str("game"), dbus::Value::str("rendering")});
    REQUIRE(reply.ok);
    REQUIRE(!reply.values.empty());
    uint32_t throttle_cookie = static_cast<uint32_t>(reply.values[0].as_uint());
    CHECK(throttle_cookie != 0);
    CHECK_EQ(server->active_inhibitions(), 1u);
    CHECK(fake_logind.open_inhibitors("game") == 1);

    reply = client->call(kDest, kPath, "org.freedesktop.ScreenSaver", "UnThrottle", {dbus::Value::u32(throttle_cookie)});
    CHECK(reply.ok);
    CHECK_EQ(server->active_inhibitions(), 0u);
    CHECK(bstest::wait_until([&] { return fake_logind.open_inhibitors("game") == 0; }, 3000ms));

    // 4. SimulateUserActivity
    r = gdbus_call(session_bus, kPath, "SimulateUserActivity", {});
    CHECK_EQ(r.exit_code, 0);
    CHECK(server->simulate_user_activity().ok);

    // 5. Alternate object path /ScreenSaver
    reply = client->call(kDest, kAltPath, "org.freedesktop.ScreenSaver", "Inhibit",
                         {dbus::Value::str("mpv"), dbus::Value::str("video stream")});
    REQUIRE(reply.ok);
    REQUIRE(!reply.values.empty());
    uint32_t alt_cookie = static_cast<uint32_t>(reply.values[0].as_uint());
    CHECK(alt_cookie != 0);
    CHECK_EQ(server->active_inhibitions(), 1u);
    CHECK(fake_logind.open_inhibitors("mpv") == 1);

    reply = client->call(kDest, kAltPath, "org.freedesktop.ScreenSaver", "UnInhibit", {dbus::Value::u32(alt_cookie)});
    CHECK(reply.ok);
    CHECK_EQ(server->active_inhibitions(), 0u);
    CHECK(bstest::wait_until([&] { return fake_logind.open_inhibitors("mpv") == 0; }, 3000ms));

    // 6. Automatic cleanup when client disconnects
    {
        std::string temp_err;
        auto temp_client = dbus::Connection::open(dbus::BusKind::Session, session_bus.address(), "client-temp", &temp_err);
        REQUIRE(temp_client);
        auto crash_reply = temp_client->call(kDest, kPath, "org.freedesktop.ScreenSaver", "Inhibit",
                                             {dbus::Value::str("temp-app"), dbus::Value::str("testing crash")});
        REQUIRE(crash_reply.ok);
        REQUIRE(!crash_reply.values.empty());
        uint32_t crash_cookie = static_cast<uint32_t>(crash_reply.values[0].as_uint());
        CHECK(crash_cookie != 0);
        CHECK_EQ(server->active_inhibitions(), 1u);
        CHECK(fake_logind.open_inhibitors("temp-app") == 1);

        // Client drops connection
        temp_client.reset();
    }

    // Server should notice the client name disappeared and release the inhibitor
    CHECK(bstest::wait_until([&] {
        return server->active_inhibitions() == 0 && fake_logind.open_inhibitors("temp-app") == 0;
    }, 5000ms));
}

void test_screensaver_power_service_integration() {
    bstest::PrivateBus session_bus;
    REQUIRE(session_bus.ok());
    bstest::PrivateBus system_bus;
    REQUIRE(system_bus.ok());

    bstest::FakeLogind fake_logind(system_bus.address(), bstest::FakeLogindConfig{});
    REQUIRE(fake_logind.ok());

    PowerConfig cfg;
    cfg.system_bus_address = system_bus.address();
    cfg.session_bus_address = session_bus.address();
    cfg.export_screensaver = true;

    std::string err;
    auto power = PowerService::create(cfg, &err);
    REQUIRE(power);
    REQUIRE(power->screensaver() != nullptr);

    // Call Inhibit through the session bus exported by PowerService
    std::string client_err;
    auto client = dbus::Connection::open(dbus::BusKind::Session, session_bus.address(), "power-client", &client_err);
    REQUIRE(client);

    auto reply = client->call(kDest, kPath, "org.freedesktop.ScreenSaver", "Inhibit",
                              {dbus::Value::str("player"), dbus::Value::str("stream")});
    REQUIRE(reply.ok);
    REQUIRE(!reply.values.empty());
    uint32_t cookie = static_cast<uint32_t>(reply.values[0].as_uint());
    CHECK(cookie != 0);
    CHECK_EQ(power->screensaver()->active_inhibitions(), 1u);
    CHECK(fake_logind.open_inhibitors("player") == 1);

    reply = client->call(kDest, kPath, "org.freedesktop.ScreenSaver", "UnInhibit", {dbus::Value::u32(cookie)});
    CHECK(reply.ok);
    CHECK_EQ(power->screensaver()->active_inhibitions(), 0u);
    CHECK(bstest::wait_until([&] { return fake_logind.open_inhibitors("player") == 0; }, 3000ms));
}

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    if (!bstest::have_program("gdbus"))
        bstest::skip("test_screensaver", "gdbus is not installed");

    test_screensaver_lifecycle();
    test_screensaver_power_service_integration();
    return bstest::finish("test_screensaver");
}
