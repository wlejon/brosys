#include "brosys/api.h"
#include "embed/embed.h"
#include "eval/eval.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace ev = bronze::embed;
using Value = bronze::Value;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::exit(1); \
    } \
} while (0)

static void runScript(const char* label, const std::string& script) {
    std::cout << "  [test] " << label << "..." << std::endl;
    ev::CallResult res = bronze::eval::evalScript(script);
    if (res.thrown) {
        std::cerr << label << " THREW: " << ev::toUtf8(res.value) << std::endl;
        std::exit(1);
    }
    if (ev::isString(res.value) && ev::toUtf8(res.value) != "SUCCESS") {
        std::cerr << label << " RETURNED: " << ev::toUtf8(res.value) << std::endl;
        std::exit(1);
    }
}

static void test_mounting() {
    auto g = ev::globalValue("bro");
    CHECK(g.found);
    CHECK(ev::isObject(g.value));

    ev::Persistent sys(ev::getProperty(g.value, "sys"));
    CHECK(ev::isObject(sys.get()));

    const char* subNames[] = {
        "power", "audio", "network", "bluetooth", "notifications", "tray"
    };
    for (const char* name : subNames) {
        ev::Persistent sub(ev::getProperty(sys.get(), name));
        CHECK(ev::isObject(sub.get()));
        std::cout << "  Found bro.sys." << name << std::endl;
    }
}

static void test_power_api() {
    runScript("Power API basics and inhibitor lifecycle", R"JS(
        const p = bro.sys.power.getState();
        if (typeof p !== "object" || p === null) throw new Error("getState failed");
        if (typeof p.source !== "string") throw new Error("missing power source string");
        if (!Array.isArray(p.devices)) throw new Error("devices must be array");
        if (typeof p.hasSystemBattery !== "boolean") throw new Error("hasSystemBattery missing");

        const caps = bro.sys.power.getCapabilities();
        if (typeof caps !== "object" || caps === null) throw new Error("getCapabilities failed");
        if (typeof caps.suspend !== "string") throw new Error("suspend capability missing");

        if (bro.sys.power.isInhibited()) throw new Error("expected not inhibited initially");

        const id = bro.sys.power.inhibit("test lock", "test_suite");
        if (typeof id !== "number" || id <= 0) throw new Error("inhibit did not return valid id");
        if (!bro.sys.power.isInhibited()) throw new Error("expected isInhibited to be true");

        const uninh = bro.sys.power.uninhibit(id);
        if (!uninh) throw new Error("uninhibit failed");
        if (bro.sys.power.isInhibited()) throw new Error("expected isInhibited to be false after uninhibit");

        return "SUCCESS";
    )JS");
}

static void test_audio_api() {
    runScript("Audio API basics and device getters", R"JS(
        const st = bro.sys.audio.getState();
        if (typeof st !== "object" || st === null) throw new Error("getState failed");
        if (!Array.isArray(st.devices)) throw new Error("devices must be array");
        if (typeof st.defaultOutput !== "string") throw new Error("defaultOutput must be string");

        const devs = bro.sys.audio.getDevices();
        if (!Array.isArray(devs)) throw new Error("getDevices must return array");

        if (devs.length > 0) {
            const d = devs[0];
            if (typeof d.id !== "string") throw new Error("device id must be string");
            if (typeof d.volume !== "number") throw new Error("volume must be number");
            if (typeof d.muted !== "boolean") throw new Error("muted must be boolean");
        }

        // The setters act on the machine's real devices, so they run only
        // with BROSYS_TEST_MUTATE=1, and then only re-apply what the current
        // default output already has: its volume, its mute state, and its
        // being the default.
        if (globalThis.__brosysMutate && st.defaultOutput) {
            const d = devs.find(x => x.id === st.defaultOutput);
            if (d) {
                bro.sys.audio.setVolume(d.id, d.volume);
                bro.sys.audio.setMute(d.id, d.muted);
                bro.sys.audio.setDefaultSink(d.id);
            }
        }

        return "SUCCESS";
    )JS");
}

