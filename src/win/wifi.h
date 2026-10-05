// The WLAN API for the Windows network service: Wi-Fi interfaces, radio
// state, the current association, BSS lists (BSSID, RSSI, channel,
// security from the beacon IEs) and scans with completion notifications.
#pragma once

#include "brosys/network.h"

#include <windows.h>
#include <wlanapi.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace brosys::win {

struct WifiInterface {
    GUID guid{};
    std::string device_id;    // interface LUID, decimal (matches NetDevice::id)
    std::string description;
    bool software_on = false;
    bool hardware_on = false;
    bool connected = false;
    std::string profile;      // connection profile name
    std::string ssid;
    std::string bssid;        // "aa:bb:..", "" when not associated
};

class WifiClient {
public:
    // kind: 0 = state changed (connect/disconnect/radio), 1 = scan complete, 2 = scan failed.
    using Notify = std::function<void(const GUID& iface, int kind, DWORD reason)>;

    WifiClient() = default;
    ~WifiClient();
    WifiClient(const WifiClient&) = delete;
    WifiClient& operator=(const WifiClient&) = delete;

    // false (+ *error) when the WLAN service is not running / no WLAN API.
    bool open(Notify notify, std::string* error);
    void close();
    bool is_open() const { return handle_ != nullptr; }

    std::vector<WifiInterface> interfaces();
    std::vector<WifiAccessPoint> access_points(const WifiInterface& iface);
    DWORD scan(const GUID& iface);

private:
    static void WINAPI on_notification(PWLAN_NOTIFICATION_DATA data, PVOID context);
    HANDLE handle_ = nullptr;
    Notify notify_;
};

// Exposed for tests: security from the IEs of one BSS (+ the privacy bit).
WifiSecurity security_from_ies(const uint8_t* ies, size_t size, bool privacy);
uint32_t channel_from_mhz(uint32_t mhz);
std::string format_mac(const uint8_t* mac, size_t len);

}  // namespace brosys::win
