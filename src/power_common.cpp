#include "brosys/power.h"
#include "power_internal.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace brosys {

struct PowerManager::Impl {
    std::unique_ptr<IPowerBackend> backend;
    std::optional<BatteryInfo> mock_battery;
    mutable std::mutex mutex;

    std::vector<BatteryCallback> callbacks;
    std::atomic<bool> monitoring{false};
    std::thread monitor_thread;

    Impl() : backend(create_platform_power_backend()) {}

    ~Impl() {
        stop_monitoring();
    }

    void stop_monitoring() {
        if (monitoring.exchange(false)) {
            if (monitor_thread.joinable()) {
                monitor_thread.join();
            }
        }
    }
};

PowerManager::PowerManager() : impl_(std::make_unique<Impl>()) {}
PowerManager::~PowerManager() = default;

PowerManager::PowerManager(PowerManager&&) noexcept = default;
PowerManager& PowerManager::operator=(PowerManager&&) noexcept = default;

BatteryInfo PowerManager::get_battery_info() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mock_battery.has_value()) {
        return *impl_->mock_battery;
    }
    if (impl_->backend) {
        return impl_->backend->get_battery_info();
    }
    return BatteryInfo{};
}

bool PowerManager::can_suspend() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend ? impl_->backend->can_suspend() : false;
}

bool PowerManager::can_hibernate() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend ? impl_->backend->can_hibernate() : false;
}

bool PowerManager::can_reboot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend ? impl_->backend->can_reboot() : false;
}

bool PowerManager::can_power_off() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend ? impl_->backend->can_power_off() : false;
}

bool PowerManager::can_lock() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend ? impl_->backend->can_lock() : false;
}

bool PowerManager::suspend(bool dry_run) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend ? impl_->backend->suspend(dry_run) : false;
}

bool PowerManager::hibernate(bool dry_run) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend ? impl_->backend->hibernate(dry_run) : false;
}

bool PowerManager::reboot(bool dry_run) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend ? impl_->backend->reboot(dry_run) : false;
}

bool PowerManager::power_off(bool dry_run) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend ? impl_->backend->power_off(dry_run) : false;
}

bool PowerManager::lock(bool dry_run) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend ? impl_->backend->lock(dry_run) : false;
}

void PowerManager::register_battery_callback(BatteryCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->callbacks.push_back(std::move(cb));
}

void PowerManager::start_monitoring(int interval_ms) {
    if (impl_->monitoring.exchange(true)) {
        return; // Already monitoring
    }

    impl_->monitor_thread = std::thread([this, interval_ms]() {
        BatteryInfo last_info{};
        bool has_last = false;

        while (impl_->monitoring.load()) {
            BatteryInfo current = get_battery_info();
            bool changed = !has_last || !(current == last_info);

            if (changed) {
                last_info = current;
                has_last = true;

                std::vector<BatteryCallback> cbs;
                {
                    std::lock_guard<std::mutex> lock(impl_->mutex);
                    cbs = impl_->callbacks;
                }
                for (auto& cb : cbs) {
                    if (cb) cb(current);
                }
            }

            int slept = 0;
            while (slept < interval_ms && impl_->monitoring.load()) {
                int slice = std::min(100, interval_ms - slept);
                std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                slept += slice;
            }
        }
    });
}

void PowerManager::stop_monitoring() {
    impl_->stop_monitoring();
}

bool PowerManager::is_monitoring() const {
    return impl_->monitoring.load();
}

void PowerManager::set_mock_battery(const BatteryInfo& info) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->mock_battery = info;
}

void PowerManager::clear_mock_battery() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->mock_battery.reset();
}

bool PowerManager::is_mocked() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->mock_battery.has_value();
}

namespace power {

static PowerManager& get_instance() {
    static PowerManager inst;
    return inst;
}

BatteryInfo get_battery_info() {
    return get_instance().get_battery_info();
}

bool can_suspend() {
    return get_instance().can_suspend();
}

bool can_hibernate() {
    return get_instance().can_hibernate();
}

bool can_reboot() {
    return get_instance().can_reboot();
}

bool can_power_off() {
    return get_instance().can_power_off();
}

bool can_lock() {
    return get_instance().can_lock();
}

bool suspend(bool dry_run) {
    return get_instance().suspend(dry_run);
}

bool hibernate(bool dry_run) {
    return get_instance().hibernate(dry_run);
}

bool reboot(bool dry_run) {
    return get_instance().reboot(dry_run);
}

bool power_off(bool dry_run) {
    return get_instance().power_off(dry_run);
}

bool lock(bool dry_run) {
    return get_instance().lock(dry_run);
}

} // namespace power

} // namespace brosys
