#include "api.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <memory>
#include <string>
#include <vector>

namespace brosys::api {

namespace {

struct PendingWifiScan {
    std::string deviceId;
    std::shared_ptr<ev::Persistent> promise;
};

std::vector<PendingWifiScan> g_pendingWifiScans;

Value buildIpConfig(const IpConfig& c) {
    ObjectBuilder b;
    ArrayBuilder addrs(c.addresses.size());
    for (size_t i = 0; i < c.addresses.size(); ++i) addrs.set(static_cast<uint32_t>(i), c.addresses[i]);
    b.set("addresses", addrs.build());

    ArrayBuilder gws(c.gateways.size());
    for (size_t i = 0; i < c.gateways.size(); ++i) gws.set(static_cast<uint32_t>(i), c.gateways[i]);
    b.set("gateways", gws.build());

    ArrayBuilder dns(c.dns.size());
    for (size_t i = 0; i < c.dns.size(); ++i) dns.set(static_cast<uint32_t>(i), c.dns[i]);
    b.set("dns", dns.build());

    return b.build();
}

Value buildNetDevice(const NetDevice& d) {
    ObjectBuilder b;
    b.set("id", d.id);
    b.set("interfaceName", d.interface_name);
    b.set("description", d.description);
    b.set("type", to_string(d.type));
    b.set("state", to_string(d.state));
    b.set("mac", d.mac);
    b.set("connection", d.connection);
    b.set("speedMbps", static_cast<double>(d.speed_mbps));

    ev::Persistent v4(buildIpConfig(d.ipv4));
    b.set("ipv4", v4.get());

    ev::Persistent v6(buildIpConfig(d.ipv6));
    b.set("ipv6", v6.get());

    b.set("isPrimary", d.is_primary);
    b.set("managed", d.managed);
    return b.build();
}

Value buildActiveConnection(const ActiveConnection& c) {
    ObjectBuilder b;
    b.set("id", c.id);
    b.set("name", c.name);
    b.set("uuid", c.uuid);
    b.set("type", to_string(c.type));
    b.set("state", to_string(c.state));

    ArrayBuilder devIds(c.device_ids.size());
    for (size_t i = 0; i < c.device_ids.size(); ++i) devIds.set(static_cast<uint32_t>(i), c.device_ids[i]);
    b.set("deviceIds", devIds.build());

    b.set("default4", c.default4);
    b.set("default6", c.default6);
    return b.build();
}

Value buildWifiAccessPoint(const WifiAccessPoint& ap) {
    ObjectBuilder b;
    b.set("deviceId", ap.device_id);
    b.set("ssid", ap.ssid);
    b.set("bssid", ap.bssid);
    b.set("strengthPercent", static_cast<double>(ap.strength_percent));
    if (ap.rssi_dbm) b.set("rssiDbm", static_cast<double>(*ap.rssi_dbm)); else b.setNull("rssiDbm");
    b.set("frequencyMhz", static_cast<double>(ap.frequency_mhz));
    b.set("channel", static_cast<double>(ap.channel));
    b.set("security", to_string(ap.security));
    b.set("active", ap.active);
    return b.build();
}

Value buildNetworkState(const NetworkState& s) {
    ObjectBuilder b;
    b.set("connectivity", to_string(s.connectivity));
    b.set("networkingEnabled", s.networking_enabled);
    b.set("wifiEnabled", s.wifi_enabled);
    b.set("wifiHardwareEnabled", s.wifi_hardware_enabled);
    b.set("primaryDevice", s.primary_device);

    ArrayBuilder devs(s.devices.size());
    for (size_t i = 0; i < s.devices.size(); ++i) {
        ev::Persistent devVal(buildNetDevice(s.devices[i]));
        devs.set(static_cast<uint32_t>(i), devVal.get());
    }
    b.set("devices", devs.build());

    ArrayBuilder conns(s.active_connections.size());
    for (size_t i = 0; i < s.active_connections.size(); ++i) {
        ev::Persistent connVal(buildActiveConnection(s.active_connections[i]));
        conns.set(static_cast<uint32_t>(i), connVal.get());
    }
    b.set("activeConnections", conns.build());
    return b.build();
}

} // namespace

void installNetwork(ObjectBuilder& sys) {
    ObjectBuilder network;

    network.def("getState", 0, [](Value, std::span<const Value>) {
        NetworkService* svc = getNetworkService();
        if (!svc) {
            NetworkState empty;
            return buildNetworkState(empty);
        }
        return buildNetworkState(svc->state());
    });

    network.def("getAccessPoints", 0, [](Value, std::span<const Value> args) {
        NetworkService* svc = getNetworkService();
        if (!svc) {
            ArrayBuilder arr(0);
            return arr.build();
        }
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string devId = strVal(arg0.get());
        auto aps = svc->access_points(devId);
        ArrayBuilder arr(aps.size());
        for (size_t i = 0; i < aps.size(); ++i) {
            ev::Persistent apVal(buildWifiAccessPoint(aps[i]));
            arr.set(static_cast<uint32_t>(i), apVal.get());
        }
        return arr.build();
    });

    network.def("scanWifi", 0, [](Value, std::span<const Value> args) {
        Value p = ev::createPromise();
        ev::Persistent pr(p);

        NetworkService* svc = getNetworkService();
        if (!svc) {
            ev::Persistent err(ev::fromUtf8("NetworkService is unavailable"));
            ev::rejectPromise(pr.get(), err.get());
            return pr.get();
        }

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string devId = strVal(arg0.get());

        Result r = svc->request_wifi_scan(devId);
        if (!r.ok) {
            ev::Persistent err(ev::fromUtf8("Wi-Fi scan failed: " + r.error));
            ev::rejectPromise(pr.get(), err.get());
            return pr.get();
        }

        g_pendingWifiScans.push_back({devId, std::make_shared<ev::Persistent>(pr.get())});
        return pr.get();
    });

    network.def("connectWifi", 2, [](Value, std::span<const Value> args) {
        Value p = ev::createPromise();
        ev::Persistent pr(p);

        NetworkService* svc = getNetworkService();
        if (!svc) {
            ev::Persistent err(ev::fromUtf8("NetworkService is unavailable"));
            ev::rejectPromise(pr.get(), err.get());
            return pr.get();
        }

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        ev::Persistent arg2(args.size() > 2 ? args[2] : ev::undefined());

        std::string ssid = strVal(arg0.get());
        std::string secret = strVal(arg1.get());

        std::string devId;
        if (ev::isString(arg2.get())) {
            devId = strVal(arg2.get());
        } else {
            auto st = svc->state();
            for (const auto& d : st.devices) {
                if (d.type == LinkType::WiFi) {
                    devId = d.id;
                    break;
                }
            }
        }

        WifiSecurity sec = WifiSecurity::Wpa2Personal;
        Result r = svc->connect_wifi(devId, ssid, secret, sec);
        if (r.ok) {
            ev::resolvePromise(pr.get(), ev::fromBool(true));
        } else {
            ev::Persistent err(ev::fromUtf8("Wi-Fi connect failed: " + r.error));
            ev::rejectPromise(pr.get(), err.get());
        }
        return pr.get();
    });

    network.def("disconnect", 1, [](Value, std::span<const Value> args) {
        NetworkService* svc = getNetworkService();
        if (!svc) return ev::throwError("NetworkService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string target = strVal(arg0.get());

        Result r = svc->disconnect(target);
        if (!r.ok) {
            return ev::throwError("Network disconnect failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    network.def("disconnectWifi", 0, [](Value, std::span<const Value>) {
        NetworkService* svc = getNetworkService();
        if (!svc) return ev::throwError("NetworkService is unavailable");

        auto st = svc->state();
        for (const auto& d : st.devices) {
            if (d.type == LinkType::WiFi && !d.connection.empty()) {
                svc->disconnect(d.id);
            }
        }
        return ev::fromBool(true);
    });

    network.def("connectVpn", 1, [](Value, std::span<const Value> args) {
        NetworkService* svc = getNetworkService();
        if (!svc) return ev::throwError("NetworkService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string vpn = strVal(arg0.get());

        Result r = svc->connect_vpn(vpn);
        if (!r.ok) {
            return ev::throwError("VPN connect failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    network.def("disconnectVpn", 1, [](Value, std::span<const Value> args) {
        NetworkService* svc = getNetworkService();
        if (!svc) return ev::throwError("NetworkService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string vpn = strVal(arg0.get());

        Result r = svc->disconnect_vpn(vpn);
        if (!r.ok) {
            return ev::throwError("VPN disconnect failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    network.def("on", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        addEventListener("network", evName, arg1.get());
        return ev::fromBool(true);
    });

    network.def("off", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        removeEventListener("network", evName, arg1.get());
        return ev::fromBool(true);
    });

    sys.set("network", network.build());
}

void tickNetwork() {
    NetworkService* svc = getNetworkService();
    if (!svc) return;

    auto events = svc->events().drain();
    for (const auto& evItem : events) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, NetworkChanged>) {
                ev::Persistent p(buildNetworkState(e.state));
                dispatchEvent("network", "changed", p.get());
                dispatchEvent("network", "networkChanged", p.get());
            } else if constexpr (std::is_same_v<T, WifiScanCompleted>) {
                ObjectBuilder b;
                b.set("deviceId", e.device_id);
                b.set("ok", e.ok);
                b.set("error", e.error);

                ArrayBuilder aps(e.access_points.size());
                for (size_t i = 0; i < e.access_points.size(); ++i) {
                    ev::Persistent apVal(buildWifiAccessPoint(e.access_points[i]));
                    aps.set(static_cast<uint32_t>(i), apVal.get());
                }
                b.set("accessPoints", aps.build());

                dispatchEvent("network", "wifiScanCompleted", b.build());

                // Resolve/reject matching pending scan promises
                std::vector<PendingWifiScan> remaining;
                for (auto& scan : g_pendingWifiScans) {
                    if (scan.deviceId.empty() || scan.deviceId == e.device_id) {
                        if (e.ok) {
                            ArrayBuilder res(e.access_points.size());
                            for (size_t i = 0; i < e.access_points.size(); ++i) {
                                ev::Persistent apVal(buildWifiAccessPoint(e.access_points[i]));
                                res.set(static_cast<uint32_t>(i), apVal.get());
                            }
                            ev::resolvePromise(scan.promise->get(), res.build());
                        } else {
                            ev::Persistent err(ev::fromUtf8(e.error.empty() ? "Wi-Fi scan failed" : e.error));
                            ev::rejectPromise(scan.promise->get(), err.get());
                        }
                    } else {
                        remaining.push_back(std::move(scan));
                    }
                }
                g_pendingWifiScans = std::move(remaining);
            }
        }, evItem);
    }
}

void shutdownNetwork() {
    for (auto& scan : g_pendingWifiScans) {
        ev::Persistent err(ev::fromUtf8("NetworkService shut down"));
        ev::rejectPromise(scan.promise->get(), err.get());
    }
    g_pendingWifiScans.clear();
}

} // namespace brosys::api
