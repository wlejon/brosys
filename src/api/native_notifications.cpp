#include "api.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <string>

namespace brosys::api {

namespace {

Value buildNotificationAction(const NotificationAction& a) {
    ObjectBuilder b;
    b.set("key", a.key);
    b.set("label", a.label);
    return b.build();
}

Value buildNotification(const Notification& n) {
    ObjectBuilder b;
    b.set("id", static_cast<double>(n.id));
    b.set("appName", n.app_name);
    b.set("appIcon", n.app_icon);
    b.set("summary", n.summary);
    b.set("body", n.body);

    ArrayBuilder acts(n.actions.size());
    for (size_t i = 0; i < n.actions.size(); ++i) {
        ev::Persistent v(buildNotificationAction(n.actions[i]));
        acts.set(static_cast<uint32_t>(i), v.get());
    }
    b.set("actions", acts.build());

    b.set("urgency", to_string(n.urgency));
    b.set("category", n.category);
    b.set("desktopEntry", n.desktop_entry);
    b.set("imagePath", n.image_path);
    b.set("soundFile", n.sound_file);
    b.set("soundName", n.sound_name);
    b.set("suppressSound", n.suppress_sound);
    b.set("transient", n.transient);
    b.set("resident", n.resident);
    b.set("actionIcons", n.action_icons);
    b.set("expireTimeoutMs", static_cast<double>(n.expire_timeout_ms));
    b.set("sender", n.sender);
    b.set("senderPid", static_cast<double>(n.sender_pid));
    return b.build();
}

Value buildNotificationServerCapabilities(const NotificationServerCapabilities& c) {
    ObjectBuilder b;
    b.set("receivesForeign", c.receives_foreign);
    b.set("source", c.source);
    b.set("detail", c.detail);
    return b.build();
}

} // namespace

void installNotifications(ObjectBuilder& sys) {
    ObjectBuilder notifications;

    notifications.def("listen", 0, [](Value, std::span<const Value> args) {
        if (getNotificationServer()) return ev::fromBool(true);

        NotificationServerConfig cfg;
        if (args.size() > 0 && ev::isObject(args[0])) {
            ev::Persistent cfgObj(args[0]);

            ev::Persistent busVal(ev::getProperty(cfgObj.get(), "sessionBusAddress"));
            if (ev::isString(busVal.get())) {
                cfg.session_bus_address = strVal(busVal.get());
            }

            ev::Persistent replVal(ev::getProperty(cfgObj.get(), "replaceExisting"));
            if (ev::isBool(replVal.get())) {
                cfg.replace_existing = boolVal(replVal.get());
            }

            ev::Persistent dndVal(ev::getProperty(cfgObj.get(), "doNotDisturb"));
            if (ev::isBool(dndVal.get())) {
                cfg.do_not_disturb = boolVal(dndVal.get());
            }
        }

        std::string err;
        auto s = NotificationServer::create(cfg, &err);
        if (!s) {
            return ev::throwError("NotificationServer listen failed: " + err);
        }
        setNotificationServer(s.release(), /*own=*/true);
        return ev::fromBool(true);
    });

    notifications.def("stop", 0, [](Value, std::span<const Value>) {
        setNotificationServer(nullptr, false);
        return ev::fromBool(true);
    });

    notifications.def("dismiss", 1, [](Value, std::span<const Value> args) {
        NotificationServer* s = getNotificationServer();
        if (!s) return ev::throwError("NotificationServer is not running");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        uint32_t id = u32Val(arg0.get());

        Result r = s->close(id, CloseReason::Dismissed);
        return ev::fromBool(r.ok);
    });

    notifications.def("close", 1, [](Value, std::span<const Value> args) {
        NotificationServer* s = getNotificationServer();
        if (!s) return ev::throwError("NotificationServer is not running");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());

        uint32_t id = u32Val(arg0.get());
        std::string reasonStr = strVal(arg1.get());
        CloseReason reason = CloseReason::Closed;
        if (reasonStr == "expired") reason = CloseReason::Expired;
        else if (reasonStr == "dismissed") reason = CloseReason::Dismissed;
        else if (reasonStr == "undefined") reason = CloseReason::Undefined;

        Result r = s->close(id, reason);
        return ev::fromBool(r.ok);
    });

    notifications.def("invokeAction", 2, [](Value, std::span<const Value> args) {
        NotificationServer* s = getNotificationServer();
        if (!s) return ev::throwError("NotificationServer is not running");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        ev::Persistent arg2(args.size() > 2 ? args[2] : ev::undefined());

        uint32_t id = u32Val(arg0.get());
        std::string key = strVal(arg1.get());
        std::string token = strVal(arg2.get());

        Result r = s->invoke_action(id, key, token);
        return ev::fromBool(r.ok);
    });

    notifications.def("getActive", 0, [](Value, std::span<const Value>) {
        NotificationServer* s = getNotificationServer();
        if (!s) {
            ArrayBuilder empty(0);
            return empty.build();
        }

        auto active = s->active();
        ArrayBuilder arr(active.size());
        for (size_t i = 0; i < active.size(); ++i) {
            ev::Persistent v(buildNotification(active[i]));
            arr.set(static_cast<uint32_t>(i), v.get());
        }
        return arr.build();
    });

