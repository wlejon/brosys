#include "brosys/sys.h"

#include <mutex>

namespace brosys {

struct SystemServices::Impl {
    PowerManager power_mgr;
    AudioManager audio_mgr;
    NetworkManager network_mgr;
    NotificationServer notification_srv;
    TrayHost tray_host;
    bool initialized = false;
    mutable std::mutex mutex;
};

SystemServices::SystemServices() : impl_(std::make_unique<Impl>()) {}
SystemServices::~SystemServices() {
    shutdown();
}

SystemServices::SystemServices(SystemServices&&) noexcept = default;
SystemServices& SystemServices::operator=(SystemServices&&) noexcept = default;

SystemServices& SystemServices::instance() {
    static SystemServices inst;
    return inst;
}

bool SystemServices::initialize() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->initialized) return true;

    impl_->notification_srv.start();
    impl_->tray_host.start();
    impl_->initialized = true;
    return true;
}

void SystemServices::shutdown() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) return;

    impl_->notification_srv.stop();
    impl_->tray_host.stop();
    impl_->power_mgr.stop_monitoring();
    impl_->initialized = false;
}

bool SystemServices::is_initialized() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->initialized;
}

PowerManager& SystemServices::power() {
    return impl_->power_mgr;
}

AudioManager& SystemServices::audio() {
    return impl_->audio_mgr;
}

NetworkManager& SystemServices::network() {
    return impl_->network_mgr;
}

NotificationServer& SystemServices::notifications() {
    return impl_->notification_srv;
}

TrayHost& SystemServices::tray() {
    return impl_->tray_host;
}

const PowerManager& SystemServices::power() const {
    return impl_->power_mgr;
}

const AudioManager& SystemServices::audio() const {
    return impl_->audio_mgr;
}

const NetworkManager& SystemServices::network() const {
    return impl_->network_mgr;
}

const NotificationServer& SystemServices::notifications() const {
    return impl_->notification_srv;
}

const TrayHost& SystemServices::tray() const {
    return impl_->tray_host;
}

} // namespace brosys
