#include "api.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <unordered_map>
#include <string>

namespace brosys::api {

namespace {

std::unordered_map<uint32_t, std::unique_ptr<Inhibitor>> g_inhibitors;
uint32_t g_nextInhibitId = 1;

Value buildPowerDevice(const PowerDevice& d) {
    ObjectBuilder b;
    b.set("id", d.id);
    b.set("kind", to_string(d.kind));
    b.set("powerSupply", d.power_supply);
    b.set("state", to_string(d.state));
    b.set("technology", to_string(d.technology));
    if (d.percent) b.set("percent", *d.percent); else b.setNull("percent");
    if (d.time_to_empty_s) b.set("timeToEmptyS", static_cast<double>(*d.time_to_empty_s)); else b.setNull("timeToEmptyS");
    if (d.time_to_full_s) b.set("timeToFullS", static_cast<double>(*d.time_to_full_s)); else b.setNull("timeToFullS");
    if (d.energy_wh) b.set("energyWh", *d.energy_wh); else b.setNull("energyWh");
    if (d.energy_full_wh) b.set("energyFullWh", *d.energy_full_wh); else b.setNull("energyFullWh");
    if (d.energy_full_design_wh) b.set("energyFullDesignWh", *d.energy_full_design_wh); else b.setNull("energyFullDesignWh");
    if (d.energy_rate_w) b.set("energyRateW", *d.energy_rate_w); else b.setNull("energyRateW");
    b.set("vendor", d.vendor);
    b.set("model", d.model);
    b.set("serial", d.serial);
    return b.build();
}

Value buildPowerState(const PowerState& s) {
    ObjectBuilder b;
    b.set("source", to_string(s.source));
    if (s.percent) b.set("percent", *s.percent); else b.setNull("percent");
    if (s.time_to_empty_s) b.set("timeToEmptyS", static_cast<double>(*s.time_to_empty_s)); else b.setNull("timeToEmptyS");
    if (s.time_to_full_s) b.set("timeToFullS", static_cast<double>(*s.time_to_full_s)); else b.setNull("timeToFullS");
    b.set("lidPresent", s.lid_present);
    b.set("lidClosed", s.lid_closed);
    b.set("hasSystemBattery", s.has_system_battery());

    ArrayBuilder devs(s.devices.size());
    for (size_t i = 0; i < s.devices.size(); ++i) {
        ev::Persistent devVal(buildPowerDevice(s.devices[i]));
        devs.set(static_cast<uint32_t>(i), devVal.get());
    }
    b.set("devices", devs.build());
    return b.build();
}

Value buildPowerCapabilities(const PowerCapabilities& c) {
    ObjectBuilder b;
    b.set("suspend", to_string(c.suspend));
    b.set("hibernate", to_string(c.hibernate));
    b.set("hybridSleep", to_string(c.hybrid_sleep));
    b.set("reboot", to_string(c.reboot));
    b.set("powerOff", to_string(c.power_off));
    b.set("lock", to_string(c.lock));
    return b.build();
}

} // namespace

void installPower(ObjectBuilder& sys) {
    ObjectBuilder power;

    power.def("getState", 0, [](Value, std::span<const Value>) {
        PowerService* svc = getPowerService();
        if (!svc) {
            PowerState empty;
            return buildPowerState(empty);
        }
        return buildPowerState(svc->state());
    });

    power.def("getCapabilities", 0, [](Value, std::span<const Value>) {
        PowerService* svc = getPowerService();
        if (!svc) {
            PowerCapabilities empty;
            return buildPowerCapabilities(empty);
        }
        return buildPowerCapabilities(svc->capabilities());
    });

    power.def("request", 1, [](Value, std::span<const Value> args) {
        PowerService* svc = getPowerService();
        if (!svc) return ev::throwError("PowerService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string actionStr = strVal(arg0.get());
        PowerAction action = PowerAction::Suspend;
        if (actionStr == "suspend") action = PowerAction::Suspend;
        else if (actionStr == "hibernate") action = PowerAction::Hibernate;
        else if (actionStr == "hybridSleep") action = PowerAction::HybridSleep;
        else if (actionStr == "reboot") action = PowerAction::Reboot;
        else if (actionStr == "powerOff") action = PowerAction::PowerOff;
        else if (actionStr == "lock") action = PowerAction::Lock;
        else return ev::throwTypeError("Unknown power action: " + actionStr);

        Result r = svc->request(action);
        if (!r.ok) {
            return ev::throwError("Power request failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    power.def("inhibit", 1, [](Value, std::span<const Value> args) {
        PowerService* svc = getPowerService();
        if (!svc) return ev::throwError("PowerService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        ev::Persistent arg2(args.size() > 2 ? args[2] : ev::undefined());
        ev::Persistent arg3(args.size() > 3 ? args[3] : ev::undefined());

        InhibitRequest req;
        req.why = strVal(arg0.get());
        if (req.why.empty()) req.why = "Inhibited by JS script";

        req.who = strVal(arg1.get());
        if (req.who.empty()) req.who = "bro";

        uint32_t what = u32Val(arg2.get());
        req.what = what != 0 ? what : inhibit::Idle;

        std::string modeStr = strVal(arg3.get());
        req.mode = (modeStr == "delay") ? InhibitMode::Delay : InhibitMode::Block;

        std::string err;
        auto inh = svc->inhibit(req, &err);
        if (!inh) {
            return ev::throwError("Failed to inhibit power: " + err);
        }

        uint32_t id = g_nextInhibitId++;
        g_inhibitors[id] = std::move(inh);
        return ev::fromDouble(static_cast<double>(id));
    });

    power.def("uninhibit", 1, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        uint32_t id = u32Val(arg0.get());

        auto it = g_inhibitors.find(id);
        if (it != g_inhibitors.end()) {
            g_inhibitors.erase(it);
            return ev::fromBool(true);
        }
        return ev::fromBool(false);
    });

    power.def("isInhibited", 0, [](Value, std::span<const Value>) {
        return ev::fromBool(!g_inhibitors.empty());
    });

    // Event listeners
    power.def("on", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        addEventListener("power", evName, arg1.get());
        return ev::fromBool(true);
    });

    power.def("off", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        removeEventListener("power", evName, arg1.get());
        return ev::fromBool(true);
    });

    sys.set("power", power.build());
}

void tickPower() {
    PowerService* svc = getPowerService();
    if (!svc) return;

    auto events = svc->events().drain();
    for (const auto& evItem : events) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, PowerChanged>) {
                ev::Persistent p(buildPowerState(e.state));
                dispatchEvent("power", "changed", p.get());
                dispatchEvent("power", "powerChanged", p.get());
            } else if constexpr (std::is_same_v<T, PowerCapabilitiesChanged>) {
                ev::Persistent p(buildPowerCapabilities(e.capabilities));
                dispatchEvent("power", "capabilitiesChanged", p.get());
            } else if constexpr (std::is_same_v<T, SleepPrepare>) {
                ObjectBuilder b;
                b.set("starting", e.starting);
                dispatchEvent("power", "sleepPrepare", b.build());
            } else if constexpr (std::is_same_v<T, ShutdownPrepare>) {
                ObjectBuilder b;
                b.set("starting", e.starting);
                dispatchEvent("power", "shutdownPrepare", b.build());
            }
        }, evItem);
    }
}

void shutdownPower() {
    g_inhibitors.clear();
}

} // namespace brosys::api
