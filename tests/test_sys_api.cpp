#include "brosys/api.h"
#include "brosys/processes.h"
#include "api.h"
#include "embed/embed.h"
#include "eval/eval.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

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
        "power", "audio", "network", "bluetooth", "notifications", "tray", "processes", "journal", "disks"
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

static void test_processes_api() {
    runScript("Processes API listProcesses and killProcess validation", R"JS(
        if (typeof bro.sys.listProcesses !== "function") throw new Error("missing bro.sys.listProcesses");
        if (typeof bro.sys.killProcess !== "function") throw new Error("missing bro.sys.killProcess");
        if (typeof bro.sys.processes.list !== "function") throw new Error("missing bro.sys.processes.list");
        if (typeof bro.sys.processes.kill !== "function") throw new Error("missing bro.sys.processes.kill");

        const procs = bro.sys.listProcesses();
        if (!Array.isArray(procs) || procs.length === 0) throw new Error("listProcesses returned empty");
        const p = procs[0];
        if (typeof p.pid !== "number" || p.pid <= 0) throw new Error("invalid pid");
        if (typeof p.ppid !== "number") throw new Error("invalid ppid");
        if (typeof p.name !== "string" || p.name.length === 0) throw new Error("invalid name");
        if (typeof p.cmdline !== "string") throw new Error("invalid cmdline");
        if (typeof p.state !== "string" || p.state.length === 0) throw new Error("invalid state");
        if (typeof p.user !== "string") throw new Error("invalid user");
        if (typeof p.uid !== "number") throw new Error("invalid uid");
        if (typeof p.rssBytes !== "number" || typeof p.rss_bytes !== "number") throw new Error("invalid rssBytes");
        if (typeof p.vszBytes !== "number" || typeof p.vsz_bytes !== "number") throw new Error("invalid vszBytes");
        if (typeof p.cpuTimeMs !== "number" || typeof p.cpu_time_ms !== "number") throw new Error("invalid cpuTimeMs");
        if (typeof p.threads !== "number" || p.threads < 1) throw new Error("invalid threads");

        const procs2 = bro.sys.processes.list();
        if (!Array.isArray(procs2) || procs2.length === 0) throw new Error("processes.list returned empty");

        let caughtPid1 = false;
        try {
            bro.sys.killProcess(1, "SIGTERM");
        } catch (e) {
            caughtPid1 = true;
        }
        if (!caughtPid1) throw new Error("expected killProcess(1) to throw");

        let caughtBadSig = false;
        try {
            bro.sys.processes.kill(999999, "NOT_A_SIGNAL");
        } catch (e) {
            caughtBadSig = true;
        }
        if (!caughtBadSig) throw new Error("expected unknown signal to throw");

        return "SUCCESS";
    )JS");
}

