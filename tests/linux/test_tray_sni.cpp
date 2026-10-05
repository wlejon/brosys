// The Linux tray host on a private bus with real item processes
// (brosys_sni_item: an SNI item + dbusmenu that registers by object path or
// by bus name and changes itself on command), checked from both sides: our
// events and the item's printed view of our calls. busctl / gdbus inspect
// the watcher as ordinary clients. The WatcherClient role runs against a
// second brosys process (brosys_tray_host) owning the watcher name, then
// takes the name over when that process dies.
#include "check.h"
#include "brosys/tray.h"
#include "linux/desktop/event_log.h"
#include "linux/desktop/signal_log.h"
#include "linux/support/private_bus.h"

#include <algorithm>
#include <csignal>
#include <sstream>
#include <unistd.h>

using namespace brosys;
using namespace std::chrono_literals;
using bstest::run;
using brosys::dbus::Value;
using Log = bstest::EventLog<TrayEvent>;

namespace {

constexpr const char* kWatcher = "org.kde.StatusNotifierWatcher";

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

struct ItemProc {
    std::unique_ptr<bstest::Daemon> proc;
    std::string unique, id;

    static ItemProc start(const bstest::PrivateBus& bus, const std::string& mode, bool expect_registered = true,
                          const std::vector<std::string>& extra = {}) {
        ItemProc p;
        std::vector<std::string> argv{BROSYS_SNI_ITEM, "--bus", bus.address(), "--mode", mode};
        argv.insert(argv.end(), extra.begin(), extra.end());
        p.proc = std::make_unique<bstest::Daemon>(argv);
        std::string line;
        if (!p.proc->wait_for_line("READY ", 10000ms, &line)) return p;
        std::istringstream in(line);
        std::string tag;
        in >> tag >> p.unique >> p.id;
        if (expect_registered && !p.proc->wait_for_line("REGISTERED", 10000ms)) p.id.clear();
        return p;
    }
    bool ok() const { return !id.empty(); }
};

struct Control {
    std::unique_ptr<dbus::Connection> conn;
    explicit Control(const bstest::PrivateBus& bus) {
        std::string err;
        conn = dbus::Connection::open(dbus::BusKind::Session, bus.address(), "control", &err);
    }
    bool call(const ItemProc& item, const char* method, dbus::Args args = dbus::Args()) {
        return conn->call(item.unique, "/control", "org.brosys.TestItem", method, args).ok;
    }
};

std::unique_ptr<TrayHost> make_host(const bstest::PrivateBus& bus, bool become_watcher = true) {
    TrayConfig cfg;
    cfg.session_bus_address = bus.address();
    cfg.become_watcher = become_watcher;
    std::string err;
    auto h = TrayHost::create(cfg, &err);
    if (!h) std::fprintf(stderr, "TrayHost::create: %s\n", err.c_str());
    return h;
}

std::function<bool(const TrayItemChanged&)> changed(const std::string& id) {
    return [id](const TrayItemChanged& e) { return e.item.id == id; };
}
std::function<bool(const TrayMenuChanged&)> menu_of(const std::string& id) {
    return [id](const TrayMenuChanged& e) { return e.item_id == id; };
}

const MenuItem* child(const MenuItem& m, int32_t id) {
    for (auto& c : m.children)
        if (c.id == id) return &c;
    return nullptr;
}

bool registered_items_contain(const bstest::PrivateBus& bus, const std::string& id) {
    auto r = run({"busctl", "--address=" + bus.address(), "get-property", kWatcher, "/StatusNotifierWatcher", kWatcher,
                  "RegisteredStatusNotifierItems"});
    return r.out.find("\"" + id + "\"") != std::string::npos;
}

// ---------------------------------------------------------------- Watcher role

void check_item_properties(const TrayItem& it, const ItemProc& item) {
    CHECK_EQ(it.app_id, std::string("brosys-test-item"));
    CHECK_EQ(it.title, std::string("Test Item"));
    CHECK(it.category == TrayCategory::Communications);
    CHECK(it.status == TrayItemStatus::Active);
    CHECK_EQ(it.icon.name, std::string("mail-unread"));
    CHECK_EQ(it.icon.theme_path, std::string("/tmp/brosys-icons"));
    REQUIRE(it.icon.pixmaps.size() == 1);
    CHECK(it.icon.pixmaps[0].width == 2 && it.icon.pixmaps[0].height == 1);
    CHECK(it.icon.pixmaps[0].rgba == (std::vector<uint8_t>{0xFF, 0, 0, 0xFF, 0, 0, 0xFF, 0x80}));
    CHECK_EQ(it.attention_icon.name, std::string("mail-attention"));
    CHECK(it.overlay_icon.empty());
    CHECK_EQ(it.tooltip.icon.name, std::string("tip-icon"));
    CHECK_EQ(it.tooltip.title, std::string("Tip title"));
    CHECK_EQ(it.tooltip.body, std::string("Tip <b>body</b>"));
    CHECK(it.has_menu);
    CHECK(!it.item_is_menu);
    CHECK_EQ(it.window_id, uint64_t(42));
    CHECK_EQ(it.pid, static_cast<uint32_t>(item.proc->pid()));
}

void check_menu(const MenuItem& root) {
    CHECK(root.id == 0 && root.has_submenu);
    REQUIRE(root.children.size() == 6);
    const MenuItem* open = child(root, 1);
    REQUIRE(open);
    CHECK_EQ(open->label, std::string("_Open"));
    CHECK_EQ(open->icon_name, std::string("document-open"));
    CHECK(open->shortcut == (std::vector<std::vector<std::string>>{{"Control", "o"}}));
    CHECK(child(root, 2) && child(root, 2)->separator);
    CHECK(child(root, 3) && child(root, 3)->toggle == MenuToggle::Checkmark && child(root, 3)->toggle_state == 1);
    const MenuItem* more = child(root, 4);
    REQUIRE(more);
    CHECK(more->has_submenu);
    CHECK(more->children.size() == 1 && more->children[0].id == 5 && !more->children[0].enabled);
    CHECK(child(root, 6) && !child(root, 6)->visible);
    CHECK(child(root, 7) && child(root, 7)->icon_png.size() == 8 && child(root, 7)->icon_png[1] == 'P');
}

void test_watcher_role(const bstest::PrivateBus& bus) {
    bstest::SignalLog sig(bus.address(), "type='signal',interface='org.kde.StatusNotifierWatcher'");
    auto host = make_host(bus);
    REQUIRE(host);
    Log log(host->events());
    auto st = log.wait_any<TrayHostStatus>();
    REQUIRE(st);
    CHECK(st->role == TrayRole::Watcher);
    CHECK(host->status().role == TrayRole::Watcher);

    // The watcher as other clients see it.
    auto g = run({"gdbus", "introspect", "--session", "--dest", kWatcher, "--object-path", "/StatusNotifierWatcher"},
                 bus.env());
    CHECK_EQ(g.exit_code, 0);
    CHECK(g.out.find("RegisterStatusNotifierItem(") != std::string::npos);
    CHECK(g.out.find("StatusNotifierItemRegistered(") != std::string::npos);
    auto b = run({"busctl", "--address=" + bus.address(), "get-property", kWatcher, "/StatusNotifierWatcher", kWatcher,
                  "ProtocolVersion", "IsStatusNotifierHostRegistered"});
    CHECK_EQ(trim(b.out), std::string("i 0\nb true"));
    // A name with no owner is refused.
    b = run({"busctl", "--address=" + bus.address(), "call", kWatcher, "/StatusNotifierWatcher", kWatcher,
             "RegisterStatusNotifierItem", "s", "org.nobody.Here"});
    CHECK(b.exit_code != 0);
    b = run({"busctl", "--address=" + bus.address(), "call", kWatcher, "/StatusNotifierWatcher", kWatcher,
             "RegisterStatusNotifierItem", "s", "not a name"});
    CHECK(b.exit_code != 0);

    // A real item registering by object path.
    ItemProc item = ItemProc::start(bus, "path");
    REQUIRE(item.ok());
    CHECK_EQ(item.id, item.unique + "/org/brosys/Item");
    auto added = log.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == item.id; }, 10000ms);
    REQUIRE(added);
    check_item_properties(added->item, item);
    CHECK(sig.wait([&](const dbus::Message& m) {
        return m.member == "StatusNotifierItemRegistered" && !m.args.empty() && m.args[0].as_string() == item.id;
    }) >= 0);
    CHECK(registered_items_contain(bus, item.id));
    auto menu = log.wait<TrayMenuChanged>(menu_of(item.id));
    REQUIRE(menu);
    check_menu(menu->root);
    CHECK(host->menu(item.id) == menu->root);
    auto items = host->items();
    CHECK(items.size() == 1 && items[0] == added->item);

    // New* signals -> refetch -> exact change bits.
    Control ctl(bus);
    REQUIRE(ctl.conn);
    struct Step {
        const char* method;
        dbus::Args args;
        uint32_t bits;
    };
    std::vector<Step> steps{
        {"SetTitle", {Value::str("New title")}, tray_change::Title},
        {"SetStatus", {Value::str("NeedsAttention")}, tray_change::Status},
        {"SetIcon", {Value::str("mail-read")}, tray_change::Icon},
        {"SetAttention", {Value::str("dialog-warning")}, tray_change::AttentionIcon},
        {"SetOverlay", {Value::str("emblem-new")}, tray_change::OverlayIcon},
        {"SetToolTip", {Value::str("T"), Value::str("B")}, tray_change::ToolTip},
    };
    for (auto& s : steps) {
        CHECK(ctl.call(item, s.method, s.args));
        auto c = log.wait<TrayItemChanged>(changed(item.id));
        REQUIRE(c);
        if (c->changes != s.bits) bstest::fail(__FILE__, __LINE__, std::string("change bits after ") + s.method);
    }
    auto now = host->items();
    REQUIRE(now.size() == 1);
    CHECK_EQ(now[0].title, std::string("New title"));
    CHECK(now[0].status == TrayItemStatus::NeedsAttention);
    CHECK_EQ(now[0].icon.name, std::string("mail-read"));
    CHECK_EQ(now[0].attention_icon.name, std::string("dialog-warning"));
    CHECK_EQ(now[0].overlay_icon.name, std::string("emblem-new"));
    CHECK(now[0].tooltip.title == "T" && now[0].tooltip.body == "B");

    // A burst of signals coalesces; the last state wins.
    CHECK(ctl.call(item, "Burst", {Value::u32(25)}));
    CHECK(bstest::wait_until([&] {
        auto v = host->items();
        return v.size() == 1 && v[0].title == "burst-24";
    }, 5000ms));

    // dbusmenu: property update, layout update, AboutToShow with refresh.
    CHECK(ctl.call(item, "RenameMenuItem", {Value::i32(3), Value::str("Checked!")}));
    menu = log.wait<TrayMenuChanged>([&](const TrayMenuChanged& e) {
        return e.item_id == item.id && child(e.root, 3) && child(e.root, 3)->label == "Checked!";
    });
    CHECK(menu.has_value());
    CHECK(ctl.call(item, "AddMenuItem", {Value::str("Added")}));
    menu = log.wait<TrayMenuChanged>([&](const TrayMenuChanged& e) {
        return e.item_id == item.id && child(e.root, 100) && child(e.root, 100)->label == "Added";
    });
    CHECK(menu.has_value());
    CHECK(ctl.call(item, "DirtyOnShow", {Value::str("Fresh _Open")}));
    CHECK(host->menu_about_to_show(item.id, 0).ok);
    CHECK(item.proc->wait_for_line("ABOUTTOSHOW 0", 5000ms));
    auto m = host->menu(item.id);
    CHECK(m && child(*m, 1) && child(*m, 1)->label == "Fresh _Open");
    CHECK(log.wait<TrayMenuChanged>(menu_of(item.id)).has_value());
    CHECK(host->menu_about_to_show(item.id, 4).ok);
    CHECK(item.proc->wait_for_line("ABOUTTOSHOW 4", 5000ms));

    CHECK(host->menu_event(item.id, 1, MenuEventType::Clicked).ok);
    CHECK(item.proc->wait_for_line("EVENT 1 clicked", 5000ms));
    CHECK(host->menu_event(item.id, 4, MenuEventType::Opened).ok);
    CHECK(item.proc->wait_for_line("EVENT 4 opened", 5000ms));
    CHECK(host->menu_event(item.id, 5, MenuEventType::Hovered).ok);
    CHECK(item.proc->wait_for_line("EVENT 5 hovered", 5000ms));
    CHECK(host->menu_event(item.id, 4, MenuEventType::Closed).ok);
    CHECK(item.proc->wait_for_line("EVENT 4 closed", 5000ms));
    // The Event payload: an int32 0 and the call time by default, else what the host gives.
    std::string line;
    CHECK(host->menu_event(item.id, 1, MenuEventType::Clicked).ok);
    CHECK(item.proc->wait_for_line("EVENT 1 clicked", 5000ms, &line));
    CHECK(line.find(" i:0 ") != std::string::npos && line.substr(line.rfind(' ') + 1) != "0");
    MenuEventData data;
    data.data = std::string("chosen");
    data.timestamp = 123456;
    CHECK(host->menu_event(item.id, 3, MenuEventType::Clicked, data).ok);
    CHECK(item.proc->wait_for_line("EVENT 3 clicked s:chosen 123456", 5000ms));
    data.data = true;
    data.timestamp = 7;
    CHECK(host->menu_event(item.id, 5, MenuEventType::Hovered, data).ok);
    CHECK(item.proc->wait_for_line("EVENT 5 hovered b:true 7", 5000ms));

    // Item interaction.
    CHECK(host->activate(item.id, 10, 20).ok);
    CHECK(item.proc->wait_for_line("ACTIVATE 10 20", 5000ms));
    CHECK(host->secondary_activate(item.id, 1, 2).ok);
    CHECK(item.proc->wait_for_line("SECONDARY 1 2", 5000ms));
    CHECK(host->context_menu(item.id, 3, 4).ok);
    CHECK(item.proc->wait_for_line("CONTEXT 3 4", 5000ms));
    CHECK(host->scroll(item.id, 5, ScrollOrientation::Horizontal).ok);
    CHECK(item.proc->wait_for_line("SCROLL 5 horizontal", 5000ms));
    CHECK(host->scroll(item.id, -120, ScrollOrientation::Vertical).ok);
    CHECK(item.proc->wait_for_line("SCROLL -120 vertical", 5000ms));
    // Keyboard selection is Activate; SNI has no double click or hover.
    CHECK(host->keyboard_select(item.id, 11, 12).ok);
    CHECK(item.proc->wait_for_line("ACTIVATE 11 12", 5000ms));
    Result no = host->double_click(item.id, 0, 0);
    CHECK(!no.ok && no.error.find("double click") != std::string::npos);
    no = host->hover(item.id, 0, 0, HoverPhase::Enter);
    CHECK(!no.ok && no.error.find("hover") != std::string::npos);
    CHECK(!host->keyboard_select("nope/x", 0, 0).ok);
    CHECK(!host->activate("nope/x", 0, 0).ok);
    CHECK(!host->menu_event("nope/x", 1, MenuEventType::Clicked).ok);
    CHECK(host->set_item_rect(item.id, Rect32{1, 2, 3, 4}).ok);

    // A second item registering by bus name.
    ItemProc named = ItemProc::start(bus, "name");
    REQUIRE(named.ok());
    CHECK_EQ(named.id, "org.kde.StatusNotifierItem-" + std::to_string(named.proc->pid()) + "-1/StatusNotifierItem");
    added = log.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == named.id; }, 10000ms);
    REQUIRE(added);
    check_item_properties(added->item, named);
    CHECK(log.wait<TrayMenuChanged>(menu_of(named.id)).has_value());
    CHECK(host->activate(named.id, 7, 8).ok);
    CHECK(named.proc->wait_for_line("ACTIVATE 7 8", 5000ms));
    CHECK_EQ(host->items().size(), size_t(2));

    // Owners vanish -> items are dropped (clean exit, then a kill).
    CHECK(ctl.call(item, "Quit"));
    CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& e) { return e.id == item.id; }).has_value());
    CHECK(sig.wait([&](const dbus::Message& m) {
        return m.member == "StatusNotifierItemUnregistered" && !m.args.empty() && m.args[0].as_string() == item.id;
    }) >= 0);
    CHECK(!registered_items_contain(bus, item.id));
    CHECK(!host->menu(item.id).has_value());
    named.proc->stop();
    CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& e) { return e.id == named.id; }).has_value());
    CHECK(host->items().empty());

    // A registration whose object never answers (busctl's own connection):
    // listed until its owner leaves, never shown as an item.
    b = run({"busctl", "--address=" + bus.address(), "call", kWatcher, "/StatusNotifierWatcher", kWatcher,
             "RegisterStatusNotifierItem", "s", "/Bogus"});
    CHECK_EQ(b.exit_code, 0);
    CHECK(sig.wait([&](const dbus::Message& m) {
        return m.member == "StatusNotifierItemUnregistered" && !m.args.empty() &&
               m.args[0].as_string().find("/Bogus") != std::string::npos;
    }) >= 0);
    auto dropped = log.wait<TrayItemDropped>(
        [](const TrayItemDropped& d) { return d.id.find("/Bogus") != std::string::npos; }, 5000ms);
    CHECK(dropped && !dropped->reason.empty());
    CHECK(!log.seen<TrayItemAdded>([](const TrayItemAdded&) { return true; }, 500ms));

    // A live process whose item object never answers: dropped once the
    // retries (on both item interfaces) run out, with the reason.
    ItemProc broken = ItemProc::start(bus, "path", true, {"--broken", "1"});
    REQUIRE(broken.ok());
    dropped = log.wait<TrayItemDropped>([&](const TrayItemDropped& d) { return d.id == broken.id; }, 10000ms);
    REQUIRE(dropped.has_value());
    CHECK(dropped->reason.find("never answered") != std::string::npos);
    CHECK(!log.seen<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == broken.id; }, 300ms));
    CHECK(host->items().empty());
}

