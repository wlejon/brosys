#include "brosys/tray.h"
#include "tray_internal.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace brosys {

struct TrayHost::Impl {
    std::unique_ptr<ITrayBackend> backend;
    std::atomic<bool> running{false};

    std::unordered_map<std::string, StatusNotifierItem> items;
    mutable std::mutex mutex;

    ItemAddedCallback added_cb;
    ItemUpdatedCallback updated_cb;
    ItemRemovedCallback removed_cb;
    ItemActivatedCallback activated_cb;
    MenuActionCallback menu_cb;

    Impl() : backend(create_platform_tray_backend()) {}

    ~Impl() {
        stop();
    }

    void stop() {
        if (backend && running.load()) {
            backend->stop();
        }
        running.store(false);
    }
};

TrayHost::TrayHost() : impl_(std::make_unique<Impl>()) {}
TrayHost::~TrayHost() = default;

TrayHost::TrayHost(TrayHost&&) noexcept = default;
TrayHost& TrayHost::operator=(TrayHost&&) noexcept = default;

bool TrayHost::start() {
    if (impl_->running.exchange(true)) {
        return true;
    }
    if (impl_->backend) {
        impl_->backend->start(this);
    }
    return true;
}

void TrayHost::stop() {
    impl_->stop();
}

bool TrayHost::is_running() const {
    return impl_->running.load();
}

std::vector<StatusNotifierItem> TrayHost::get_items() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<StatusNotifierItem> result;
    result.reserve(impl_->items.size());
    for (const auto& [_, item] : impl_->items) {
        result.push_back(item);
    }
    return result;
}

std::optional<StatusNotifierItem> TrayHost::get_item(const std::string& id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->items.find(id);
    if (it != impl_->items.end()) {
        return it->second;
    }
    return std::nullopt;
}

size_t TrayHost::item_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->items.size();
}

bool TrayHost::register_item(const StatusNotifierItem& item) {
    if (item.id.empty()) return false;

    ItemAddedCallback cb;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->items[item.id] = item;
        cb = impl_->added_cb;
    }

    if (impl_->backend && impl_->running.load()) {
        impl_->backend->on_item_registered(item);
    }

    if (cb) {
        cb(item);
    }
    return true;
}

bool TrayHost::update_item(const StatusNotifierItem& item) {
    if (item.id.empty()) return false;

    ItemUpdatedCallback cb;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->items.find(item.id);
        if (it != impl_->items.end()) {
            it->second = item;
            found = true;
            cb = impl_->updated_cb;
        }
    }

    if (found && cb) {
        cb(item);
    }
    return found;
}

bool TrayHost::unregister_item(const std::string& id) {
    if (id.empty()) return false;

    ItemRemovedCallback cb;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->items.find(id);
        if (it != impl_->items.end()) {
            impl_->items.erase(it);
            found = true;
            cb = impl_->removed_cb;
        }
    }

    if (found) {
        if (impl_->backend && impl_->running.load()) {
            impl_->backend->on_item_unregistered(id);
        }
        if (cb) {
            cb(id);
        }
        return true;
    }
    return false;
}

void TrayHost::clear() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& [id, _] : impl_->items) {
            ids.push_back(id);
        }
    }
    for (const auto& id : ids) {
        unregister_item(id);
    }
}

bool TrayHost::activate_item(const std::string& id, int x, int y) {
    ItemActivatedCallback cb;
    bool exists = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        exists = (impl_->items.find(id) != impl_->items.end());
        cb = impl_->activated_cb;
    }

    if (exists) {
        if (impl_->backend && impl_->running.load()) {
            impl_->backend->activate_item(id, x, y);
        }
        if (cb) {
            cb(id, x, y);
        }
        return true;
    }
    return false;
}

bool TrayHost::secondary_activate_item(const std::string& id, int x, int y) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->items.find(id) == impl_->items.end()) {
        return false;
    }
    if (impl_->backend && impl_->running.load()) {
        return impl_->backend->secondary_activate_item(id, x, y);
    }
    return true;
}

bool TrayHost::context_menu(const std::string& id, int x, int y) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->items.find(id) == impl_->items.end()) {
        return false;
    }
    if (impl_->backend && impl_->running.load()) {
        return impl_->backend->context_menu(id, x, y);
    }
    return true;
}

bool TrayHost::trigger_menu_action(const std::string& id, int action_id) {
    MenuActionCallback cb;
    bool exists = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        exists = (impl_->items.find(id) != impl_->items.end());
        cb = impl_->menu_cb;
    }

    if (exists && cb) {
        cb(id, action_id);
        return true;
    }
    return exists;
}

void TrayHost::set_item_added_callback(ItemAddedCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->added_cb = std::move(cb);
}

void TrayHost::set_item_updated_callback(ItemUpdatedCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->updated_cb = std::move(cb);
}

void TrayHost::set_item_removed_callback(ItemRemovedCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->removed_cb = std::move(cb);
}

void TrayHost::set_item_activated_callback(ItemActivatedCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->activated_cb = std::move(cb);
}

void TrayHost::set_menu_action_callback(MenuActionCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->menu_cb = std::move(cb);
}

} // namespace brosys