// Ticks until `globalThis[flag]` is set (a promise chain settled) or ~10 s pass.
static void pumpUntil(const char* flag) {
    const std::string probe = std::string("globalThis.") + flag + " ? 'yes' : 'no'";
    for (int i = 0; i < 1000; ++i) {
        brosys::api::tickSysAsync();
        ev::CallResult r = bronze::eval::evalScript(probe);
        if (!r.thrown && ev::isString(r.value) && ev::toUtf8(r.value) == "yes") return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cerr << "timed out waiting for " << flag << std::endl;
    std::exit(1);
}

static void test_journal_api() {
    runScript("Journal API shapes and argument checks", R"JS(
        const j = bro.sys.journal;
        if (typeof j !== "object" || j === null) throw new Error("missing bro.sys.journal");
        for (const f of ["query", "boots", "values", "follow"])
            if (typeof j[f] !== "function") throw new Error("missing journal." + f);
        if (j.priorities.length !== 8 || j.priorities[3] !== "err") throw new Error("priorities");
        let threw = false;
        try { j.follow({}); } catch (e) { threw = true; }
        if (!threw) throw new Error("follow without a callback must throw");

        globalThis.__journal = null;
        globalThis.__journalDone = false;
        const note = (k, v) => { globalThis.__journal = globalThis.__journal || {}; globalThis.__journal[k] = v; };
        Promise.allSettled([
            j.query({ limit: 5, priority: "info" }),
            j.query({ priority: "loud" }),
            j.boots(),
            j.values("_TRANSPORT"),
        ]).then((r) => {
            note("results", r);
            globalThis.__journalDone = true;
        });
        return "SUCCESS";
    )JS");
    pumpUntil("__journalDone");
    runScript("Journal API results", R"JS(
        const [page, badPrio, boots, values] = globalThis.__journal.results;
        if (badPrio.status !== "rejected" || !(badPrio.reason instanceof Error)) throw new Error("bad priority must reject");
        if (page.status === "rejected") {
            // No readable journal here: every call says why.
            if (!String(page.reason.message || page.reason)) throw new Error("rejection without a reason");
            return "SUCCESS";
        }
        const p = page.value;
        if (!Array.isArray(p.entries) || p.entries.length > 5) throw new Error("entries");
        if (typeof p.more !== "boolean") throw new Error("more");
        for (const e of p.entries) {
            if (typeof e.cursor !== "string" || !e.cursor) throw new Error("cursor");
            if (typeof e.time !== "number" || e.time <= 0) throw new Error("time");
            if (typeof e.message !== "string") throw new Error("message");
            if (e.priority !== null && (e.priority < 0 || e.priority > 6)) throw new Error("priority filter: " + e.priority);
        }
        if (boots.status !== "fulfilled" || !Array.isArray(boots.value)) throw new Error("boots");
        if (values.status !== "fulfilled" || !Array.isArray(values.value)) throw new Error("values");
        return "SUCCESS";
    )JS");
}

// ioCounters' shape over the machine's real counters, then its values through
// a ProcessService that answers fixed ones.
class FixedIoService : public brosys::ProcessService {
public:
    brosys::IoCounters io_counters() const override {
        brosys::IoCounters c;
        c.timestamp_ms = 1234.5;
        c.disks.push_back({"disk-a", 6000000000ull, 2048, 7, 3});
        c.interfaces.push_back({"lo", true, 10, 20, 1, 2});
        c.interfaces.push_back({"eth9", false, 5000000000ull, 4000, 50, 40});
        return c;
    }
};

static void test_io_counters_api() {
    runScript("Processes API ioCounters on this machine", R"JS(
        if (typeof bro.sys.processes.ioCounters !== "function") throw new Error("missing bro.sys.processes.ioCounters");
        const a = bro.sys.processes.ioCounters();
        if (typeof a.timestamp !== "number" || !(a.timestamp > 0)) throw new Error("invalid timestamp");
        if (!Array.isArray(a.disks) || !Array.isArray(a.interfaces)) throw new Error("disks / interfaces not arrays");
        for (const d of a.disks)
            for (const k of ["readBytes", "writeBytes", "reads", "writes"])
                if (typeof d[k] !== "number" || d[k] < 0 || typeof d.name !== "string") throw new Error("bad disk " + k);
        for (const n of a.interfaces) {
            for (const k of ["rxBytes", "txBytes", "rxPackets", "txPackets"])
                if (typeof n[k] !== "number" || n[k] < 0) throw new Error("bad interface " + k);
            if (typeof n.name !== "string" || typeof n.loopback !== "boolean") throw new Error("bad interface name");
        }
        const b = bro.sys.processes.ioCounters();
        if (!(b.timestamp >= a.timestamp)) throw new Error("timestamp went back");
        return "SUCCESS";
    )JS");

    FixedIoService fixed;
    brosys::api::setProcessService(&fixed, false);
    runScript("Processes API ioCounters values", R"JS(
        const c = bro.sys.processes.ioCounters();
        if (c.timestamp !== 1234.5) throw new Error("timestamp " + c.timestamp);
        if (c.disks.length !== 1 || c.interfaces.length !== 2) throw new Error("counts");
        const d = c.disks[0];
        if (d.name !== "disk-a" || d.readBytes !== 6000000000 || d.writeBytes !== 2048 || d.reads !== 7 || d.writes !== 3)
            throw new Error("disk " + JSON.stringify(d));
        const [lo, eth] = c.interfaces;
        if (lo.name !== "lo" || lo.loopback !== true || lo.rxBytes !== 10 || lo.txBytes !== 20) throw new Error("lo");
        if (eth.name !== "eth9" || eth.loopback !== false || eth.rxBytes !== 5000000000 || eth.txBytes !== 4000 ||
            eth.rxPackets !== 50 || eth.txPackets !== 40) throw new Error("eth " + JSON.stringify(eth));
        return "SUCCESS";
    )JS");
    brosys::api::setProcessService(nullptr, false);
}

static void test_disks_api() {
    runScript("Disks API shapes and argument checks", R"JS(
        const d = bro.sys.disks;
        if (typeof d !== "object" || d === null) throw new Error("missing bro.sys.disks");
        for (const f of ["getState", "getDrives", "getBlocks", "getDrive", "getBlock", "mount", "unmount",
                         "eject", "powerOff", "smart", "on", "off"])
            if (typeof d[f] !== "function") throw new Error("missing disks." + f);
        const st = d.getState();
        if (typeof st.available !== "boolean" || !Array.isArray(st.drives) || !Array.isArray(st.blocks))
            throw new Error("getState shape");
        for (const b of st.blocks) {
            if (typeof b.device !== "string" || !Array.isArray(b.mountPoints)) throw new Error("block shape");
            if (b.mounted !== (b.mountPoints.length > 0)) throw new Error("mounted");
        }
        if (d.getBlock("/dev/brosys-no-such-device") !== null) throw new Error("unknown block is null");
        d.on("change", () => {});
        globalThis.__disksDone = false;
        const smartable = st.drives.find(x => x.smartSupported);
        Promise.allSettled([
            d.mount(""),
            d.unmount("/dev/brosys-no-such-device"),
            smartable ? d.smart(smartable.id) : Promise.resolve(null),
        ]).then((r) => { globalThis.__disks = r; globalThis.__disksDone = true; });
        return "SUCCESS";
    )JS");
    pumpUntil("__disksDone");
    runScript("Disks API results", R"JS(
        const [noArg, unknown, smart] = globalThis.__disks;
        if (noArg.status !== "rejected" || !(noArg.reason instanceof Error)) throw new Error("mount('') rejects");
        if (unknown.status !== "rejected") throw new Error("an unknown device rejects");
        if (smart.status === "rejected") throw new Error("smart rejected: " + smart.reason.message);
        if (smart.value) {
            const s = smart.value;
            if (typeof s.assessment !== "string" || !Array.isArray(s.attributes)) throw new Error("smart shape");
        }
        return "SUCCESS";
    )JS");
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
        test_processes_api();
        test_journal_api();
        test_io_counters_api();
        test_disks_api();
        test_events_and_async_tick();
        test_gc_stress();

        brosys::api::shutdownSysAsync();
    }
    ev::destroyRealm(realm);

    std::cout << "All brosys API unit tests passed!" << std::endl;
    return 0;
}
