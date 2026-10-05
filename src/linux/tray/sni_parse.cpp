#include "linux/tray/sni_parse.h"

namespace brosys::tray {

namespace {

constexpr int kMaxMenuDepth = 32;
constexpr int64_t kMaxIconSide = 4096;

const dbus::Value* find(const std::map<std::string, dbus::Value>& props, const char* key) {
    auto it = props.find(key);
    return it == props.end() ? nullptr : &it->second.unwrap();
}

std::string str(const std::map<std::string, dbus::Value>& props, const char* key) {
    auto* v = find(props, key);
    return v ? v->as_string() : std::string();
}

TrayIcon icon_from(const std::map<std::string, dbus::Value>& props, const char* name_key, const char* pixmap_key,
                   const std::string& theme_path) {
    TrayIcon icon;
    icon.name = str(props, name_key);
    if (auto* p = find(props, pixmap_key)) icon.pixmaps = pixmaps_from(*p);
    if (!icon.empty()) icon.theme_path = theme_path;
    return icon;
}

std::optional<MenuItem> menu_node(const dbus::Value& raw, int depth) {
    const dbus::Value& v = raw.unwrap();
    if (depth > kMaxMenuDepth || v.type() != '(') return std::nullopt;
    auto& f = v.items();
    if (f.size() != 3) return std::nullopt;
    auto id = f[0].to_int();
    if (!id) return std::nullopt;
    MenuItem m;
    m.id = static_cast<int32_t>(*id);
    const dbus::Value& props = f[1];
    if (auto* t = props.lookup("type")) m.separator = t->as_string() == "separator";
    if (auto* l = props.lookup("label")) m.label = l->as_string();
    if (auto* e = props.lookup("enabled")) m.enabled = e->as_bool(true);
    if (auto* vis = props.lookup("visible")) m.visible = vis->as_bool(true);
    if (auto* n = props.lookup("icon-name")) m.icon_name = n->as_string();
    if (auto* d = props.lookup("icon-data"))
        if (auto* b = d->as_bytes()) m.icon_png = *b;
    if (auto* tt = props.lookup("toggle-type")) {
        std::string t = tt->as_string();
        m.toggle = t == "checkmark" ? MenuToggle::Checkmark : t == "radio" ? MenuToggle::Radio : MenuToggle::None;
    }
    if (auto* ts = props.lookup("toggle-state")) m.toggle_state = static_cast<int32_t>(ts->as_int(-1));
    if (m.toggle_state != 0 && m.toggle_state != 1) m.toggle_state = -1;
    if (auto* sc = props.lookup("shortcut"))
        for (auto& combo : sc->items()) m.shortcut.push_back(combo.as_strings());
    if (auto* cd = props.lookup("children-display")) m.has_submenu = cd->as_string() == "submenu";
    for (auto& child : f[2].items()) {
        auto c = menu_node(child, depth + 1);
        if (c) m.children.push_back(std::move(*c));
    }
    return m;
}

}  // namespace

std::vector<Image> pixmaps_from(const dbus::Value& raw) {
    std::vector<Image> out;
    for (auto& entry : raw.unwrap().items()) {
        auto& f = entry.items();
        if (f.size() != 3) continue;
        int64_t w = f[0].as_int(0), h = f[1].as_int(0);
        auto* data = f[2].as_bytes();
        if (w <= 0 || h <= 0 || w > kMaxIconSide || h > kMaxIconSide || !data) continue;
        size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
        if (data->size() < n * 4) continue;
        Image img;
        img.width = static_cast<int32_t>(w);
        img.height = static_cast<int32_t>(h);
        img.rgba.resize(n * 4);
        const uint8_t* s = data->data();
        uint8_t* d = img.rgba.data();
        for (size_t i = 0; i < n; ++i, s += 4, d += 4) {
            d[0] = s[1];  // A R G B -> R G B A
            d[1] = s[2];
            d[2] = s[3];
            d[3] = s[0];
        }
        out.push_back(std::move(img));
    }
    return out;
}

TrayToolTip tooltip_from(const dbus::Value& raw, const std::string& theme_path) {
    TrayToolTip t;
    const dbus::Value& v = raw.unwrap();
    auto& f = v.items();
    if (v.type() != '(' || f.size() != 4) return t;
    t.icon.name = f[0].as_string();
    t.icon.pixmaps = pixmaps_from(f[1]);
    if (!t.icon.empty()) t.icon.theme_path = theme_path;
    t.title = f[2].as_string();
    t.body = f[3].as_string();
    return t;
}

TrayItemStatus status_from(const std::string& s) {
    if (s == "Passive") return TrayItemStatus::Passive;
    if (s == "NeedsAttention") return TrayItemStatus::NeedsAttention;
    return TrayItemStatus::Active;
}

TrayCategory category_from(const std::string& s) {
    if (s == "Communications") return TrayCategory::Communications;
    if (s == "SystemServices") return TrayCategory::SystemServices;
    if (s == "Hardware") return TrayCategory::Hardware;
    return TrayCategory::ApplicationStatus;
}

ItemSnapshot item_from_properties(const std::map<std::string, dbus::Value>& props) {
    ItemSnapshot s;
    TrayItem& it = s.item;
    std::string theme = str(props, "IconThemePath");
    it.app_id = str(props, "Id");
    it.title = str(props, "Title");
    it.category = category_from(str(props, "Category"));
    it.status = status_from(str(props, "Status"));
    it.icon = icon_from(props, "IconName", "IconPixmap", theme);
    it.overlay_icon = icon_from(props, "OverlayIconName", "OverlayIconPixmap", theme);
    it.attention_icon = icon_from(props, "AttentionIconName", "AttentionIconPixmap", theme);
    it.attention_movie = str(props, "AttentionMovieName");
    if (auto* tt = find(props, "ToolTip")) it.tooltip = tooltip_from(*tt, theme);
    if (auto* m = find(props, "ItemIsMenu")) it.item_is_menu = m->as_bool(false);
    if (auto* w = find(props, "WindowId")) {
        // The spec says i; some items send u (or t).
        if (auto u = w->to_uint()) it.window_id = *u;
    }
    s.menu_path = str(props, "Menu");
    if (s.menu_path == "/") s.menu_path.clear();
    it.has_menu = !s.menu_path.empty();
    return s;
}

uint32_t item_changes(const ItemSnapshot& a, const ItemSnapshot& b) {
    namespace c = tray_change;
    const TrayItem& x = a.item;
    const TrayItem& y = b.item;
    uint32_t bits = 0;
    if (x.title != y.title) bits |= c::Title;
    if (x.icon != y.icon) bits |= c::Icon;
    if (x.attention_icon != y.attention_icon || x.attention_movie != y.attention_movie) bits |= c::AttentionIcon;
    if (x.overlay_icon != y.overlay_icon) bits |= c::OverlayIcon;
    if (x.tooltip != y.tooltip) bits |= c::ToolTip;
    if (x.status != y.status) bits |= c::Status;
    if (a.menu_path != b.menu_path || x.has_menu != y.has_menu) bits |= c::Menu;
    if (x.app_id != y.app_id || x.category != y.category || x.item_is_menu != y.item_is_menu ||
        x.window_id != y.window_id || x.pid != y.pid || x.hidden != y.hidden)
        bits |= c::Other;
    return bits;
}

std::optional<MenuItem> menu_from_layout(const dbus::Value& node) { return menu_node(node, 0); }

}  // namespace brosys::tray
