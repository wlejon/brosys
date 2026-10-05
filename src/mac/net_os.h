// The Objective-C-only parts of the macOS network backend, behind C++:
// CoreWLAN (wifi.mm) and Network.framework path monitoring (net_path.mm).
#pragma once

#include "brosys/network.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brosys::mac {

// One Wi-Fi interface as CoreWLAN sees it. SSID / BSSID are empty unless the
// process has Location Services authorization (macOS 14+ redacts them).
struct WifiInterfaceInfo {
    std::string name;  // BSD name, "en0"
    bool power_on = false;
    std::string ssid;
    std::string bssid;
    std::optional<int32_t> rssi_dbm;
    double tx_rate_mbps = 0;
    uint32_t channel = 0;
    uint32_t frequency_mhz = 0;
};

std::vector<WifiInterfaceInfo> wifi_interfaces();

struct WifiScanResult {
    bool ok = false;
    std::string error;
    std::vector<WifiAccessPoint> access_points;
};

// Blocking active scan (seconds) on one interface.
WifiScanResult wifi_scan(const std::string& interface);
// The results of the system's last scan, without scanning.
std::vector<WifiAccessPoint> wifi_cached(const std::string& interface);

// CoreWLAN power / SSID / BSSID / link / mode events; `fn` runs on an
// arbitrary thread until the handle is destroyed.
std::shared_ptr<void> watch_wifi(std::function<void()> fn);

// Network.framework default-path status as Connectivity, first value
// delivered before this returns (or after 2 s at most). `fn` runs on a
// private queue until the handle is destroyed.
std::shared_ptr<void> watch_path(std::function<void(Connectivity)> fn);

uint32_t wifi_frequency_mhz(uint32_t channel, int band_ghz);  // band 2, 5 or 6
std::string normalize_mac(const std::string& mac);            // "a:b:..." -> "0a:0b:..."

}  // namespace brosys::mac