static void test_network_api() {
    runScript("Network API basics and Wi-Fi scan promises", R"JS(
        const net = bro.sys.network.getState();
        if (typeof net !== "object" || net === null) throw new Error("getState failed");
        if (typeof net.connectivity !== "string") throw new Error("connectivity string missing");
        if (!Array.isArray(net.devices)) throw new Error("devices must be array");
        if (!Array.isArray(net.activeConnections)) throw new Error("activeConnections must be array");

        const aps = bro.sys.network.getAccessPoints();
        if (!Array.isArray(aps)) throw new Error("getAccessPoints must be array");

        const scan = bro.sys.network.scanWifi();
        if (!(scan instanceof Promise)) throw new Error("scanWifi must return a Promise");

        // NetworkManager saves a profile for a connect attempt, so only with
        // BROSYS_TEST_MUTATE=1.
        if (globalThis.__brosysMutate) {
            const conn = bro.sys.network.connectWifi("NonExistentSSID", "secret");
            if (!(conn instanceof Promise)) throw new Error("connectWifi must return a Promise");
        }

        return "SUCCESS";
    )JS");
}

static void test_bluetooth_api() {
    runScript("Bluetooth API basics and device discovery methods", R"JS(
        const bt = bro.sys.bluetooth.getState();
        if (typeof bt !== "object" || bt === null) throw new Error("getState failed");
        if (!Array.isArray(bt.adapters)) throw new Error("adapters must be array");
        if (!Array.isArray(bt.devices)) throw new Error("devices must be array");

        const adapters = bro.sys.bluetooth.getAdapters();
        if (!Array.isArray(adapters)) throw new Error("getAdapters must be array");

        const devs = bro.sys.bluetooth.getDevices();
        if (!Array.isArray(devs)) throw new Error("getDevices must be array");

        // Discovery drives the machine's real adapter: BROSYS_TEST_MUTATE=1 only.
        if (globalThis.__brosysMutate) {
            try { bro.sys.bluetooth.startDiscovery(); } catch (e) {}
            try { bro.sys.bluetooth.stopDiscovery(); } catch (e) {}
        }

        const nonDev = bro.sys.bluetooth.getDevice("00:00:00:00:00:00");
        if (nonDev !== null) throw new Error("expected null for non-existent device");

        return "SUCCESS";
    )JS");
}

static void test_notifications_api() {
    runScript("Notifications API server lifecycle, posting and history", R"JS(
        const caps = bro.sys.notifications.getCapabilities();
        if (typeof caps !== "object" || caps === null) throw new Error("capabilities missing");
        if (typeof caps.receivesForeign !== "boolean") throw new Error("receivesForeign missing");

        // listen() claims org.freedesktop.Notifications on the user's session
        // bus on Linux, standing in for the desktop's notification daemon:
        // BROSYS_TEST_MUTATE=1 only.
        if (!globalThis.__brosysMutate) return "SUCCESS";
        bro.sys.notifications.listen();

        const id = bro.sys.notifications.post({
            appName: "TestApp",
            summary: "Notification Title",
            body: "Notification body content",
            urgency: "normal",
            expireTimeoutMs: 5000
        });
        if (typeof id !== "number") throw new Error("post must return number");

        const active = bro.sys.notifications.getActive();
        if (!Array.isArray(active)) throw new Error("getActive must return array");

        const history = bro.sys.notifications.getHistory();
        if (!Array.isArray(history)) throw new Error("getHistory must return array");

        bro.sys.notifications.setDoNotDisturb(true);
        if (!bro.sys.notifications.isDoNotDisturb()) throw new Error("isDoNotDisturb should be true");
        bro.sys.notifications.setDoNotDisturb(false);
        if (bro.sys.notifications.isDoNotDisturb()) throw new Error("isDoNotDisturb should be false");

        if (id > 0) {
            bro.sys.notifications.dismiss(id);
            bro.sys.notifications.invokeAction(id, "default");
        }

        bro.sys.notifications.clearHistory();
        return "SUCCESS";
    )JS");
}