// The freedesktop flavour: the same registry under
// org.freedesktop.StatusNotifierWatcher (signals on both interfaces), and an
// item that implements org.freedesktop.StatusNotifierItem.
void test_freedesktop_flavor(const bstest::PrivateBus& bus) {
    constexpr const char* kFdo = "org.freedesktop.StatusNotifierWatcher";
    bstest::SignalLog sig(bus.address(), std::string("type='signal',interface='") + kFdo + "'");
    auto host = make_host(bus);
    REQUIRE(host);
    Log log(host->events());
    auto st = log.wait<TrayHostStatus>([](const TrayHostStatus& s) { return s.role == TrayRole::Watcher; });
    REQUIRE(st);
    CHECK(st->detail.find(kFdo) != std::string::npos);
    auto r = run({"busctl", "--address=" + bus.address(), "status", kFdo});
    CHECK_EQ(r.exit_code, 0);
    r = run({"busctl", "--address=" + bus.address(), "get-property", kFdo, "/StatusNotifierWatcher", kFdo,
             "ProtocolVersion", "IsStatusNotifierHostRegistered"});
    CHECK_EQ(trim(r.out), std::string("i 0\nb true"));

    ItemProc item = ItemProc::start(bus, "name", true, {"--flavor", "freedesktop"});
    REQUIRE(item.ok());
    auto added = log.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == item.id; }, 10000ms);
    REQUIRE(added.has_value());
    check_item_properties(added->item, item);
    CHECK(sig.wait([&](const dbus::Message& m) {
        return m.member == "StatusNotifierItemRegistered" && !m.args.empty() && m.args[0].as_string() == item.id;
    }) >= 0);
    // The same registry seen through either interface.
    r = run({"busctl", "--address=" + bus.address(), "get-property", kFdo, "/StatusNotifierWatcher", kFdo,
             "RegisteredStatusNotifierItems"});
    CHECK(r.out.find("\"" + item.id + "\"") != std::string::npos);
    CHECK(registered_items_contain(bus, item.id));
    // Its New* signals and calls go through the freedesktop item interface.
    Control ctl(bus);
    REQUIRE(ctl.conn);
    CHECK(ctl.call(item, "SetTitle", {Value::str("fdo title")}));
    auto c = log.wait<TrayItemChanged>(changed(item.id));
    CHECK(c && c->changes == tray_change::Title && c->item.title == "fdo title");
    CHECK(host->activate(item.id, 5, 6).ok);
    CHECK(item.proc->wait_for_line("ACTIVATE 5 6", 5000ms));
    CHECK(host->menu_event(item.id, 1, MenuEventType::Clicked).ok);
    CHECK(item.proc->wait_for_line("EVENT 1 clicked", 5000ms));

    // An org.kde-flavoured item registering through the freedesktop
    // interface: the other item interface is found by probing.
    ItemProc mixed = ItemProc::start(bus, "path", true, {"--watcher", "freedesktop"});
    REQUIRE(mixed.ok());
    added = log.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == mixed.id; }, 10000ms);
    REQUIRE(added.has_value());
    check_item_properties(added->item, mixed);
    CHECK(host->context_menu(mixed.id, 1, 1).ok);
    CHECK(mixed.proc->wait_for_line("CONTEXT 1 1", 5000ms));
    mixed.proc->stop();
    CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& e) { return e.id == mixed.id; }).has_value());
    r = run({"busctl", "--address=" + bus.address(), "call", kFdo, "/StatusNotifierWatcher", kFdo,
             "RegisterStatusNotifierItem", "s", item.id});  // already registered: accepted, no duplicate
    CHECK_EQ(r.exit_code, 0);
    CHECK_EQ(host->items().size(), size_t(1));
}

