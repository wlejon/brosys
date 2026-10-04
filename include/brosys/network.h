#pragma once

#include "brosys/export.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brosys {

enum class ConnectionType {
    None = 0,
    Ethernet = 1,
    WiFi = 2,
    Cellular = 3,
    Other = 4
};

enum class ConnectionState {
    Disconnected = 0,
    Connecting = 1,
    Connected = 2
};

enum class SecurityType {
    Open = 0,
    WEP = 1,
    WPA_Personal = 2,
    WPA2_Personal = 3,
    WPA3_Personal = 4,
    WPA_Enterprise = 5,
    WPA2_Enterprise = 6,
    WPA3_Enterprise = 7,
    Unknown = 8
};

struct WiFiAccessPoint {
    std::string ssid;
    std::string bssid;
    int signal_strength_percent = 0; // 0 - 100
    int signal_strength_dbm = -100;   // e.g. -65 dBm
    SecurityType security = SecurityType::Unknown;
    int channel = 0;
    int frequency_mhz = 0;
    bool is_connected = false;

    bool operator==(const WiFiAccessPoint& other) const = default;
};

struct NetworkStatus {
    bool is_online = false;
    ConnectionType active_type = ConnectionType::None;
    ConnectionState state = ConnectionState::Disconnected;
    std::string connection_name;
    std::string ip_address;
    std::string gateway;
    std::string dns;
    std::string mac_address;

    bool operator==(const NetworkStatus& other) const = default;
};

class BROSYS_API NetworkManager {
public:
    using NetworkChangeCallback = std::function<void(const NetworkStatus&)>;
    using WiFiScanCallback = std::function<void(const std::vector<WiFiAccessPoint>&)>;

    NetworkManager();
    ~NetworkManager();

    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;
    NetworkManager(NetworkManager&&) noexcept;
    NetworkManager& operator=(NetworkManager&&) noexcept;

    [[nodiscard]] NetworkStatus get_status() const;
    [[nodiscard]] bool is_wifi_available() const;
    [[nodiscard]] std::vector<WiFiAccessPoint> scan_wifi() const;
    void scan_wifi_async(WiFiScanCallback cb);

    void register_status_callback(NetworkChangeCallback cb);

    // Mocking / simulation support
    void set_mock_status(const NetworkStatus& status);
    void set_mock_wifi_networks(const std::vector<WiFiAccessPoint>& aps);
    void clear_mock_network();
    [[nodiscard]] bool is_mocked() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace network {
    BROSYS_API NetworkStatus get_status();
    BROSYS_API bool is_wifi_available();
    BROSYS_API std::vector<WiFiAccessPoint> scan_wifi();
} // namespace network

} // namespace brosys
