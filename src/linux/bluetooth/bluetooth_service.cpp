// Linux BluetoothService: BlueZ 5 over the system bus.
//
// ObjectManager at / mirrors the BlueZ object tree. Adapters and devices
// are translated into immutable snapshots, and control operations (power,
// discovery, connect, disconnect, pair, remove) are routed to BlueZ methods.
#include "brosys/bluetooth.h"
#include "linux/bluetooth/bluez_model.h"
#include "linux/dbus/connection.h"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace brosys {

namespace {

using dbus::Value;
using namespace bluez;

bool mac_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

class LinuxBluetoothService final : public BluetoothService {
public:
    ~LinuxBluetoothService() override { conn_.reset(); }

    bool start(const BluetoothConfig& config, std::string* error) {
        config_ = config;
        conn_ = dbus::Connection::open(dbus::BusKind::System, config.system_bus_address,
                                       "brosys-bluetooth", error);
        if (!conn_) return false;

        bool up = conn_->run_sync([this] {
            install_watches();
            reload();
            return bluez_up_;
        });

        if (!up) {
            if (error) *error = "BlueZ is not running (org.bluez has no owner)";
            return false;
        }

        conn_->set_disconnect_handler([this] {
            std::lock_guard<std::mutex> lock(mu_);
            objects_.clear();
            adapters_.clear();
            devices_.clear();
            bluez_up_ = false;
        });

        conn_->set_reconnect_handler([this] {
            reload();
        });

        return true;
    }

    BluetoothEventQueue& events() override { return queue_; }

    std::vector<BluetoothAdapter> adapters() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return adapters_;
    }

    std::optional<BluetoothAdapter> default_adapter() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return adapters_.empty() ? std::nullopt : std::make_optional(adapters_.front());
    }

    std::vector<BluetoothDevice> devices() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return devices_;
    }

    std::optional<BluetoothDevice> device(const std::string& mac) const override {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& d : devices_) {
            if (mac_equal(d.mac, mac)) return d;
        }
        return std::nullopt;
    }

    Result set_powered(bool powered, const std::string& adapter_id) override {
        std::string path = resolve_adapter(adapter_id);
        if (path.empty()) return Result::failure("no Bluetooth adapter found");

        dbus::Reply r = conn_->call(kService, path, "org.freedesktop.DBus.Properties", "Set",
                                    {Value::str(kAdapter), Value::str("Powered"),
                                     Value::variant(Value::boolean(powered))});
        return r.ok ? Result::success() : Result::failure(r.error());
    }

    Result start_discovery(const std::string& adapter_id) override {
        std::string path = resolve_adapter(adapter_id);
        if (path.empty()) return Result::failure("no Bluetooth adapter found");

        dbus::Reply r = conn_->call(kService, path, kAdapter, "StartDiscovery");
        return r.ok ? Result::success() : Result::failure(r.error());
    }

    Result stop_discovery(const std::string& adapter_id) override {
        std::string path = resolve_adapter(adapter_id);
        if (path.empty()) return Result::failure("no Bluetooth adapter found");

        dbus::Reply r = conn_->call(kService, path, kAdapter, "StopDiscovery");
        return r.ok ? Result::success() : Result::failure(r.error());
    }

    Result connect_device(const std::string& mac) override {
        std::string path = resolve_device(mac);
        if (path.empty()) return Result::failure("Bluetooth device not found: " + mac);

        dbus::Reply r = conn_->call(kService, path, kDevice, "Connect");
        return r.ok ? Result::success() : Result::failure(r.error());
    }

    Result disconnect_device(const std::string& mac) override {
        std::string path = resolve_device(mac);
        if (path.empty()) return Result::failure("Bluetooth device not found: " + mac);

        dbus::Reply r = conn_->call(kService, path, kDevice, "Disconnect");
        return r.ok ? Result::success() : Result::failure(r.error());
    }

    Result pair_device(const std::string& mac) override {
        std::string path = resolve_device(mac);
        if (path.empty()) return Result::failure("Bluetooth device not found: " + mac);

        dbus::Reply r = conn_->call(kService, path, kDevice, "Pair");
        return r.ok ? Result::success() : Result::failure(r.error());
    }

    Result remove_device(const std::string& mac) override {
        std::string dev_path, adapter_path;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& d : devices_) {
                if (mac_equal(d.mac, mac)) {
                    dev_path = d.id;
                    adapter_path = d.adapter_id;
                    break;
                }
            }
        }
        if (dev_path.empty()) return Result::failure("Bluetooth device not found: " + mac);
        if (adapter_path.empty()) adapter_path = resolve_adapter("");
        if (adapter_path.empty()) return Result::failure("no Bluetooth adapter found for device");

        dbus::Reply r = conn_->call(kService, adapter_path, kAdapter, "RemoveDevice",
                                    {Value::obj(dev_path)});
        return r.ok ? Result::success() : Result::failure(r.error());
    }