// ---------------------------------------------------------------- WatcherClient role

void test_client_role(const bstest::PrivateBus& bus) {
    bstest::Daemon other({BROSYS_TRAY_HOST, "--bus", bus.address()});
    REQUIRE(other.wait_for_line("ROLE watcher", 10000ms));

    ItemProc early = ItemProc::start(bus, "path");
    REQUIRE(early.ok());
    REQUIRE(other.wait_for_line("ADDED " + early.id, 10000ms));

    bstest::SignalLog sig(bus.address(), "type='signal',interface='org.kde.StatusNotifierWatcher'");
    auto host = make_host(bus);
    REQUIRE(host);
    Log log(host->events());
    auto st = log.wait_any<TrayHostStatus>();
    REQUIRE(st);
    CHECK(st->role == TrayRole::WatcherClient);
    CHECK(sig.wait([](const dbus::Message& m) { return m.member == "StatusNotifierHostRegistered"; }) >= 0);

    // Known before we joined: fetched from RegisteredStatusNotifierItems.
    auto added = log.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == early.id; }, 10000ms);
    REQUIRE(added);
    check_item_properties(added->item, early);
    CHECK(log.wait<TrayMenuChanged>(menu_of(early.id)).has_value());

    // Registered afterwards: followed through the watcher's signals.
    ItemProc late = ItemProc::start(bus, "name");
    REQUIRE(late.ok());
    CHECK(log.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == late.id; }, 10000ms).has_value());
    CHECK(other.wait_for_line("ADDED " + late.id, 10000ms));

    Control ctl(bus);
    CHECK(ctl.call(early, "SetTitle", {Value::str("client sees this")}));
    auto c = log.wait<TrayItemChanged>(changed(early.id));
    CHECK(c && c->changes == tray_change::Title && c->item.title == "client sees this");
    CHECK(host->activate(early.id, 5, 6).ok);
    CHECK(early.proc->wait_for_line("ACTIVATE 5 6", 5000ms));
    CHECK(host->menu_event(late.id, 3, MenuEventType::Clicked).ok);
    CHECK(late.proc->wait_for_line("EVENT 3 clicked", 5000ms));

    CHECK(ctl.call(early, "Quit"));
    CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& e) { return e.id == early.id; }).has_value());

    // A second in-process host that must not become the watcher.
    auto passive = make_host(bus, false);
    REQUIRE(passive);
    Log plog(passive->events());
    st = plog.wait_any<TrayHostStatus>();
    CHECK(st && st->role == TrayRole::WatcherClient);
    CHECK(plog.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == late.id; }, 10000ms).has_value());

    // The watcher process dies: we were queued for the name and take over;
    // the item re-registers with us, the passive host follows us.
    other.stop();
    st = log.wait<TrayHostStatus>([](const TrayHostStatus& s) { return s.role == TrayRole::Watcher; }, 10000ms);
    CHECK(st.has_value());
    CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& e) { return e.id == late.id; }).has_value());
    CHECK(log.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == late.id; }, 10000ms).has_value());
    st = plog.wait<TrayHostStatus>([](const TrayHostStatus& s) { return s.role == TrayRole::WatcherClient; }, 10000ms);
    CHECK(st.has_value());
    CHECK(plog.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == late.id; }, 10000ms).has_value());
    CHECK(host->activate(late.id, 9, 9).ok);
    CHECK(late.proc->wait_for_line("ACTIVATE 9 9", 5000ms));

    // Our watcher goes away; the passive host waits instead of taking over.
    host.reset();
    st = plog.wait<TrayHostStatus>([](const TrayHostStatus& s) { return s.role == TrayRole::None; }, 10000ms);
    CHECK(st.has_value());
    CHECK(plog.wait<TrayItemRemoved>([&](const TrayItemRemoved& e) { return e.id == late.id; }).has_value());
    CHECK(run({"busctl", "--address=" + bus.address(), "status", kWatcher}).exit_code != 0);

    // A watcher appears again (another brosys process): the passive host joins it.
    bstest::Daemon again({BROSYS_TRAY_HOST, "--bus", bus.address()});
    CHECK(again.wait_for_line("ROLE watcher", 10000ms));
    st = plog.wait<TrayHostStatus>([](const TrayHostStatus& s) { return s.role == TrayRole::WatcherClient; }, 10000ms);
    CHECK(st.has_value());
    CHECK(plog.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == late.id; }, 10000ms).has_value());
}

