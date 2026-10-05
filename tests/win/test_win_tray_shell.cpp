// Shell-mode tray hosting for real: a TrayHost owns Shell_TrayWnd on a
// private desktop, and a separate client process on that desktop calls the
// genuine Shell_NotifyIconW. Verifies isolation from the user's desktop,
// the TaskbarCreated announcement, every NIM_* message, icon pixels,
// tooltips, GUID identity, Shell_NotifyIconGetRect, interaction encodings,
// and removal when the owner window dies.
#include "brosys/tray.h"
#include "check.h"
#include "win/event_log.h"
#include "win/shell_fixture.h"

#include <shellapi.h>

using namespace brosys;
using namespace std::chrono_literals;
using bstest::win::cb_line;

namespace {

constexpr const char* kName = "test_win_tray_shell";
const char* kGuidId = "guid:{7E5C6A55-2B21-4C1E-9D0A-3F1B5E2D9A11}";

bstest::win::ShellFixture fx;
std::unique_ptr<TrayHost> host;

std::optional<TrayItem> find_item(const std::string& id) {
    for (auto& i : host->items())
        if (i.id == id) return i;
    return std::nullopt;
}

// Waits for exactly `expected` callback lines on the client after `from`
// and checks nothing else arrives shortly after.
void expect_callbacks(const std::vector<std::string>& expected, size_t from) {
    auto is_cb = [](const std::string& l) { return l.rfind("cb ", 0) == 0; };
    size_t at = from;
    for (auto& want : expected) {
        size_t idx = 0;
        auto got = fx.client.wait(is_cb, at, 5s, &idx);
        if (!got) {
            bstest::fail(__FILE__, __LINE__, "missing callback " + want);
            return;
        }
        if (*got != want) bstest::fail(__FILE__, __LINE__, "callback '" + *got + "' expected '" + want + "'");
        at = idx + 1;
    }
    auto extra = fx.client.wait(is_cb, at, 150ms);
    if (extra) bstest::fail(__FILE__, __LINE__, "unexpected extra callback " + *extra);
}

std::string client_find() { return fx.client_find(); }

// Every step that makes the client call Shell_NotifyIcon is gated on proof
// that its lookup resolves to our window (or to nothing): the user's tray must
// never see a call.
bool test_isolation_and_announce(bstest::win::TaskbarCreatedProbe& probe, HWND user_tray) {
    const std::string ours = " " + std::to_string(GetCurrentProcessId());
    // Before the host exists the private desktop has no tray at all.
    if (fx.private_tray_owner() != 0 || client_find() != "0 0") {
        bstest::fail(__FILE__, __LINE__, "the private desktop already has a Shell_TrayWnd; not isolated");
        return false;
    }

    std::string error;
    host = fx.create_host(TrayMode::Auto, &error);
    if (!host || host->status().role != TrayRole::Shell) {
        bstest::fail(__FILE__, __LINE__, "no shell-mode host on the private desktop: " + error);
        return false;
    }
    CHECK(host->status().detail.find("brosys-test-") != std::string::npos);

    // Shell_TrayWnd on the private desktop is ours; the user's is untouched.
    const std::string seen = client_find();
    if (fx.private_tray_owner() != GetCurrentProcessId() || !seen.ends_with(ours)) {
        bstest::fail(__FILE__, __LINE__, "the client's Shell_TrayWnd lookup does not resolve to the host: " + seen);
        return false;
    }
    CHECK(FindWindowW(L"Shell_TrayWnd", nullptr) == user_tray);
    if (user_tray) {
        DWORD owner = 0;
        GetWindowThreadProcessId(user_tray, &owner);
        CHECK(owner != GetCurrentProcessId());
    }

    // The client's genuine Shell_NotifyIcon reaches this host.
    {
        bstest::EventLog<TrayEvent> first(host->events());
        CHECK_EQ(fx.client.command("version 1 4"), std::string("0"));  // not added yet
        CHECK_EQ(fx.client.command("add 1"), std::string("1"));
        if (!first.wait<TrayItemAdded>([&](const TrayItemAdded& a) { return a.item.id == fx.item_id(1); }, 0, 3s)) {
            bstest::fail(__FILE__, __LINE__, "NIM_ADD did not reach the host");
            return false;
        }
        CHECK_EQ(fx.client.command("version 1 4"), std::string("1"));
        CHECK_EQ(fx.client.command("add 2"), std::string("1"));
    }

    // Shell restart: without a tray, Shell_NotifyIcon fails on this desktop
    // (a NIM_MODIFY of an icon only this desktop ever had) ...
    host.reset();
    if (fx.private_tray_owner() != 0 || client_find() != "0 0") {
        bstest::fail(__FILE__, __LINE__, "Shell_TrayWnd survived the host");
        return false;
    }
    CHECK_EQ(fx.client.command("tip 1 nobody"), std::string("0"));

    // ... and a new host's TaskbarCreated makes the client re-add both icons.
    size_t client_mark = fx.client.mark();
    host = fx.create_host(TrayMode::Auto, &error);
    if (!host || host->status().role != TrayRole::Shell) {
        bstest::fail(__FILE__, __LINE__, "no second shell-mode host: " + error);
        return false;
    }
    CHECK(fx.client.wait_prefix("taskbarcreated", client_mark, 5s).has_value());
    auto readded = fx.client.wait_prefix("readded ", client_mark, 5s);
    CHECK(readded && *readded == "readded 2");
    // ... and the announcement never reached the user's desktop.
    CHECK_EQ(probe.count(), 0);

    // A second shell on the same desktop is refused; Auto falls back to None.
    std::string err2;
    auto second = fx.create_host(TrayMode::Shell, &err2);
    CHECK(second == nullptr);
    CHECK(err2.find("Shell_TrayWnd") != std::string::npos);
    auto alongside = fx.create_host(TrayMode::Auto, &err2);
    CHECK(alongside != nullptr);
    if (alongside) {
        CHECK(alongside->status().role == TrayRole::None);
        CHECK(alongside->status().detail.find("test_win_tray_shell.exe") != std::string::npos);
        CHECK(alongside->items().empty());
    }
    return true;
}

void test_items(bstest::EventLog<TrayEvent>& log) {
    const std::string id1 = fx.item_id(1), id2 = fx.item_id(2);
    auto added1 = log.wait<TrayItemAdded>([&](const TrayItemAdded& a) { return a.item.id == id1; }, 0);
    auto added2 = log.wait<TrayItemAdded>([&](const TrayItemAdded& a) { return a.item.id == id2; }, 0);
    REQUIRE(added1.has_value());
    REQUIRE(added2.has_value());
    const TrayItem& it = added1->item;
    CHECK_EQ(it.app_id, std::string("brosys_tray_client.exe"));
    CHECK_EQ(it.pid, uint32_t(fx.client_pid));
    CHECK_EQ(bstest::win::hex(it.window_id), fx.window1);
    // Re-added with the tip the app set while no tray existed.
    CHECK_EQ(it.title, std::string("nobody"));
    CHECK_EQ(it.tooltip.title, std::string("nobody"));
    CHECK(!it.hidden);
    REQUIRE(it.icon.pixmaps.size() == 1);
    const Image& img = it.icon.pixmaps[0];
    CHECK_EQ(img.width, 16);
    CHECK_EQ(img.height, 16);
    REQUIRE(img.rgba.size() == 16u * 16u * 4u);
    CHECK_EQ(int(img.rgba[3]), 0);  // pixel (0,0) transparent
    CHECK_EQ(int(img.rgba[4]), 255);
    CHECK_EQ(int(img.rgba[5]), 0);
    CHECK_EQ(int(img.rgba[6]), 0);
    CHECK_EQ(int(img.rgba[7]), 255);
    CHECK_EQ(host->items().size(), size_t(2));
}

void test_modify(bstest::EventLog<TrayEvent>& log) {
    const std::string id1 = fx.item_id(1), id2 = fx.item_id(2);
    auto changed = [&](const std::string& id, size_t from) {
        return log.wait<TrayItemChanged>([&](const TrayItemChanged& c) { return c.item.id == id; }, from);
    };

    size_t m = log.mark();
    CHECK_EQ(fx.client.command("tip 1 hello"), std::string("1"));
    auto c = changed(id1, m);
    REQUIRE(c.has_value());
    CHECK_EQ(c->changes, tray_change::Title | tray_change::ToolTip);
    CHECK_EQ(c->item.tooltip.title, std::string("hello"));

    // Version 4 without NIF_SHOWTIP: the app draws its own popup, no standard tooltip.
    m = log.mark();
    CHECK_EQ(fx.client.command("tipnoshow 1 quiet"), std::string("1"));
    c = changed(id1, m);
    REQUIRE(c.has_value());
    CHECK_EQ(c->item.title, std::string("quiet"));
    CHECK_EQ(c->item.tooltip.title, std::string());
    // Version 0 ignores NIF_SHOWTIP.
    m = log.mark();
    CHECK_EQ(fx.client.command("tipnoshow 2 old"), std::string("1"));
    c = changed(id2, m);
    REQUIRE(c.has_value());
    CHECK_EQ(c->item.tooltip.title, std::string("old"));

    m = log.mark();
    CHECK_EQ(fx.client.command("icon 1 ff00ff00"), std::string("1"));
    c = changed(id1, m);
    REQUIRE(c.has_value());
    CHECK_EQ(c->changes, tray_change::Icon);
    REQUIRE(c->item.icon.pixmaps.size() == 1 && c->item.icon.pixmaps[0].rgba.size() == 1024u);
    const auto& px = c->item.icon.pixmaps[0].rgba;
    CHECK(px[4] == 0 && px[5] == 255 && px[6] == 0 && px[7] == 255);

    m = log.mark();
    CHECK_EQ(fx.client.command("hide 1 1"), std::string("1"));
    c = changed(id1, m);
    REQUIRE(c.has_value());
    CHECK_EQ(c->changes, tray_change::Other);
    CHECK(c->item.hidden);
    CHECK(find_item(id1) && find_item(id1)->hidden);
    CHECK_EQ(fx.client.command("hide 1 0"), std::string("1"));

    CHECK_EQ(fx.client.command("focus 1"), std::string("1"));
    CHECK_EQ(fx.client.command("focus 99"), std::string("0"));  // unknown icon
    CHECK_EQ(fx.client.command("tip 98 x"), std::string("0"));  // NIM_MODIFY of an unknown icon
}

void test_interaction() {
    const std::string id1 = fx.item_id(1), id2 = fx.item_id(2);
    // Version 4: wParam = anchor (x, y), lParam = MAKELPARAM(event, uID).
    size_t m = fx.client.mark();
    CHECK(host->activate(id1, 100, 200).ok);
    expect_callbacks({cb_line(1, 0x00C80064, 0x10201), cb_line(1, 0x00C80064, 0x10202),
                      cb_line(1, 0x00C80064, 0x10000 | NIN_SELECT)},
                     m);
    m = fx.client.mark();
    CHECK(host->context_menu(id1, 5, 6).ok);
    expect_callbacks({cb_line(1, 0x00060005, 0x10204), cb_line(1, 0x00060005, 0x10205),
                      cb_line(1, 0x00060005, 0x10000 | WM_CONTEXTMENU)},
                     m);
    m = fx.client.mark();
    CHECK(host->secondary_activate(id1, 7, 8).ok);
    expect_callbacks({cb_line(1, 0x00080007, 0x10207), cb_line(1, 0x00080007, 0x10208)}, m);

    // Version 0: wParam = uID, lParam = the mouse message; no NIN_SELECT / WM_CONTEXTMENU.
    m = fx.client.mark();
    CHECK(host->activate(id2, 1, 1).ok);
    expect_callbacks({cb_line(1, 2, WM_LBUTTONDOWN), cb_line(1, 2, WM_LBUTTONUP)}, m);
    m = fx.client.mark();
    CHECK(host->context_menu(id2, 1, 1).ok);
    expect_callbacks({cb_line(1, 2, WM_RBUTTONDOWN), cb_line(1, 2, WM_RBUTTONUP)}, m);

    CHECK(!host->activate("hwnd:0:0", 0, 0).ok);
    CHECK(!host->scroll(id1, 1, ScrollOrientation::Vertical).ok);
    CHECK(!host->menu(id1).has_value());
    CHECK(!host->menu_event(id1, 1, MenuEventType::Clicked).ok);
}

void test_guid_and_rect(bstest::EventLog<TrayEvent>& log) {
    size_t m = log.mark();
    CHECK_EQ(fx.client.command("add g"), std::string("1"));
    auto added = log.wait<TrayItemAdded>([&](const TrayItemAdded& a) { return a.item.id == kGuidId; }, m);
    REQUIRE(added.has_value());
    CHECK_EQ(fx.client.command("add g"), std::string("0"));  // duplicate identity
    size_t cm = fx.client.mark();
    CHECK(host->activate(kGuidId, 0, 0).ok);
    expect_callbacks({cb_line(1, 0, WM_LBUTTONDOWN), cb_line(1, 0, WM_LBUTTONUP)}, cm);

    // Shell_NotifyIconGetRect is answered from set_item_rect.
    cm = fx.client.mark();
    fx.client.command("rect 1");
    auto none = fx.client.wait_prefix("rect ", cm, 3s);
    CHECK(none && none->rfind("rect 0 ", 0) != 0);  // a failure HRESULT before a rect is known
    CHECK(host->set_item_rect(fx.item_id(1), Rect32{10, 20, 30, 40}).ok);
    CHECK(host->set_item_rect(kGuidId, Rect32{100, 0, 16, 16}).ok);
    CHECK(!host->set_item_rect("hwnd:0:0", Rect32{}).ok);
    cm = fx.client.mark();
    fx.client.command("rect 1");
    auto r1 = fx.client.wait_prefix("rect ", cm, 3s);
    CHECK(r1 && *r1 == "rect 0 10 20 40 60");
    cm = fx.client.mark();
    fx.client.command("rect g");
    auto rg = fx.client.wait_prefix("rect ", cm, 3s);
    CHECK(rg && *rg == "rect 0 100 0 116 16");
}

void test_removal(bstest::EventLog<TrayEvent>& log) {
    const std::string id2 = fx.item_id(2);
    size_t m = log.mark();
    CHECK_EQ(fx.client.command("del 2"), std::string("1"));
    CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& r) { return r.id == id2; }, m).has_value());
    CHECK_EQ(fx.client.command("del 2"), std::string("0"));
    CHECK(!find_item(id2).has_value());

    // An icon whose owner window is destroyed without NIM_DELETE goes away.
    CHECK_EQ(fx.client.command("window2"), std::string("1"));
    m = log.mark();
    CHECK_EQ(fx.client.command("add w7"), std::string("1"));
    auto added = log.wait<TrayItemAdded>(
        [&](const TrayItemAdded& a) { return a.item.id.size() > 2 && a.item.id.ends_with(":7"); }, m);
    REQUIRE(added.has_value());
    const std::string id7 = added->item.id;
    m = log.mark();
    CHECK_EQ(fx.client.command("killwindow2"), std::string("1"));
    CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& r) { return r.id == id7; }, m, 5s).has_value());
    CHECK(!host->activate(id7, 0, 0).ok);

    // The whole client dies: its remaining icons go too.
    m = log.mark();
    fx.client.kill();
    CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& r) { return r.id == fx.item_id(1); }, m, 5s)
              .has_value());
    CHECK(log.wait<TrayItemRemoved>([&](const TrayItemRemoved& r) { return r.id == kGuidId; }, m, 5s).has_value());
    CHECK(host->items().empty());
}

}  // namespace

int main() {
    fx.require_desktop(kName);
    HWND user_tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    bstest::win::TaskbarCreatedProbe probe;
    CHECK(probe.ok());

    std::string why;
    if (!fx.start_client(&why)) {
        bstest::fail(__FILE__, __LINE__, "client: " + why);
        return bstest::finish(kName);
    }
    if (test_isolation_and_announce(probe, user_tray)) {
        bstest::EventLog<TrayEvent> log(host->events());
        test_items(log);
        test_modify(log);
        test_interaction();
        test_guid_and_rect(log);
        test_removal(log);
        host.reset();
    }
    fx.client.stop();
    // Nothing reached the user's desktop at any point.
    CHECK_EQ(probe.count(), 0);
    CHECK(FindWindowW(L"Shell_TrayWnd", nullptr) == user_tray);
    return bstest::finish(kName);
}
