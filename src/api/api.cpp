#include "api.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

namespace brosys::api {

namespace {

struct Subscription {
    std::string target;
    std::string event;
    std::shared_ptr<ev::Persistent> callback;
};

std::vector<Subscription> g_subscriptions;

struct ServiceState {
    PowerService* power = nullptr;
    bool ownPower = false;

    AudioService* audio = nullptr;
    bool ownAudio = false;

    NetworkService* network = nullptr;
    bool ownNetwork = false;

    BluetoothService* bluetooth = nullptr;
    bool ownBluetooth = false;

    NotificationServer* notifications = nullptr;
    bool ownNotifications = false;

    TrayHost* tray = nullptr;
    bool ownTray = false;
};

ServiceState g_services;

} // namespace

void addEventListener(std::string_view target, std::string_view event, Value callback) {
    if (!ev::isFunction(callback)) return;
    Subscription sub;
    sub.target = std::string(target);
    sub.event = std::string(event);
    sub.callback = std::make_shared<ev::Persistent>(callback);
    g_subscriptions.push_back(std::move(sub));
}

void removeEventListener(std::string_view target, std::string_view event, Value callback) {
    if (!ev::isFunction(callback)) return;
    const uint64_t targetBits = ev::toBits(callback);
    g_subscriptions.erase(
        std::remove_if(g_subscriptions.begin(), g_subscriptions.end(),
                       [&](const Subscription& sub) {
                           if (sub.target != target || sub.event != event) return false;
                           return sub.callback && ev::toBits(sub.callback->get()) == targetBits;
                       }),
        g_subscriptions.end());
}

void dispatchEvent(std::string_view target, std::string_view event, Value payload) {
    ev::Persistent payloadRoot(payload);

    // Snapshot matching callbacks to remain resilient if a callback mutates subscriptions
    std::vector<std::shared_ptr<ev::Persistent>> targets;
    for (const auto& sub : g_subscriptions) {
        if ((sub.target == target || sub.target == "sys") &&
            (sub.event == event || sub.event == "*" ||
             sub.event == (std::string(target) + ":" + std::string(event)))) {
            if (sub.callback) {
                targets.push_back(sub.callback);
            }
        }
    }

    for (const auto& cb : targets) {
        if (!cb) continue;
        const Value arg = payloadRoot.get();
        ev::catchThrow([&]() {
            return ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1)).value;
        });
    }
}

PowerService* getPowerService() {
    if (!g_services.power) {
        std::string err;
        auto svc = PowerService::create(PowerConfig(), &err);
        if (svc) {
            g_services.power = svc.release();
            g_services.ownPower = true;
        }
    }
    return g_services.power;
}

void setPowerService(PowerService* service, bool own) {
    if (g_services.ownPower && g_services.power) {
        delete g_services.power;
    }
    g_services.power = service;
    g_services.ownPower = own;
}

AudioService* getAudioService() {
    if (!g_services.audio) {
        std::string err;
        auto svc = AudioService::create(AudioConfig(), &err);
        if (svc) {
            g_services.audio = svc.release();
            g_services.ownAudio = true;
        }
    }
    return g_services.audio;
}

void setAudioService(AudioService* service, bool own) {
    if (g_services.ownAudio && g_services.audio) {
        delete g_services.audio;
    }
    g_services.audio = service;
    g_services.ownAudio = own;
}

NetworkService* getNetworkService() {
    if (!g_services.network) {
        std::string err;
        auto svc = NetworkService::create(NetworkConfig(), &err);
        if (svc) {
            g_services.network = svc.release();
            g_services.ownNetwork = true;
        }
    }
    return g_services.network;
}

void setNetworkService(NetworkService* service, bool own) {
    if (g_services.ownNetwork && g_services.network) {
        delete g_services.network;
    }
    g_services.network = service;
    g_services.ownNetwork = own;
}

