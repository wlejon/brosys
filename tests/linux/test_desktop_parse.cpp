// Pure conversions of the notification server and the tray host, no bus:
// image-data (iiibiiay) to straight RGBA, hint precedence and pass-through,
// SNI IconPixmap ARGB32 to RGBA, ToolTip, item properties and change bits,
// dbusmenu layouts.
#include "check.h"
#include "linux/notify/hints.h"
#include "linux/tray/sni_parse.h"
#include "linux/tray/watcher.h"

using namespace brosys;
using dbus::Value;

namespace {

Value image_struct(int w, int h, int stride, bool alpha, int bps, int channels, std::vector<uint8_t> data) {
    return Value::structure({Value::i32(w), Value::i32(h), Value::i32(stride), Value::boolean(alpha), Value::i32(bps),
                             Value::i32(channels), Value::bytes(std::move(data))});
}

std::string hint(const Notification& n, const std::string& key) {
    for (auto& [k, v] : n.other_hints)
        if (k == key) return v;
    return "<absent>";
}

void test_images() {
    // RGB, width 2, rowstride 8 (2 padding bytes), last row unpadded.
    auto img = notify::image_from_struct(image_struct(2, 2, 8, false, 8, 3,
                                                      {1, 2, 3, 4, 5, 6, 0xEE, 0xEE,  //
                                                       7, 8, 9, 10, 11, 12}));
    REQUIRE(img.has_value());
    CHECK_EQ(img->width, 2);
    CHECK_EQ(img->height, 2);
    CHECK(img->rgba == (std::vector<uint8_t>{1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255}));

    // RGBA with alpha kept straight.
    img = notify::image_from_struct(image_struct(1, 2, 4, true, 8, 4, {10, 20, 30, 40, 50, 60, 70, 0}));
    REQUIRE(img.has_value());
    CHECK(img->rgba == (std::vector<uint8_t>{10, 20, 30, 40, 50, 60, 70, 0}));

    // 4 channels without has_alpha: the 4th byte is padding -> opaque.
    img = notify::image_from_struct(image_struct(1, 1, 4, false, 8, 4, {1, 2, 3, 9}));
    REQUIRE(img.has_value());
    CHECK(img->rgba == (std::vector<uint8_t>{1, 2, 3, 255}));

    // Malformed.
    CHECK(!notify::image_from_struct(image_struct(2, 2, 8, false, 8, 3, {1, 2, 3})));        // data short
    CHECK(!notify::image_from_struct(image_struct(2, 1, 5, false, 8, 3, std::vector<uint8_t>(6))));  // stride short
    CHECK(!notify::image_from_struct(image_struct(1, 1, 8, false, 16, 3, std::vector<uint8_t>(8))));  // 16 bit
    CHECK(!notify::image_from_struct(image_struct(1, 1, 4, true, 8, 3, std::vector<uint8_t>(4))));    // alpha + 3ch
    CHECK(!notify::image_from_struct(image_struct(0, 1, 4, true, 8, 4, std::vector<uint8_t>(4))));
    CHECK(!notify::image_from_struct(Value::str("nope")));
    // Wrapped in a variant (as it arrives in a{sv}).
    CHECK(notify::image_from_struct(Value::variant(image_struct(1, 1, 3, false, 8, 3, {1, 2, 3}))).has_value());
}

void test_hints() {
    Notification n;
    Value hints = Value::vardict({
        {"urgency", Value::byte(2)},
        {"category", Value::str("email.arrived")},
        {"desktop-entry", Value::str("org.gnome.Evolution")},
        {"icon_data", image_struct(1, 1, 3, false, 8, 3, {9, 9, 9})},
        {"image_data", image_struct(1, 1, 3, false, 8, 3, {5, 5, 5})},
        {"image-data", image_struct(1, 1, 3, false, 8, 3, {1, 2, 3})},
        {"image_path", Value::str("/old.png")},
        {"image-path", Value::str("file:///new.png")},
        {"sound-file", Value::str("/s.oga")},
        {"sound-name", Value::str("message-new-email")},
        {"suppress-sound", Value::boolean(true)},
        {"transient", Value::boolean(true)},
        {"resident", Value::boolean(true)},
        {"action-icons", Value::boolean(true)},
        {"x", Value::i32(-10)},
        {"y", Value::i32(20)},
        {"x-vendor", Value::strings({"a", "b"})},
        {"value", Value::i32(42)},
    });
    notify::apply_hints(hints, n);
    CHECK(n.urgency == Urgency::Critical);
    CHECK_EQ(n.category, std::string("email.arrived"));
    CHECK_EQ(n.desktop_entry, std::string("org.gnome.Evolution"));
    REQUIRE(n.image.has_value());
    CHECK(n.image->rgba == (std::vector<uint8_t>{1, 2, 3, 255}));  // image-data wins
    CHECK_EQ(n.image_path, std::string("file:///new.png"));        // image-path wins
    CHECK_EQ(n.sound_file, std::string("/s.oga"));
    CHECK_EQ(n.sound_name, std::string("message-new-email"));
    CHECK(n.suppress_sound && n.transient && n.resident && n.action_icons);
    CHECK(n.x == -10 && n.y == 20);
    CHECK_EQ(hint(n, "x-vendor"), std::string("['a', 'b']"));
    CHECK_EQ(hint(n, "value"), std::string("42"));
    CHECK(hint(n, "image_data") != "<absent>");  // shadowed images stay visible
    CHECK(hint(n, "category") == "<absent>");

    // Deprecated names alone, a malformed image falls through to the next.
    Notification m;
    notify::apply_hints(Value::vardict({{"image-data", Value::str("broken")},
                                        {"icon_data", image_struct(1, 1, 3, false, 8, 3, {7, 8, 9})},
                                        {"image_path", Value::str("/p.png")},
                                        {"urgency", Value::byte(7)}}),
                        m);
    REQUIRE(m.image.has_value());
    CHECK(m.image->rgba == (std::vector<uint8_t>{7, 8, 9, 255}));
    CHECK_EQ(m.image_path, std::string("/p.png"));
    CHECK(m.urgency == Urgency::Normal);
    CHECK_EQ(hint(m, "urgency"), std::string("7"));
    CHECK_EQ(hint(m, "image-data"), std::string("'broken'"));
}

void test_sni() {
    // Two pixmaps; one truncated is skipped.
    Value pix = Value::array("(iiay)", {Value::structure({Value::i32(2), Value::i32(1), Value::bytes({0x80, 1, 2, 3, 0xFF, 4, 5, 6})}),
                                        Value::structure({Value::i32(4), Value::i32(4), Value::bytes({1, 2, 3})})});
    auto images = tray::pixmaps_from(pix);
    REQUIRE(images.size() == 1);
    CHECK(images[0].width == 2 && images[0].height == 1);
    CHECK(images[0].rgba == (std::vector<uint8_t>{1, 2, 3, 0x80, 4, 5, 6, 0xFF}));

    std::map<std::string, Value> props{
        {"Category", Value::str("Hardware")},
        {"Id", Value::str("app")},
        {"Title", Value::str("Title")},
        {"Status", Value::str("NeedsAttention")},
        {"WindowId", Value::i32(77)},
        {"IconName", Value::str("icon")},
        {"IconPixmap", pix},
        {"AttentionIconName", Value::str("att")},
        {"AttentionMovieName", Value::str("movie")},
        {"OverlayIconName", Value::str("")},
        {"ToolTip", Value::structure({Value::str("tip"), Value::array("(iiay)", {}), Value::str("tt"), Value::str("<b>x</b>")})},
        {"ItemIsMenu", Value::boolean(true)},
        {"Menu", Value::obj("/Menu")},
        {"IconThemePath", Value::str("/themes")},
    };
    auto snap = tray::item_from_properties(props);
    CHECK_EQ(snap.item.app_id, std::string("app"));
    CHECK(snap.item.category == TrayCategory::Hardware);
    CHECK(snap.item.status == TrayItemStatus::NeedsAttention);
    CHECK_EQ(snap.item.window_id, uint64_t(77));
    CHECK_EQ(snap.item.icon.name, std::string("icon"));
    CHECK_EQ(snap.item.icon.theme_path, std::string("/themes"));
    CHECK_EQ(snap.item.icon.pixmaps.size(), size_t(1));
    CHECK_EQ(snap.item.attention_icon.name, std::string("att"));
    CHECK(snap.item.overlay_icon.empty());
    CHECK_EQ(snap.item.overlay_icon.theme_path, std::string(""));
    CHECK_EQ(snap.item.attention_movie, std::string("movie"));
    CHECK_EQ(snap.item.tooltip.icon.name, std::string("tip"));
    CHECK_EQ(snap.item.tooltip.title, std::string("tt"));
    CHECK_EQ(snap.item.tooltip.body, std::string("<b>x</b>"));
    CHECK(snap.item.item_is_menu && snap.item.has_menu);
    CHECK_EQ(snap.menu_path, std::string("/Menu"));

    auto changed = props;
    changed["Title"] = Value::str("T2");
    changed["Status"] = Value::str("Active");
    changed["Menu"] = Value::obj("/");
    auto snap2 = tray::item_from_properties(changed);
    CHECK(!snap2.item.has_menu);
    namespace c = tray_change;
    CHECK_EQ(tray::item_changes(snap, snap2), c::Title | c::Status | c::Menu);
    changed = props;
    changed["IconThemePath"] = Value::str("/other");
    CHECK_EQ(tray::item_changes(snap, tray::item_from_properties(changed)),
             c::Icon | c::AttentionIcon | c::ToolTip);
    changed = props;
    changed["ToolTip"] = Value::str("garbage");  // wrong type: empty tooltip
    CHECK_EQ(tray::item_changes(snap, tray::item_from_properties(changed)), c::ToolTip);
    CHECK_EQ(tray::item_changes(snap, snap), 0u);

    std::string service, path;
    CHECK(tray::split_item_id(":1.5/org/ayatana/NotificationItem/x", &service, &path));
    CHECK(service == ":1.5" && path == "/org/ayatana/NotificationItem/x");
    CHECK(!tray::split_item_id("/nopath", &service, &path));
    CHECK(!tray::split_item_id("noslash", &service, &path));
}

Value menu_node(int id, std::vector<std::pair<std::string, Value>> props, std::vector<Value> children) {
    dbus::Value::Items boxed;
    for (auto& c : children) boxed.push_back(Value::variant(std::move(c)));
    return Value::structure({Value::i32(id), Value::vardict(std::move(props)), Value::array("v", std::move(boxed))});
}

void test_menu() {
    Value layout = menu_node(0, {{"children-display", Value::str("submenu")}},
                             {menu_node(1, {{"label", Value::str("_Open")},
                                            {"icon-name", Value::str("document-open")},
                                            {"shortcut", Value::array("as", {Value::strings({"Control", "o"})})}},
                                        {}),
                              menu_node(2, {{"type", Value::str("separator")}}, {}),
                              menu_node(3, {{"label", Value::str("Check")},
                                            {"toggle-type", Value::str("checkmark")},
                                            {"toggle-state", Value::i32(1)}},
                                        {}),
                              menu_node(4, {{"label", Value::str("More")}, {"children-display", Value::str("submenu")}},
                                        {menu_node(5, {{"label", Value::str("Deep")}, {"enabled", Value::boolean(false)}}, {})}),
                              menu_node(6, {{"visible", Value::boolean(false)}, {"icon-data", Value::bytes({0x89, 'P'})},
                                            {"toggle-type", Value::str("radio")}, {"toggle-state", Value::i32(9)}},
                                        {})});
    auto m = tray::menu_from_layout(layout);
    REQUIRE(m.has_value());
    CHECK(m->id == 0 && m->has_submenu);
    REQUIRE(m->children.size() == 5);
    auto& open = m->children[0];
    CHECK_EQ(open.label, std::string("_Open"));
    CHECK_EQ(open.icon_name, std::string("document-open"));
    CHECK(open.shortcut == (std::vector<std::vector<std::string>>{{"Control", "o"}}));
    CHECK(open.enabled && open.visible && !open.separator && open.toggle == MenuToggle::None && open.toggle_state == -1);
    CHECK(m->children[1].separator);
    CHECK(m->children[2].toggle == MenuToggle::Checkmark && m->children[2].toggle_state == 1);
    REQUIRE(m->children[3].children.size() == 1);
    CHECK(m->children[3].has_submenu);
    CHECK(!m->children[3].children[0].enabled);
    CHECK(!m->children[4].visible);
    CHECK(m->children[4].icon_png == (std::vector<uint8_t>{0x89, 'P'}));
    CHECK(m->children[4].toggle == MenuToggle::Radio && m->children[4].toggle_state == -1);
    CHECK(!tray::menu_from_layout(Value::str("x")));
}

}  // namespace

int main() {
    test_images();
    test_hints();
    test_sni();
    test_menu();
    return bstest::finish("test_desktop_parse");
}
