#include "api.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <string>

namespace brosys::api {

namespace {

Value buildTrayIcon(const TrayIcon& icon) {
    ObjectBuilder b;
    b.set("name", icon.name);
    b.set("themePath", icon.theme_path);
    b.set("hasPixmaps", !icon.pixmaps.empty());
    return b.build();
}

Value buildTrayToolTip(const TrayToolTip& tip) {
    ObjectBuilder b;
    ev::Persistent iconVal(buildTrayIcon(tip.icon));
    b.set("icon", iconVal.get());
    b.set("title", tip.title);
    b.set("body", tip.body);
    return b.build();
}

Value buildMenuItem(const MenuItem& m) {
    ObjectBuilder b;
    b.set("id", static_cast<double>(m.id));
    b.set("separator", m.separator);
    b.set("label", m.label);
    b.set("enabled", m.enabled);
    b.set("visible", m.visible);
    b.set("iconName", m.icon_name);

    std::string toggleStr = "none";
    if (m.toggle == MenuToggle::Checkmark) toggleStr = "checkmark";
    else if (m.toggle == MenuToggle::Radio) toggleStr = "radio";
    b.set("toggle", toggleStr);
    b.set("toggleState", static_cast<double>(m.toggle_state));
    b.set("hasSubmenu", m.has_submenu);

    ArrayBuilder children(m.children.size());
    for (size_t i = 0; i < m.children.size(); ++i) {
        ev::Persistent c(buildMenuItem(m.children[i]));
        children.set(static_cast<uint32_t>(i), c.get());
    }
    b.set("children", children.build());
    return b.build();
}

Value buildTrayItem(const TrayItem& it) {
    ObjectBuilder b;
    b.set("id", it.id);
    b.set("appId", it.app_id);
    b.set("title", it.title);
    b.set("status", to_string(it.status));

    std::string cat = "applicationStatus";
    if (it.category == TrayCategory::Communications) cat = "communications";
    else if (it.category == TrayCategory::SystemServices) cat = "systemServices";
    else if (it.category == TrayCategory::Hardware) cat = "hardware";
    b.set("category", cat);

    ev::Persistent iconVal(buildTrayIcon(it.icon));
    b.set("icon", iconVal.get());

    ev::Persistent ovIconVal(buildTrayIcon(it.overlay_icon));
    b.set("overlayIcon", ovIconVal.get());

    ev::Persistent attIconVal(buildTrayIcon(it.attention_icon));
    b.set("attentionIcon", attIconVal.get());

    ev::Persistent tipVal(buildTrayToolTip(it.tooltip));
    b.set("tooltip", tipVal.get());

    b.set("itemIsMenu", it.item_is_menu);
    b.set("hasMenu", it.has_menu);
    b.set("windowId", static_cast<double>(it.window_id));
    b.set("pid", static_cast<double>(it.pid));
    b.set("hidden", it.hidden);
    return b.build();
}

Value buildTrayHostStatus(const TrayHostStatus& s) {
    ObjectBuilder b;
    b.set("role", to_string(s.role));
    b.set("detail", s.detail);
    return b.build();
}

} // namespace

void installTray(ObjectBuilder& sys) {
    ObjectBuilder tray;

    tray.def("start", 0, [](Value, std::span<const Value> args) {
        if (getTrayHost()) return ev::fromBool(true);

        TrayConfig cfg;
        if (args.size() > 0 && ev::isObject(args[0])) {
            ev::Persistent cfgObj(args[0]);

            ev::Persistent busVal(ev::getProperty(cfgObj.get(), "sessionBusAddress"));
            if (ev::isString(busVal.get())) {
                cfg.session_bus_address = strVal(busVal.get());
            }

            ev::Persistent bwVal(ev::getProperty(cfgObj.get(), "becomeWatcher"));
            if (ev::isBool(bwVal.get())) {
                cfg.become_watcher = boolVal(bwVal.get());
            }
        }

        std::string err;
        auto host = TrayHost::create(cfg, &err);
        if (!host) {
            return ev::throwError("TrayHost start failed: " + err);
        }
        setTrayHost(host.release(), /*own=*/true);
        return ev::fromBool(true);
    });

    tray.def("stop", 0, [](Value, std::span<const Value>) {
        setTrayHost(nullptr, false);
        return ev::fromBool(true);
    });

    tray.def("getStatus", 0, [](Value, std::span<const Value>) {
        TrayHost* host = getTrayHost();
        if (!host) {
            TrayHostStatus empty;
            return buildTrayHostStatus(empty);
        }
        return buildTrayHostStatus(host->status());
    });

    tray.def("getItems", 0, [](Value, std::span<const Value>) {
        TrayHost* host = getTrayHost();
        if (!host) {
            ArrayBuilder empty(0);
            return empty.build();
        }
        auto items = host->items();
        ArrayBuilder arr(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            ev::Persistent v(buildTrayItem(items[i]));
            arr.set(static_cast<uint32_t>(i), v.get());
        }
        return arr.build();
    });

    tray.def("getItem", 1, [](Value, std::span<const Value> args) {
        TrayHost* host = getTrayHost();
        if (!host) return ev::null();

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string id = strVal(arg0.get());

        auto items = host->items();
        for (const auto& item : items) {
            if (item.id == id) {
                return buildTrayItem(item);
            }
        }
        return ev::null();
    });

    tray.def("activate", 1, [](Value, std::span<const Value> args) {
        TrayHost* host = getTrayHost();
        if (!host) return ev::throwError("TrayHost is not running");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        ev::Persistent arg2(args.size() > 2 ? args[2] : ev::undefined());

        std::string id = strVal(arg0.get());
        int32_t x = i32Val(arg1.get());
        int32_t y = i32Val(arg2.get());

        Result r = host->activate(id, x, y);
        return ev::fromBool(r.ok);
    });

    tray.def("secondaryActivate", 1, [](Value, std::span<const Value> args) {
        TrayHost* host = getTrayHost();
        if (!host) return ev::throwError("TrayHost is not running");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        ev::Persistent arg2(args.size() > 2 ? args[2] : ev::undefined());

        std::string id = strVal(arg0.get());
        int32_t x = i32Val(arg1.get());
        int32_t y = i32Val(arg2.get());

        Result r = host->secondary_activate(id, x, y);
        return ev::fromBool(r.ok);
    });

    tray.def("contextMenu", 1, [](Value, std::span<const Value> args) {
        TrayHost* host = getTrayHost();
        if (!host) return ev::throwError("TrayHost is not running");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        ev::Persistent arg2(args.size() > 2 ? args[2] : ev::undefined());

        std::string id = strVal(arg0.get());
        int32_t x = i32Val(arg1.get());
        int32_t y = i32Val(arg2.get());

        Result r = host->context_menu(id, x, y);
        return ev::fromBool(r.ok);
    });

    tray.def("scroll", 2, [](Value, std::span<const Value> args) {
        TrayHost* host = getTrayHost();
        if (!host) return ev::throwError("TrayHost is not running");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        ev::Persistent arg2(args.size() > 2 ? args[2] : ev::undefined());

        std::string id = strVal(arg0.get());
        int32_t delta = i32Val(arg1.get());
        std::string orientStr = strVal(arg2.get());
        ScrollOrientation orient = (orientStr == "horizontal") ? ScrollOrientation::Horizontal : ScrollOrientation::Vertical;

        Result r = host->scroll(id, delta, orient);
        return ev::fromBool(r.ok);
    });

    tray.def("doubleClick", 1, [](Value, std::span<const Value> args) {
        TrayHost* host = getTrayHost();
        if (!host) return ev::throwError("TrayHost is not running");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        ev::Persistent arg2(args.size() > 2 ? args[2] : ev::undefined());

        std::string id = strVal(arg0.get());
        int32_t x = i32Val(arg1.get());
        int32_t y = i32Val(arg2.get());

        Result r = host->double_click(id, x, y);
        return ev::fromBool(r.ok);
    });

    tray.def("getMenu", 1, [](Value, std::span<const Value> args) {
        TrayHost* host = getTrayHost();
        if (!host) return ev::null();

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string id = strVal(arg0.get());

        auto m = host->menu(id);
        if (m) {
            return buildMenuItem(*m);
        }
        return ev::null();
    });

    tray.def("menuAboutToShow", 2, [](Value, std::span<const Value> args) {
        TrayHost* host = getTrayHost();
        if (!host) return ev::throwError("TrayHost is not running");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());

        std::string id = strVal(arg0.get());
        int32_t menuId = i32Val(arg1.get());

        Result r = host->menu_about_to_show(id, menuId);
        return ev::fromBool(r.ok);
    });