BluetoothService* getBluetoothService() {
    if (!g_services.bluetooth) {
        std::string err;
        auto svc = BluetoothService::create(BluetoothConfig(), &err);
        if (svc) {
            g_services.bluetooth = svc.release();
            g_services.ownBluetooth = true;
        }
    }
    return g_services.bluetooth;
}

void setBluetoothService(BluetoothService* service, bool own) {
    if (g_services.ownBluetooth && g_services.bluetooth) {
        delete g_services.bluetooth;
    }
    g_services.bluetooth = service;
    g_services.ownBluetooth = own;
}

NotificationServer* getNotificationServer() {
    return g_services.notifications;
}

void setNotificationServer(NotificationServer* server, bool own) {
    if (g_services.ownNotifications && g_services.notifications) {
        delete g_services.notifications;
    }
    g_services.notifications = server;
    g_services.ownNotifications = own;
}

TrayHost* getTrayHost() {
    return g_services.tray;
}

void setTrayHost(TrayHost* host, bool own) {
    if (g_services.ownTray && g_services.tray) {
        delete g_services.tray;
    }
    g_services.tray = host;
    g_services.ownTray = own;
}

void installSys() {
    // Locate or create the `bro` namespace
    ev::GlobalValue g = ev::globalValue("bro");
    ev::Persistent broNs(g.found && ev::isObject(g.value) ? g.value : ev::createObject());

    // Locate or create `bro.sys`
    ev::Persistent sysProp(ev::getProperty(broNs.get(), "sys"));
    ObjectBuilder sys(sysProp.get().isObject() ? sysProp.get() : ev::createObject());

    // Event API on bro.sys
    sys.def("on", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        addEventListener("sys", evName, arg1.get());
        return ev::fromBool(true);
    });

    sys.def("off", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        removeEventListener("sys", evName, arg1.get());
        return ev::fromBool(true);
    });

    sys.def("addEventListener", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        addEventListener("sys", evName, arg1.get());
        return ev::fromBool(true);
    });

    sys.def("removeEventListener", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        removeEventListener("sys", evName, arg1.get());
        return ev::fromBool(true);
    });

    // Install all subsystem APIs
    installPower(sys);
    installAudio(sys);
    installNetwork(sys);
    installBluetooth(sys);
    installNotifications(sys);
    installTray(sys);

    // Attach sys to bro
    broNs.set(ev::setProperty(broNs.get(), "sys", sys.get()));

    // Register bro global and attach to globalThis
    ev::registerGlobal("bro", broNs.get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && !gt.value.isUndefined() && ev::isObject(gt.value)) {
        ev::setProperty(gt.value, "bro", broNs.get());
    }
}

void tickSysAsync() {
    tickPower();
    tickAudio();
    tickNetwork();
    tickBluetooth();
    tickNotifications();
    tickTray();

    ev::drainMicrotasks();
}

void shutdownSysAsync() {
    shutdownPower();
    shutdownAudio();
    shutdownNetwork();
    shutdownBluetooth();
    shutdownNotifications();
    shutdownTray();

    g_subscriptions.clear();

    if (g_services.ownPower && g_services.power) {
        delete g_services.power;
        g_services.power = nullptr;
        g_services.ownPower = false;
    }
    if (g_services.ownAudio && g_services.audio) {
        delete g_services.audio;
        g_services.audio = nullptr;
        g_services.ownAudio = false;
    }
    if (g_services.ownNetwork && g_services.network) {
        delete g_services.network;
        g_services.network = nullptr;
        g_services.ownNetwork = false;
    }
    if (g_services.ownBluetooth && g_services.bluetooth) {
        delete g_services.bluetooth;
        g_services.bluetooth = nullptr;
        g_services.ownBluetooth = false;
    }
    if (g_services.ownNotifications && g_services.notifications) {
        delete g_services.notifications;
        g_services.notifications = nullptr;
        g_services.ownNotifications = false;
    }
    if (g_services.ownTray && g_services.tray) {
        delete g_services.tray;
        g_services.tray = nullptr;
        g_services.ownTray = false;
    }
}

} // namespace brosys::api
