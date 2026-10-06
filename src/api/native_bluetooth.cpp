#include "api.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <string>

namespace brosys::api {

namespace {

Value buildBluetoothAdapter(const BluetoothAdapter& a) {
    ObjectBuilder b;
    b.set("id", a.id);
    b.set("address", a.address);
    b.set("name", a.name);
    b.set("alias", a.alias);
    b.set("powered", a.powered);
    b.set("discovering", a.discovering);
    b.set("pairable", a.pairable);
    b.set("discoverable", a.discoverable);
    return b.build();
}

Value buildBluetoothDevice(const BluetoothDevice& d) {
    ObjectBuilder b;
    b.set("id", d.id);
    b.set("adapterId", d.adapter_id);
    b.set("mac", d.mac);
    b.set("name", d.name);
    b.set("alias", d.alias);
    b.set("icon", d.icon);
    b.set("paired", d.paired);
    b.set("connected", d.connected);
    b.set("trusted", d.trusted);
    b.set("blocked", d.blocked);
    if (d.rssi) b.set("rssi", static_cast<double>(*d.rssi)); else b.setNull("rssi");
    return b.build();
}

Value buildBluetoothState(BluetoothService* svc) {
    ObjectBuilder b;
    if (!svc) {
        ArrayBuilder empty(0);
        b.set("adapters", empty.build());
        b.set("devices", empty.build());
        b.setNull("defaultAdapter");
        return b.build();
    }

    auto adapters = svc->adapters();
    ArrayBuilder ad(adapters.size());
    for (size_t i = 0; i < adapters.size(); ++i) {
        ev::Persistent v(buildBluetoothAdapter(adapters[i]));
        ad.set(static_cast<uint32_t>(i), v.get());
    }
    b.set("adapters", ad.build());

    auto devices = svc->devices();
    ArrayBuilder dv(devices.size());
    for (size_t i = 0; i < devices.size(); ++i) {
        ev::Persistent v(buildBluetoothDevice(devices[i]));
        dv.set(static_cast<uint32_t>(i), v.get());
    }
    b.set("devices", dv.build());

    auto def = svc->default_adapter();
    if (def) {
        ev::Persistent v(buildBluetoothAdapter(*def));
        b.set("defaultAdapter", v.get());
    } else {
        b.setNull("defaultAdapter");
    }
    return b.build();
}

} // namespace

void installBluetooth(ObjectBuilder& sys) {
    ObjectBuilder bluetooth;

    bluetooth.def("getState", 0, [](Value, std::span<const Value>) {
        return buildBluetoothState(getBluetoothService());
    });

    bluetooth.def("getAdapters", 0, [](Value, std::span<const Value>) {
        BluetoothService* svc = getBluetoothService();
        if (!svc) {
            ArrayBuilder arr(0);
            return arr.build();
        }
        auto adapters = svc->adapters();
        ArrayBuilder arr(adapters.size());
        for (size_t i = 0; i < adapters.size(); ++i) {
            ev::Persistent v(buildBluetoothAdapter(adapters[i]));
            arr.set(static_cast<uint32_t>(i), v.get());
        }
        return arr.build();
    });

    bluetooth.def("getDevices", 0, [](Value, std::span<const Value>) {
        BluetoothService* svc = getBluetoothService();
        if (!svc) {
            ArrayBuilder arr(0);
            return arr.build();
        }
        auto devices = svc->devices();
        ArrayBuilder arr(devices.size());
        for (size_t i = 0; i < devices.size(); ++i) {
            ev::Persistent v(buildBluetoothDevice(devices[i]));
            arr.set(static_cast<uint32_t>(i), v.get());
        }
        return arr.build();
    });

    bluetooth.def("getDevice", 1, [](Value, std::span<const Value> args) {
        BluetoothService* svc = getBluetoothService();
        if (!svc) return ev::null();

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string mac = strVal(arg0.get());

        auto d = svc->device(mac);
        if (d) {
            return buildBluetoothDevice(*d);
        }
        return ev::null();
    });

    bluetooth.def("setPowered", 1, [](Value, std::span<const Value> args) {
        BluetoothService* svc = getBluetoothService();
        if (!svc) return ev::throwError("BluetoothService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());

        bool powered = boolVal(arg0.get());
        std::string adapterId = strVal(arg1.get());

        Result r = svc->set_powered(powered, adapterId);
        if (!r.ok) {
            return ev::throwError("Bluetooth setPowered failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    bluetooth.def("startDiscovery", 0, [](Value, std::span<const Value> args) {
        BluetoothService* svc = getBluetoothService();
        if (!svc) return ev::throwError("BluetoothService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string adapterId = strVal(arg0.get());

        Result r = svc->start_discovery(adapterId);
        if (!r.ok) {
            return ev::throwError("Bluetooth startDiscovery failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    bluetooth.def("stopDiscovery", 0, [](Value, std::span<const Value> args) {
        BluetoothService* svc = getBluetoothService();
        if (!svc) return ev::throwError("BluetoothService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string adapterId = strVal(arg0.get());

        Result r = svc->stop_discovery(adapterId);
        if (!r.ok) {
            return ev::throwError("Bluetooth stopDiscovery failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    bluetooth.def("pair", 1, [](Value, std::span<const Value> args) {
        BluetoothService* svc = getBluetoothService();
        if (!svc) return ev::throwError("BluetoothService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string mac = strVal(arg0.get());

        Result r = svc->pair_device(mac);
        if (!r.ok) {
            return ev::throwError("Bluetooth pair failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    bluetooth.def("connect", 1, [](Value, std::span<const Value> args) {
        BluetoothService* svc = getBluetoothService();
        if (!svc) return ev::throwError("BluetoothService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string mac = strVal(arg0.get());

        Result r = svc->connect_device(mac);
        if (!r.ok) {
            return ev::throwError("Bluetooth connect failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    bluetooth.def("disconnect", 1, [](Value, std::span<const Value> args) {
        BluetoothService* svc = getBluetoothService();
        if (!svc) return ev::throwError("BluetoothService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string mac = strVal(arg0.get());

        Result r = svc->disconnect_device(mac);
        if (!r.ok) {
            return ev::throwError("Bluetooth disconnect failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    bluetooth.def("removeDevice", 1, [](Value, std::span<const Value> args) {
        BluetoothService* svc = getBluetoothService();
        if (!svc) return ev::throwError("BluetoothService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string mac = strVal(arg0.get());

        Result r = svc->remove_device(mac);
        if (!r.ok) {
            return ev::throwError("Bluetooth removeDevice failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    bluetooth.def("on", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        addEventListener("bluetooth", evName, arg1.get());
        return ev::fromBool(true);
    });

    bluetooth.def("off", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        removeEventListener("bluetooth", evName, arg1.get());
        return ev::fromBool(true);
    });

    sys.set("bluetooth", bluetooth.build());
}

void tickBluetooth() {
    BluetoothService* svc = getBluetoothService();
    if (!svc) return;

    auto events = svc->events().drain();
    for (const auto& evItem : events) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, AdapterChanged>) {
                ev::Persistent p(buildBluetoothAdapter(e.adapter));
                dispatchEvent("bluetooth", "adapterChanged", p.get());
            } else if constexpr (std::is_same_v<T, DeviceFound>) {
                ev::Persistent p(buildBluetoothDevice(e.device));
                dispatchEvent("bluetooth", "deviceFound", p.get());
            } else if constexpr (std::is_same_v<T, DeviceChanged>) {
                ev::Persistent p(buildBluetoothDevice(e.device));
                dispatchEvent("bluetooth", "deviceChanged", p.get());
            } else if constexpr (std::is_same_v<T, DeviceRemoved>) {
                ObjectBuilder b;
                b.set("mac", e.mac);
                b.set("id", e.id);
                dispatchEvent("bluetooth", "deviceRemoved", b.build());
            }
        }, evItem);
    }
}

void shutdownBluetooth() {
}

} // namespace brosys::api