private:
    std::string resolve_adapter(const std::string& adapter_id) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (!adapter_id.empty()) {
            for (auto& a : adapters_) {
                if (a.id == adapter_id || a.name == adapter_id || a.address == adapter_id)
                    return a.id;
            }
            return "";
        }
        return adapters_.empty() ? "" : adapters_.front().id;
    }

    std::string resolve_device(const std::string& mac) const {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& d : devices_) {
            if (mac_equal(d.mac, mac) || d.id == mac) return d.id;
        }
        return "";
    }

    void install_watches() {
        conn_->watch_name_owner(kService, [this](const std::string&, const std::string&, const std::string& new_owner) {
            if (new_owner.empty()) {
                std::lock_guard<std::mutex> lock(mu_);
                objects_.clear();
                adapters_.clear();
                devices_.clear();
                bluez_up_ = false;
            } else {
                reload();
            }
        });

        conn_->add_match(
            "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'",
            [this](const dbus::Message& m) {
                if (m.args.size() < 2) return;
                std::string path = m.args[0].as_string();
                merge_interfaces(objects_, path, m.args[1]);
                publish_changes(path, false);
            });

        conn_->add_match(
            "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved'",
            [this](const dbus::Message& m) {
                if (m.args.size() < 2) return;
                std::string path = m.args[0].as_string();
                std::string dev_mac;
                bool was_device = false;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    for (auto& d : devices_) {
                        if (d.id == path) {
                            was_device = true;
                            dev_mac = d.mac;
                            break;
                        }
                    }
                    objects_.erase(path);
                    update_snapshots();
                }
                if (was_device) {
                    queue_.push(DeviceRemoved{dev_mac, path});
                }
            });

        conn_->add_match(
            "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
            [this](const dbus::Message& m) {
                if (m.args.size() < 2) return;
                std::string iface = m.args[0].as_string();
                if (iface != kAdapter && iface != kDevice) return;
                if (apply_properties_changed(objects_, m.path, iface, m.args[1])) {
                    publish_changes(m.path, true);
                }
            });
    }

    void reload() {
        dbus::Reply r = conn_->call(kService, kRoot, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        std::lock_guard<std::mutex> lock(mu_);
        objects_.clear();
        bluez_up_ = r.ok;
        if (r.ok && !r.values.empty()) {
            merge_managed_objects(objects_, r.values[0]);
        }
        update_snapshots();
    }

    void update_snapshots() {
        // mu_ held
        adapters_ = build_adapters(objects_);
        devices_ = build_devices(objects_);
    }

    void publish_changes(const std::string& path, bool is_modify) {
        std::optional<BluetoothAdapter> adapter;
        std::optional<BluetoothDevice> device;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = objects_.find(path);
            if (it != objects_.end()) {
                auto ait = it->second.find(kAdapter);
                if (ait != it->second.end()) adapter = parse_adapter(path, ait->second);
                auto dit = it->second.find(kDevice);
                if (dit != it->second.end()) device = parse_device(path, dit->second);
            }
            update_snapshots();
        }
        if (adapter) {
            queue_.push(AdapterChanged{std::move(*adapter)});
        }
        if (device) {
            if (is_modify) {
                queue_.push(DeviceChanged{std::move(*device)});
            } else {
                queue_.push(DeviceFound{std::move(*device)});
            }
        }
    }

    BluetoothConfig config_;
    BluetoothEventQueue queue_;
    std::unique_ptr<dbus::Connection> conn_;

    mutable std::mutex mu_;
    Objects objects_;
    std::vector<BluetoothAdapter> adapters_;
    std::vector<BluetoothDevice> devices_;
    bool bluez_up_ = false;
};

}  // namespace

std::unique_ptr<BluetoothService> BluetoothService::create(const BluetoothConfig& config, std::string* error) {
    auto s = std::make_unique<LinuxBluetoothService>();
    if (!s->start(config, error)) return nullptr;
    return s;
}

}  // namespace brosys