    notifications.def("getHistory", 0, [](Value, std::span<const Value>) {
        NotificationServer* s = getNotificationServer();
        if (!s) {
            ArrayBuilder empty(0);
            return empty.build();
        }

        auto history = s->history();
        ArrayBuilder arr(history.size());
        for (size_t i = 0; i < history.size(); ++i) {
            ev::Persistent v(buildNotification(history[i]));
            arr.set(static_cast<uint32_t>(i), v.get());
        }
        return arr.build();
    });

    notifications.def("clearHistory", 0, [](Value, std::span<const Value>) {
        NotificationServer* s = getNotificationServer();
        if (!s) return ev::fromBool(false);
        s->clear_history();
        return ev::fromBool(true);
    });

    notifications.def("removeFromHistory", 1, [](Value, std::span<const Value> args) {
        NotificationServer* s = getNotificationServer();
        if (!s) return ev::fromBool(false);

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        uint32_t id = u32Val(arg0.get());
        return ev::fromBool(s->remove_from_history(id));
    });

    notifications.def("post", 1, [](Value, std::span<const Value> args) {
        NotificationServer* s = getNotificationServer();
        if (!s) return ev::throwError("NotificationServer is not running");

        if (args.empty() || !ev::isObject(args[0])) {
            return ev::throwTypeError("Notification must be an object");
        }

        ev::Persistent notifObj(args[0]);
        Notification n;

        ev::Persistent appNameVal(ev::getProperty(notifObj.get(), "appName"));
        n.app_name = strVal(appNameVal.get());

        ev::Persistent appIconVal(ev::getProperty(notifObj.get(), "appIcon"));
        n.app_icon = strVal(appIconVal.get());

        ev::Persistent summaryVal(ev::getProperty(notifObj.get(), "summary"));
        n.summary = strVal(summaryVal.get());

        ev::Persistent bodyVal(ev::getProperty(notifObj.get(), "body"));
        n.body = strVal(bodyVal.get());

        ev::Persistent urgencyVal(ev::getProperty(notifObj.get(), "urgency"));
        std::string urg = strVal(urgencyVal.get());
        if (urg == "low") n.urgency = Urgency::Low;
        else if (urg == "critical") n.urgency = Urgency::Critical;
        else n.urgency = Urgency::Normal;

        ev::Persistent categoryVal(ev::getProperty(notifObj.get(), "category"));
        n.category = strVal(categoryVal.get());

        ev::Persistent toVal(ev::getProperty(notifObj.get(), "expireTimeoutMs"));
        if (ev::isNumber(toVal.get())) {
            n.expire_timeout_ms = i32Val(toVal.get());
        }

        uint32_t id = s->post(n);
        return ev::fromDouble(static_cast<double>(id));
    });

    notifications.def("setDoNotDisturb", 1, [](Value, std::span<const Value> args) {
        NotificationServer* s = getNotificationServer();
        if (!s) return ev::throwError("NotificationServer is not running");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        bool dnd = boolVal(arg0.get());
        s->set_do_not_disturb(dnd);
        return ev::fromBool(true);
    });

    notifications.def("isDoNotDisturb", 0, [](Value, std::span<const Value>) {
        NotificationServer* s = getNotificationServer();
        if (!s) return ev::fromBool(false);
        return ev::fromBool(s->is_do_not_disturb());
    });

    notifications.def("getCapabilities", 0, [](Value, std::span<const Value>) {
        NotificationServer* s = getNotificationServer();
        if (!s) {
            NotificationServerCapabilities empty;
            return buildNotificationServerCapabilities(empty);
        }
        return buildNotificationServerCapabilities(s->capabilities());
    });

    notifications.def("on", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        addEventListener("notifications", evName, arg1.get());
        return ev::fromBool(true);
    });

    notifications.def("off", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        removeEventListener("notifications", evName, arg1.get());
        return ev::fromBool(true);
    });

    sys.set("notifications", notifications.build());
}

void tickNotifications() {
    NotificationServer* s = getNotificationServer();
    if (!s) return;

    auto events = s->events().drain();
    for (const auto& evItem : events) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, NotificationPosted>) {
                ObjectBuilder b;
                ev::Persistent nVal(buildNotification(e.notification));
                b.set("notification", nVal.get());
                b.set("replaced", e.replaced);
                b.set("popupSuppressed", e.popup_suppressed);

                dispatchEvent("notifications", "posted", b.build());
                dispatchEvent("notifications", "notificationPosted", b.build());
            } else if constexpr (std::is_same_v<T, NotificationClosed>) {
                ObjectBuilder b;
                b.set("id", static_cast<double>(e.id));
                b.set("reason", to_string(e.reason));

                dispatchEvent("notifications", "closed", b.build());
                dispatchEvent("notifications", "notificationClosed", b.build());
            } else if constexpr (std::is_same_v<T, NotificationServerStatus>) {
                ObjectBuilder b;
                b.set("active", e.active);
                b.set("detail", e.detail);

                dispatchEvent("notifications", "serverStatus", b.build());
            } else if constexpr (std::is_same_v<T, DoNotDisturbChanged>) {
                ObjectBuilder b;
                b.set("enabled", e.enabled);

                dispatchEvent("notifications", "dndChanged", b.build());
            }
        }, evItem);
    }
}

void shutdownNotifications() {
}

} // namespace brosys::api
