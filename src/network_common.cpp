#include "brosys/network.h"
#include "network_internal.h"

#include <future>
#include <mutex>
#include <vector>

namespace brosys {

struct NetworkManager::Impl {
    std::unique_ptr<INetworkBackend> backend;
    bool mocked = false;
    NetworkStatus mock_status;
    std::vector<WiFiAccessPoint> mock_aps;

    std::vector<NetworkChangeCallback> callbacks;
    mutable std::mutex mutex;

    Impl() : backend(create_platform_network_backend()) {}
};

NetworkManager::NetworkManager() : impl_(std::make_unique<Impl>()) {}
NetworkManager::~NetworkManager() = default;

NetworkManager::NetworkManager(NetworkManager&&) noexcept = default;
NetworkManager& NetworkManager::operator=(NetworkManager&&) noexcept = default;

NetworkStatus NetworkManager::get_status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        return impl_->mock_status;
    }
    return impl_->backend ? impl_->backend->get_status() : NetworkStatus{};
}

bool NetworkManager::is_wifi_available() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        return !impl_->mock_aps.empty() || impl_->mock_status.active_type == ConnectionType::WiFi;
    }
    return impl_->backend ? impl_->backend->is_wifi_available() : false;
}

std::vector<WiFiAccessPoint> NetworkManager::scan_wifi() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        return impl_->mock_aps;
    }
    return impl_->backend ? impl_->backend->scan_wifi() : std::vector<WiFiAccessPoint>{};
}

void NetworkManager::scan_wifi_async(WiFiScanCallback cb) {
    std::thread([this, callback = std::move(cb)]() {
        auto aps = scan_wifi();
        if (callback) {
            callback(aps);
        }
    }).detach();
}

void NetworkManager::register_status_callback(NetworkChangeCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->callbacks.push_back(cb);
    if (impl_->backend) {
        impl_->backend->register_status_callback(std::move(cb));
    }
}

void NetworkManager::set_mock_status(const NetworkStatus& status) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->mocked = true;
    impl_->mock_status = status;
}

void NetworkManager::set_mock_wifi_networks(const std::vector<WiFiAccessPoint>& aps) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->mocked = true;
    impl_->mock_aps = aps;
}

void NetworkManager::clear_mock_network() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->mocked = false;
    impl_->mock_status = NetworkStatus{};
    impl_->mock_aps.clear();
}

bool NetworkManager::is_mocked() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->mocked;
}

namespace network {

static NetworkManager& get_instance() {
    static NetworkManager inst;
    return inst;
}

NetworkStatus get_status() {
    return get_instance().get_status();
}

bool is_wifi_available() {
    return get_instance().is_wifi_available();
}

std::vector<WiFiAccessPoint> scan_wifi() {
    return get_instance().scan_wifi();
}

} // namespace network

} // namespace brosys
