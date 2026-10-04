#pragma once

#include "brosys/network.h"
#include <memory>

namespace brosys {

class INetworkBackend {
public:
    virtual ~INetworkBackend() = default;

    virtual NetworkStatus get_status() = 0;
    virtual bool is_wifi_available() = 0;
    virtual std::vector<WiFiAccessPoint> scan_wifi() = 0;
    virtual void register_status_callback(NetworkManager::NetworkChangeCallback cb) = 0;
};

std::unique_ptr<INetworkBackend> create_platform_network_backend();

} // namespace brosys
