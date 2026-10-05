// Network: connectivity, devices with their IP configuration, which device
// carries the default route (the primary), active connections, and Wi-Fi
// access points with scans that report completion.
//
// Linux: NetworkManager over the system bus (Connectivity, PrimaryConnection,
// ActiveConnections, Devices, Ip4/Ip6Config, wireless AccessPoints,
// RequestScan + LastScan).
// Windows: IP Helper (adapters, route table: the primary is the interface of
// the best default route), NotifyIpInterfaceChange / NotifyRouteChange2 /
// NotifyUnicastIpAddressChange / NotifyNetworkConnectivityHintChange, and
// the WLAN API (BSS list for BSSID / channel / RSSI; scan completion via
// WlanRegisterNotification).
// macOS: SystemConfiguration (the current set's services, SCDynamicStore
// State:/Network keys, the primary service) + getifaddrs, Network.framework's
// default path for connectivity (satisfied = Full: it does not probe for
// captive portals, so Portal / Limited are never reported), and CoreWLAN.
// Device ids are BSD names ("en0"); active connections are network services
// (id and uuid = service ID); `managed` = an enabled, visible service. SSID
// and BSSID are empty unless the process has Location Services permission
// (macOS 14+), and a scan request fails while the radio is off.
#pragma once

#include "brosys/common.h"
#include "brosys/event_queue.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace brosys {

enum class Connectivity {
    Unknown,
    None,     // no network
    Portal,   // behind a captive portal
    Limited,  // network but no internet
    Full,
};

enum class LinkType { Unknown, Ethernet, WiFi, Cellular, Loopback, Bridge, Vpn, Tunnel, Virtual, Other };

enum class LinkState {
    Unknown,
    Unavailable,   // no carrier / radio off / unmanaged
    Disconnected,  // available, not connected
    Connecting,    // link up, configuring (DHCP, auth)
    Connected,
    Disconnecting,
};

struct IpConfig {
    std::vector<std::string> addresses;  // CIDR: "192.168.1.5/24", "fe80::1/64"
    std::vector<std::string> gateways;
    std::vector<std::string> dns;

    bool operator==(const IpConfig&) const = default;
};

struct NetDevice {
    std::string id;              // NM device path; Windows interface LUID (decimal)
    std::string interface_name;  // "enp7s0", "wlan0"; Windows alias "Ethernet 2"
    std::string description;     // driver / adapter description
    LinkType type = LinkType::Unknown;
    LinkState state = LinkState::Unknown;
    std::string mac;             // "aa:bb:cc:dd:ee:ff", "" when none
    std::string connection;      // active connection / profile name ("" when none)
    uint64_t speed_mbps = 0;     // 0 unknown
    IpConfig ipv4;
    IpConfig ipv6;
    bool is_primary = false;     // carries the default route used for internet traffic
    bool managed = true;         // false: NM-unmanaged / externally configured

    bool operator==(const NetDevice&) const = default;
};

struct ActiveConnection {
    std::string id;     // NM active-connection path; Windows: interface LUID
    std::string name;   // profile name ("Wired connection 1", SSID, VPN name)
    std::string uuid;   // NM connection uuid ("" on Windows)
    LinkType type = LinkType::Unknown;
    LinkState state = LinkState::Unknown;
    std::vector<std::string> device_ids;
    bool default4 = false;
    bool default6 = false;

    bool operator==(const ActiveConnection&) const = default;
};

enum class WifiSecurity { Unknown, Open, Wep, WpaPersonal, Wpa2Personal, Wpa3Personal, WpaEnterprise, Wpa2Enterprise, Wpa3Enterprise, Owe };

struct WifiAccessPoint {
    std::string device_id;  // the Wi-Fi device that sees it
    std::string ssid;       // raw SSID bytes (usually UTF-8); "" for hidden
    std::string bssid;      // "aa:bb:cc:dd:ee:ff"
    uint8_t strength_percent = 0;
    std::optional<int32_t> rssi_dbm;
    uint32_t frequency_mhz = 0;
    uint32_t channel = 0;
    WifiSecurity security = WifiSecurity::Unknown;
    bool active = false;    // the device is associated with this BSS

    bool operator==(const WifiAccessPoint&) const = default;
};

struct NetworkState {
    Connectivity connectivity = Connectivity::Unknown;
    bool networking_enabled = true;
    bool wifi_enabled = false;           // software switch
    bool wifi_hardware_enabled = false;  // rfkill / hardware switch
    std::string primary_device;          // NetDevice::id or ""
    std::vector<NetDevice> devices;
    std::vector<ActiveConnection> active_connections;

    const NetDevice* primary() const {
        for (auto& d : devices)
            if (d.id == primary_device) return &d;
        return nullptr;
    }
    bool operator==(const NetworkState&) const = default;
};

// ---------------------------------------------------------------- events

// New snapshot after anything in NetworkState changed. Pushed once at start.
struct NetworkChanged {
    NetworkState state;
};

// A scan requested with request_wifi_scan() finished (or the OS finished a
// scan of its own); `access_points` is the device's full current list.
struct WifiScanCompleted {
    std::string device_id;
    bool ok = true;
    std::string error;
    std::vector<WifiAccessPoint> access_points;
};

using NetworkEvent = std::variant<NetworkChanged, WifiScanCompleted>;
using NetworkEventQueue = MessageQueue<NetworkEvent>;

struct NetworkConfig {
    // Linux: system-bus address override; empty = the default system bus.
    std::string system_bus_address;
    uint32_t scan_timeout_ms = 15000;  // a scan that has not completed by then reports ok=false
};

class NetworkService {
public:
    static std::unique_ptr<NetworkService> create(const NetworkConfig& config, std::string* error);
    virtual ~NetworkService() = default;

    virtual NetworkEventQueue& events() = 0;
    virtual NetworkState state() const = 0;

    // The device's last known access points ("" = all Wi-Fi devices).
    virtual std::vector<WifiAccessPoint> access_points(const std::string& device_id) const = 0;

    // Starts a scan ("" = every Wi-Fi device). Returns once the request is
    // accepted; each device reports a WifiScanCompleted when the scan ends.
    virtual Result request_wifi_scan(const std::string& device_id) = 0;

    // Connects to a Wi-Fi network (creates and activates or activates connection).
    virtual Result connect_wifi(const std::string& device_id, const std::string& ssid,
                                const std::string& passphrase, WifiSecurity security) = 0;

    // Disconnects an active connection (by path, uuid, or name) or device (by path or interface name).
    virtual Result disconnect(const std::string& connection_id_or_device_id) = 0;

    // Activates a VPN connection (by profile name or UUID).
    virtual Result connect_vpn(const std::string& vpn_name_or_uuid) = 0;

    // Deactivates a VPN connection (by profile name, UUID, or active connection path).
    virtual Result disconnect_vpn(const std::string& vpn_name_or_uuid) = 0;
};

const char* to_string(Connectivity c);
const char* to_string(LinkType t);
const char* to_string(LinkState s);
const char* to_string(WifiSecurity s);

}  // namespace brosys
