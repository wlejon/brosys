// The tray host against a third-party item: libayatana-appindicator3 with a
// GTK menu exported by libdbusmenu, running under Xvfb on a private bus.
// Skips (77) when the helper could not be built (no
// libayatana-appindicator3-dev at configure time) or Xvfb is missing.
#include "check.h"
#include "brosys/tray.h"
#include "linux/desktop/event_log.h"
#include "linux/support/private_bus.h"

#include <csignal>

using namespace brosys;
using namespace std::chrono_literals;
using Log = bstest::EventLog<TrayEvent>;

namespace {

const MenuItem* find_label(const MenuItem& m, const std::string& label) {
    for (auto& c : m.children) {
        if (c.label == label) return &c;
        if (auto* d = find_label(c, label)) return d;
    }
    return nullptr;
}

void test_ayatana(const bstest::PrivateBus& bus, const std::string& display) {
    TrayConfig cfg;
    cfg.session_bus_address = bus.address();
    std::string err;
    auto host = TrayHost::create(cfg, &err);
    REQUIRE(host);
    Log log(host->events());
    auto st = log.wait_any<TrayHostStatus>();
    REQUIRE(st && st->role == TrayRole::Watcher);

    bstest::Env env = bus.env();
    env["DISPLAY"] = display;
    env["GDK_BACKEND"] = "x11";
    env["NO_AT_BRIDGE"] = "1";
    env["GSETTINGS_BACKEND"] = "memory";
    env["WAYLAND_DISPLAY"] = "";
    env["ASAN_OPTIONS"] = "detect_leaks=0";  // GTK's process-lifetime allocations (sanitizer builds)
    bstest::Daemon app({BROSYS_AYATANA_ITEM}, env);
    REQUIRE(app.wait_for_line("READY", 20000ms));
    CHECK(app.wait_for_line("CONNECTED 1", 20000ms));

    auto added = log.wait<TrayItemAdded>([](const TrayItemAdded& e) { return e.item.app_id == "brosys-ayatana"; }, 20000ms);
    REQUIRE(added);
    const TrayItem& it = added->item;
    std::printf("ayatana item: %s\n", it.id.c_str());
    CHECK(it.id.find("/org/ayatana/NotificationItem/") != std::string::npos);
    CHECK_EQ(it.title, std::string("Ayatana Title"));
    CHECK_EQ(it.icon.name, std::string("dialog-information"));
    CHECK_EQ(it.attention_icon.name, std::string("dialog-error"));
    CHECK(it.category == TrayCategory::Communications);
    CHECK(it.status == TrayItemStatus::Active);
    CHECK(it.has_menu);
    CHECK_EQ(it.pid, static_cast<uint32_t>(app.pid()));

    // libdbusmenu-gtk's export of the GTK menu.
    std::optional<MenuItem> root;
    CHECK(bstest::wait_until([&] {
        auto m = log.wait<TrayMenuChanged>([&](const TrayMenuChanged& e) { return e.item_id == it.id; }, 200ms);
        if (m) root = m->root;
        return root && find_label(*root, "Hello") && find_label(*root, "Toggle");
    }, 15000ms));
    REQUIRE(root);
    const MenuItem* hello = find_label(*root, "Hello");
    const MenuItem* toggle = find_label(*root, "Toggle");
    const MenuItem* disabled = find_label(*root, "Disabled");
    REQUIRE(hello && toggle && disabled);
    CHECK(toggle->toggle == MenuToggle::Checkmark && toggle->toggle_state == 1);
    CHECK(!disabled->enabled);
    bool has_separator = false;
    for (auto& c : root->children) has_separator |= c.separator;
    CHECK(has_separator);
    CHECK(host->menu(it.id).has_value());

    CHECK(host->menu_about_to_show(it.id, 0).ok);
    CHECK(host->menu_event(it.id, hello->id, MenuEventType::Clicked).ok);
    CHECK(app.wait_for_line("CLICKED Hello", 10000ms));
    // SecondaryActivate -> the secondary activate target (Hello).
    CHECK(host->secondary_activate(it.id, 1, 1).ok);
    CHECK(app.wait_for_line("CLICKED Hello", 10000ms));
    CHECK(host->scroll(it.id, 3, ScrollOrientation::Vertical).ok);
    CHECK(app.wait_for_line("SCROLL 3", 10000ms));

    // Icon / title / status through the New* signals.
    kill(app.pid(), SIGUSR1);
    CHECK(app.wait_for_line("CHANGED", 10000ms));
    CHECK(bstest::wait_until([&] {
        log.wait<TrayItemChanged>([](const TrayItemChanged&) { return true; }, 100ms);
        auto v = host->items();
        return v.size() == 1 && v[0].title == "Ayatana Changed" && v[0].icon.name == "dialog-warning" &&
               v[0].status == TrayItemStatus::NeedsAttention;
    }, 10000ms));

    // A menu item added later reaches us through LayoutUpdated.
    kill(app.pid(), SIGUSR2);
    CHECK(app.wait_for_line("MENU-ADDED", 10000ms));
    CHECK(bstest::wait_until([&] {
        log.wait<TrayMenuChanged>([](const TrayMenuChanged&) { return true; }, 100ms);
        auto m = host->menu(it.id);
        return m && find_label(*m, "Added Later");
    }, 10000ms));

    app.stop();
    CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& e) { return e.id == it.id; }, 10000ms).has_value());
}

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    std::string helper = BROSYS_AYATANA_ITEM;
    if (helper.empty())
        bstest::skip("test_tray_ayatana", "libayatana-appindicator3-dev (ayatana-appindicator3-0.1) was not found at configure time");
    if (!bstest::have_program("Xvfb")) bstest::skip("test_tray_ayatana", "Xvfb is not installed (xvfb)");
    bstest::PrivateBus bus;
    if (!bus.ok()) bstest::skip("test_tray_ayatana", bus.error());
    bstest::Daemon xvfb({"Xvfb", "-displayfd", "1", "-nolisten", "tcp", "-screen", "0", "640x480x24"});
    std::string number;
    if (!xvfb.read_line(number, 20000ms) || number.empty()) {
        bstest::fail(__FILE__, __LINE__, "Xvfb did not report a display");
        return bstest::finish("test_tray_ayatana");
    }
    test_ayatana(bus, ":" + number);
    return bstest::finish("test_tray_ayatana");
}