    tray.def("menuEvent", 3, [](Value, std::span<const Value> args) {
        TrayHost* host = getTrayHost();
        if (!host) return ev::throwError("TrayHost is not running");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        ev::Persistent arg2(args.size() > 2 ? args[2] : ev::undefined());

        std::string id = strVal(arg0.get());
        int32_t menuId = i32Val(arg1.get());
        std::string eventTypeStr = strVal(arg2.get());

        MenuEventType type = MenuEventType::Clicked;
        if (eventTypeStr == "hovered") type = MenuEventType::Hovered;
        else if (eventTypeStr == "opened") type = MenuEventType::Opened;
        else if (eventTypeStr == "closed") type = MenuEventType::Closed;

        Result r = host->menu_event(id, menuId, type);
        return ev::fromBool(r.ok);
    });

    tray.def("on", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        addEventListener("tray", evName, arg1.get());
        return ev::fromBool(true);
    });

    tray.def("off", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        removeEventListener("tray", evName, arg1.get());
        return ev::fromBool(true);
    });

    sys.set("tray", tray.build());
}

void tickTray() {
    TrayHost* host = getTrayHost();
    if (!host) return;

    auto events = host->events().drain();
    for (const auto& evItem : events) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, TrayItemAdded>) {
                ev::Persistent itemVal(buildTrayItem(e.item));
                dispatchEvent("tray", "itemAdded", itemVal.get());
            } else if constexpr (std::is_same_v<T, TrayItemChanged>) {
                ObjectBuilder b;
                ev::Persistent itemVal(buildTrayItem(e.item));
                b.set("item", itemVal.get());
                b.set("changes", static_cast<double>(e.changes));
                dispatchEvent("tray", "itemChanged", b.build());
            } else if constexpr (std::is_same_v<T, TrayItemRemoved>) {
                ObjectBuilder b;
                b.set("id", e.id);
                dispatchEvent("tray", "itemRemoved", b.build());
            } else if constexpr (std::is_same_v<T, TrayItemDropped>) {
                ObjectBuilder b;
                b.set("id", e.id);
                b.set("reason", e.reason);
                dispatchEvent("tray", "itemDropped", b.build());
            } else if constexpr (std::is_same_v<T, TrayMenuChanged>) {
                ObjectBuilder b;
                b.set("itemId", e.item_id);
                ev::Persistent menuVal(buildMenuItem(e.root));
                b.set("root", menuVal.get());
                dispatchEvent("tray", "menuChanged", b.build());
            } else if constexpr (std::is_same_v<T, TrayHostStatus>) {
                ev::Persistent statusVal(buildTrayHostStatus(e));
                dispatchEvent("tray", "hostStatus", statusVal.get());
            }
        }, evItem);
    }
}

void shutdownTray() {
}

} // namespace brosys::api