static void test_tray_api() {
    runScript("Tray API host lifecycle, items and status", R"JS(
        // start() hosts a StatusNotifier watcher on the user's session bus, and
        // the item calls click other applications' real tray icons:
        // BROSYS_TEST_MUTATE=1 only.
        if (!globalThis.__brosysMutate) return "SUCCESS";
        bro.sys.tray.start();
        const st = bro.sys.tray.getStatus();
        if (typeof st !== "object" || st === null) throw new Error("getStatus failed");
        if (typeof st.role !== "string") throw new Error("role must be string");

        const items = bro.sys.tray.getItems();
        if (!Array.isArray(items)) throw new Error("getItems must return array");

        if (items.length > 0) {
            const it = items[0];
            if (typeof it.id !== "string") throw new Error("item id must be string");
            bro.sys.tray.activate(it.id, 0, 0);
            bro.sys.tray.secondaryActivate(it.id, 0, 0);
            bro.sys.tray.contextMenu(it.id, 0, 0);
            bro.sys.tray.scroll(it.id, 1, "vertical");
            bro.sys.tray.getMenu(it.id);
        }

        return "SUCCESS";
    )JS");
}

static void test_events_and_async_tick() {
    runScript("Event subscriptions and async tick handling", R"JS(
        let powerCalled = false;
        let audioCalled = false;
        let anyCalled = 0;

        bro.sys.power.on("changed", (st) => {
            powerCalled = true;
        });

        bro.sys.audio.on("deviceAdded", (d) => {
            audioCalled = true;
        });

        bro.sys.on("*", () => {
            anyCalled++;
        });

        return "SUCCESS";
    )JS");

    // Pump async tick
    std::cout << "  Ticking async system events..." << std::endl;
    brosys::api::tickSysAsync();
}

static void test_gc_stress() {
    std::cout << "  Running GC stress and heap churning test..." << std::endl;
    runScript("GC stress under high heap allocation pressure", R"JS(
        function churn() {
            const junk = [];
            for (let i = 0; i < 50; i++) {
                junk.push({ index: i, str: "stress_test_string_" + i, nested: { x: i * 2 } });
            }
            return junk.length;
        }

        // Attach listener that churns memory during callbacks
        bro.sys.power.on("changed", () => { churn(); });
        bro.sys.audio.on("deviceAdded", () => { churn(); });
        bro.sys.network.on("changed", () => { churn(); });
        bro.sys.on("sys:tick", () => { churn(); });

        for (let i = 0; i < 20; i++) {
            churn();
            const p = bro.sys.power.getState();
            const a = bro.sys.audio.getState();
            const n = bro.sys.network.getState();
            const b = bro.sys.bluetooth.getState();
            if (!p || !a || !n || !b) throw new Error("State read failed during churn");
        }

        return "SUCCESS";
    )JS");

    // Pump events under GC stress
    for (int i = 0; i < 5; ++i) {
        brosys::api::tickSysAsync();
    }
}

int main() {
    std::cout << "Running brosys API unit tests..." << std::endl;
    const char* stress = std::getenv("BRONZE_GC_STRESS");
    std::cout << "  (BRONZE_GC_STRESS=" << (stress ? stress : "unset") << ")" << std::endl;

    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        brosys::api::installSys();

        // Calls that act on the machine's real devices or desktop services run
        // only with BROSYS_TEST_MUTATE=1 (CI runners are disposable and set it).
        const char* mutate = std::getenv("BROSYS_TEST_MUTATE");
        const bool doMutate = mutate && std::string(mutate) == "1";
        bronze::eval::evalScript(doMutate ? "globalThis.__brosysMutate = true;"
                                          : "globalThis.__brosysMutate = false;");
        if (!doMutate) {
            std::cout << "Note: audio setters, Wi-Fi connect, Bluetooth discovery, the notification "
                         "server and the tray host were not exercised; set BROSYS_TEST_MUTATE=1 to "
                         "run them" << std::endl;
        }

        test_mounting();
        test_power_api();
        test_audio_api();
        test_network_api();
        test_bluetooth_api();
        test_notifications_api();
        test_tray_api();
        test_events_and_async_tick();
        test_gc_stress();

        brosys::api::shutdownSysAsync();
    }
    ev::destroyRealm(realm);

    std::cout << "All brosys API unit tests passed!" << std::endl;
    return 0;
}