void test_disconnect() {
    bstest::PrivateBus bus;
    REQUIRE(bus.ok());
    auto host = make_host(bus);
    REQUIRE(host);
    Log log(host->events());
    CHECK(log.wait<TrayHostStatus>([](const TrayHostStatus& s) { return s.role == TrayRole::Watcher; }).has_value());
    ItemProc item = ItemProc::start(bus, "path");
    REQUIRE(item.ok());
    CHECK(log.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == item.id; }, 10000ms).has_value());
    bus.kill();
    auto st = log.wait<TrayHostStatus>([](const TrayHostStatus& s) { return s.role == TrayRole::None; }, 5000ms);
    CHECK(st && st->detail.find("lost") != std::string::npos);
    CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& e) { return e.id == item.id; }).has_value());
    CHECK(host->items().empty());
    CHECK(!host->activate(item.id, 0, 0).ok);
}

// The session bus daemon restarts (same address; every connection drops and
// every name is released). The host reports the loss and its items go,
// reconnects, owns the watcher names again, and the items, which
// re-register by themselves as real ones do, come back: one under its new
// unique name, one under its well-known name. Then a passive host follows
// another brosys process's watcher across a restart.
void test_bus_restart() {
    bstest::PrivateBus bus;
    REQUIRE(bus.ok());
    auto host = make_host(bus);
    REQUIRE(host);
    Log log(host->events());
    auto is_watcher = [](const TrayHostStatus& s) { return s.role == TrayRole::Watcher; };
    REQUIRE(log.wait<TrayHostStatus>(is_watcher).has_value());
    ItemProc by_path = ItemProc::start(bus, "path");
    ItemProc by_name = ItemProc::start(bus, "name");
    REQUIRE(by_path.ok() && by_name.ok());
    for (auto* it : {&by_path, &by_name})
        CHECK(log.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == it->id; }, 10000ms));

    REQUIRE(bus.restart());
    auto st = log.wait<TrayHostStatus>([](const TrayHostStatus& s) { return s.role == TrayRole::None; }, 5000ms);
    CHECK(st && st->detail.find("lost") != std::string::npos);
    for (auto* it : {&by_path, &by_name})
        CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& e) { return e.id == it->id; }));
    CHECK(log.wait<TrayHostStatus>(is_watcher, 10000ms).has_value());
    CHECK_EQ(run({"busctl", "--address=" + bus.address(), "status", kWatcher}).exit_code, 0);
    CHECK_EQ(run({"busctl", "--address=" + bus.address(), "status", "org.freedesktop.StatusNotifierWatcher"}).exit_code,
             0);
    std::string line, tag, unique, new_id;
    REQUIRE(by_path.proc->wait_for_line("RECONNECTED ", 10000ms, &line));
    std::istringstream in(line);
    in >> tag >> unique >> new_id;
    CHECK_EQ(new_id, unique + "/org/brosys/Item");  // (a new daemon numbers from :1.0: it may repeat the old id)
    CHECK(log.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == new_id; }, 10000ms));
    CHECK(log.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == by_name.id; }, 10000ms));
    CHECK(registered_items_contain(bus, new_id));
    CHECK(registered_items_contain(bus, by_name.id));
    CHECK_EQ(host->items().size(), size_t(2));
    CHECK(host->activate(new_id, 1, 2).ok);
    CHECK(by_path.proc->wait_for_line("ACTIVATE 1 2", 5000ms));
    CHECK(host->activate(by_name.id, 3, 4).ok);
    CHECK(by_name.proc->wait_for_line("ACTIVATE 3 4", 5000ms));

    // Passive: another process owns the watcher; we follow it across a restart.
    host.reset();
    bstest::Daemon other({BROSYS_TRAY_HOST, "--bus", bus.address()});
    REQUIRE(other.wait_for_line("ROLE watcher", 10000ms));
    auto passive = make_host(bus, false);
    REQUIRE(passive);
    Log plog(passive->events());
    auto is_client = [](const TrayHostStatus& s) { return s.role == TrayRole::WatcherClient; };
    CHECK(plog.wait<TrayHostStatus>(is_client, 10000ms).has_value());
    CHECK(plog.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == by_name.id; }, 10000ms));
    REQUIRE(bus.restart());
    CHECK(plog.wait<TrayHostStatus>([](const TrayHostStatus& s) { return s.role == TrayRole::None; }, 5000ms));
    CHECK(plog.wait<TrayItemRemoved>([&](const TrayItemRemoved& e) { return e.id == by_name.id; }));
    CHECK(other.wait_for_line("ROLE watcher", 10000ms));  // the other process took its role up again
    CHECK(plog.wait<TrayHostStatus>(is_client, 10000ms).has_value());
    CHECK(plog.wait<TrayItemAdded>([&](const TrayItemAdded& e) { return e.item.id == by_name.id; }, 10000ms));
    CHECK(passive->activate(by_name.id, 5, 6).ok);
    CHECK(by_name.proc->wait_for_line("ACTIVATE 5 6", 5000ms));
}

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    for (const char* tool : {"busctl", "gdbus"})
        if (!bstest::have_program(tool))
            bstest::skip("test_tray_sni", std::string(tool) + " is not installed");
    {
        bstest::PrivateBus probe;
        if (!probe.ok()) bstest::skip("test_tray_sni", probe.error());
    }
    {
        bstest::PrivateBus bus;
        test_watcher_role(bus);
    }
    {
        bstest::PrivateBus bus;
        test_client_role(bus);
    }
    {
        bstest::PrivateBus bus;
        test_freedesktop_flavor(bus);
    }
    test_disconnect();
    test_bus_restart();
    return bstest::finish("test_tray_sni");
}
