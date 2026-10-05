// Linux NetworkService: NetworkManager over the system bus. The whole NM
// object tree is mirrored from the ObjectManager (GetManagedObjects, then
// InterfacesAdded / InterfacesRemoved / PropertiesChanged) on the
// connection's thread; changes arm a short timer and one NetworkChanged
// snapshot is built per burst. Wi-Fi scans complete when the device's
// LastScan changes.
#include "brosys/network.h"
#include "linux/dbus/connection.h"
#include "linux/network/nm_model.h"

#include <mutex>

namespace brosys {

namespace {

using dbus::Value;

constexpr std::chrono::milliseconds kCoalesce(30);

class LinuxNetworkService final : public NetworkService {
public:
    ~LinuxNetworkService() override { conn_.reset(); }  // joins the bus thread before members go

    bool start(const NetworkConfig& config, std::string* error) {
        config_ = config;
        conn_ = dbus::Connection::open(dbus::BusKind::System, config.system_bus_address, "brosys-network", error);
        if (!conn_) return false;
        bool up = conn_->run_sync([this] {
            install_watches();
            reload();
            publish(true);
            return nm_up_;
        });
        if (!up) {
            if (error) *error = "NetworkManager is not running (org.freedesktop.NetworkManager has no owner)";
            return false;
        }
        // The system bus restarting: empty while it is down (every call
        // fails), reloaded once the connection is back. NetworkManager
        // restarting on its own is the NameOwnerChanged watch.
        auto resync = [this] {
            reload();
            publish();
        };
        conn_->set_disconnect_handler(resync);
        conn_->set_reconnect_handler(resync);
        return true;
    }

    NetworkEventQueue& events() override { return queue_; }

    NetworkState state() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return state_;
    }

    std::vector<WifiAccessPoint> access_points(const std::string& device_id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<WifiAccessPoint> out;
        for (auto& [dev, aps] : aps_)
            if (device_id.empty() || dev == device_id) out.insert(out.end(), aps.begin(), aps.end());
        return out;
    }

    Result request_wifi_scan(const std::string& device_id) override {
        auto devices = conn_->run_sync([this] { return nm::wifi_devices(objects_); });
        if (!device_id.empty()) {
            bool found = false;
            for (auto& d : devices) found |= d == device_id;
            if (!found) return Result::failure("not a Wi-Fi device: " + device_id);
            devices = {device_id};
        }
        if (devices.empty()) return Result::failure("no Wi-Fi devices");

        std::string first_error;
        size_t accepted = 0;
        for (auto& dev : devices) {
            // Armed before the request so a fast LastScan change cannot slip past.
            conn_->run_sync([&] { arm_scan_timeout(dev); });
            dbus::Reply r = conn_->call(nm::kService, dev, nm::kWireless, "RequestScan",
                                        {Value::vardict({})});
            if (r.ok) {
                ++accepted;
                continue;
            }
            conn_->run_sync([&] { disarm_scan_timeout(dev); });
            if (first_error.empty()) first_error = r.error();
            if (device_id.empty()) {  // "every device": each one still reports
                WifiScanCompleted done;
                done.device_id = dev;
                done.ok = false;
                done.error = r.error();
                done.access_points = access_points(dev);
                queue_.push(std::move(done));
            }
        }
        return accepted > 0 ? Result::success() : Result::failure(first_error);
    }

    Result connect_wifi(const std::string& device_id, const std::string& ssid,
                        const std::string& passphrase, WifiSecurity security) override {
        std::string dev_path;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!device_id.empty()) {
                for (auto& d : state_.devices) {
                    if (d.id == device_id || d.interface_name == device_id) {
                        dev_path = d.id;
                        break;
                    }
                }
                if (dev_path.empty() && device_id[0] == '/') dev_path = device_id;
            } else {
                for (auto& d : state_.devices) {
                    if (d.type == LinkType::WiFi) {
                        dev_path = d.id;
                        break;
                    }
                }
            }
        }

        std::vector<std::pair<std::string, Value>> conn_props;
        conn_props.emplace_back("id", Value::str(ssid));
        conn_props.emplace_back("type", Value::str("802-11-wireless"));

        std::vector<std::pair<std::string, Value>> wifi_props;
        wifi_props.emplace_back("ssid", Value::bytes(std::vector<uint8_t>(ssid.begin(), ssid.end())));
        wifi_props.emplace_back("mode", Value::str("infrastructure"));

        std::vector<std::pair<std::string, Value>> sec_props;
        if (security != WifiSecurity::Open && security != WifiSecurity::Unknown) {
            if (security == WifiSecurity::Wep) {
                sec_props.emplace_back("key-mgmt", Value::str("none"));
                sec_props.emplace_back("wep-key0", Value::str(passphrase));
            } else if (security == WifiSecurity::Wpa3Personal) {
                sec_props.emplace_back("key-mgmt", Value::str("sae"));
                sec_props.emplace_back("psk", Value::str(passphrase));
            } else {
                sec_props.emplace_back("key-mgmt", Value::str("wpa-psk"));
                sec_props.emplace_back("psk", Value::str(passphrase));
            }
        }

        std::vector<std::pair<Value, Value>> settings;
        settings.emplace_back(Value::str("connection"), Value::vardict(conn_props));
        settings.emplace_back(Value::str("802-11-wireless"), Value::vardict(wifi_props));
        if (!sec_props.empty()) {
            settings.emplace_back(Value::str("802-11-wireless-security"), Value::vardict(sec_props));
        }

        Value conn_dict = Value::dict("s", "a{sv}", settings);
        dbus::Reply r = conn_->call(nm::kService, nm::kPath, nm::kIface, "AddAndActivateConnection",
                                    {conn_dict, Value::obj(dev_path.empty() ? "/" : dev_path), Value::obj("/")});
        return r.ok ? Result::success() : Result::failure(r.error());
    }

    Result disconnect(const std::string& connection_id_or_device_id) override {
        std::string active_path;
        std::string dev_path;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (connection_id_or_device_id.empty()) {
                if (!state_.active_connections.empty()) {
                    active_path = state_.active_connections.front().id;
                }
            } else {
                for (auto& ac : state_.active_connections) {
                    if (ac.id == connection_id_or_device_id || ac.uuid == connection_id_or_device_id ||
                        ac.name == connection_id_or_device_id) {
                        active_path = ac.id;
                        break;
                    }
                }
                if (active_path.empty()) {
                    for (auto& d : state_.devices) {
                        if (d.id == connection_id_or_device_id || d.interface_name == connection_id_or_device_id) {
                            dev_path = d.id;
                            break;
                        }
                    }
                }
            }
        }
        if (!active_path.empty()) {
            dbus::Reply r = conn_->call(nm::kService, nm::kPath, nm::kIface, "DeactivateConnection",
                                        {Value::obj(active_path)});
            return r.ok ? Result::success() : Result::failure(r.error());
        }
        if (!dev_path.empty()) {
            dbus::Reply r = conn_->call(nm::kService, dev_path, nm::kDevice, "Disconnect");
            return r.ok ? Result::success() : Result::failure(r.error());
        }
        if (connection_id_or_device_id.rfind("/org/freedesktop/NetworkManager", 0) == 0) {
            dbus::Reply r = conn_->call(nm::kService, nm::kPath, nm::kIface, "DeactivateConnection",
                                        {Value::obj(connection_id_or_device_id)});
            return r.ok ? Result::success() : Result::failure(r.error());
        }
        return Result::failure("connection or device not found: " + connection_id_or_device_id);
    }

    Result connect_vpn(const std::string& vpn_name_or_uuid) override {
        // Query configured connections from Settings
        dbus::Reply list_rep = conn_->call(nm::kService, "/org/freedesktop/NetworkManager/Settings",
                                           "org.freedesktop.NetworkManager.Settings", "ListConnections");
        if (!list_rep.ok) return Result::failure("Settings.ListConnections failed: " + list_rep.error());
        if (list_rep.values.empty()) return Result::failure("no connections found");

        std::string found_conn_path;
        for (auto& p_val : list_rep.values[0].items()) {
            std::string c_path = p_val.as_string();
            dbus::Reply set_rep = conn_->call(nm::kService, c_path,
                                              "org.freedesktop.NetworkManager.Settings.Connection", "GetSettings");
            if (!set_rep.ok || set_rep.values.empty()) continue;
            auto* conn_sec = set_rep.values[0].lookup("connection");
            if (!conn_sec) continue;
            auto* id_val = conn_sec->lookup("id");
            auto* uuid_val = conn_sec->lookup("uuid");
            auto* type_val = conn_sec->lookup("type");
            std::string id = id_val ? id_val->as_string() : "";
            std::string uuid = uuid_val ? uuid_val->as_string() : "";
            std::string type = type_val ? type_val->as_string() : "";

            if ((id == vpn_name_or_uuid || uuid == vpn_name_or_uuid) &&
                (type == "vpn" || type == "wireguard")) {
                found_conn_path = c_path;
                break;
            }
        }

        if (found_conn_path.empty()) {
            return Result::failure("VPN connection not found: " + vpn_name_or_uuid);
        }

        dbus::Reply r = conn_->call(nm::kService, nm::kPath, nm::kIface, "ActivateConnection",
                                    {Value::obj(found_conn_path), Value::obj("/"), Value::obj("/")});
        return r.ok ? Result::success() : Result::failure(r.error());
    }

    Result disconnect_vpn(const std::string& vpn_name_or_uuid) override {
        std::string active_path;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& ac : state_.active_connections) {
                if (ac.type == LinkType::Vpn &&
                    (ac.name == vpn_name_or_uuid || ac.uuid == vpn_name_or_uuid || ac.id == vpn_name_or_uuid)) {
                    active_path = ac.id;
                    break;
                }
            }
        }
        if (active_path.empty()) {
            return Result::failure("active VPN connection not found: " + vpn_name_or_uuid);
        }
        dbus::Reply r = conn_->call(nm::kService, nm::kPath, nm::kIface, "DeactivateConnection",
                                    {Value::obj(active_path)});
        return r.ok ? Result::success() : Result::failure(r.error());
    }

private:
    // ------------------------------------------------------------ bus thread

    void install_watches() {
        conn_->add_match(
            "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
            "path_namespace='/org/freedesktop/NetworkManager'",
            [this](const dbus::Message& m) {
                if (m.args.size() < 2) return;
                bool changed = nm::apply_properties_changed(objects_, m.path, m.args[0].as_string(), m.args[1]);
                if (m.args.size() >= 3 && !m.args[2].items().empty()) {
                    refetch(m.path, m.args[0].as_string());
                    changed = true;
                }
                if (changed) schedule_publish();
            });
        conn_->add_match(
            "type='signal',interface='org.freedesktop.DBus.ObjectManager',path='/org/freedesktop'",
            [this](const dbus::Message& m) {
                if (m.args.size() < 2) return;
                std::string path = m.args[0].as_string();
                if (m.member == "InterfacesAdded") {
                    nm::merge_interfaces(objects_, path, m.args[1]);
                } else if (m.member == "InterfacesRemoved") {
                    auto it = objects_.find(path);
                    if (it == objects_.end()) return;
                    for (auto& iface : m.args[1].as_strings()) it->second.erase(iface);
                    if (it->second.empty()) objects_.erase(it);
                } else {
                    return;
                }
                schedule_publish();
            });
        conn_->watch_name_owner(nm::kService, [this](const std::string&, const std::string&, const std::string&) {
            reload();
            publish();
        });
    }

    void reload() {
        objects_.clear();
        dbus::Reply r = conn_->call(nm::kService, nm::kRoot, "org.freedesktop.DBus.ObjectManager",
                                    "GetManagedObjects");
        nm_up_ = r.ok;
        if (!r.ok || !r.first()) return;
        nm::merge_managed_objects(objects_, *r.first());
        if (objects_.find(nm::kPath) == objects_.end()) {  // older NM: the manager is not a managed object
            std::string err;
            auto props = conn_->get_all_properties(nm::kService, nm::kPath, nm::kIface, &err);
            if (err.empty()) objects_[nm::kPath][nm::kIface] = std::move(props);
        }
    }

    // Invalidated properties: fetch the interface again.
    void refetch(const std::string& path, const std::string& iface) {
        conn_->get_all_properties_async(nm::kService, path, iface,
                                        [this, path, iface](bool ok, std::map<std::string, Value> props, std::string) {
                                            if (!ok) return;
                                            objects_[path][iface] = std::move(props);
                                            schedule_publish();
                                        });
    }

    void schedule_publish() {
        if (publish_scheduled_) return;
        publish_scheduled_ = true;
        conn_->add_timer(kCoalesce, [this] { publish(); });
    }

    void publish(bool initial = false) {
        publish_scheduled_ = false;
        NetworkState s = nm::build_state(objects_);
        std::map<std::string, std::vector<WifiAccessPoint>> aps;
        std::map<std::string, int64_t> scans;
        for (auto& dev : nm::wifi_devices(objects_)) {
            aps[dev] = nm::access_points(objects_, dev);
            scans[dev] = nm::last_scan(objects_, dev).value_or(-1);
        }
        bool push_state;
        {
            std::lock_guard<std::mutex> lock(mu_);
            push_state = initial || !(s == state_);
            state_ = s;
            aps_ = aps;
        }
        if (push_state) queue_.push(NetworkChanged{std::move(s)});

        for (auto& [dev, when] : scans) {
            auto prev = last_scans_.find(dev);
            bool scanned = prev != last_scans_.end() && prev->second != when && when >= 0;
            last_scans_[dev] = when;
            if (!scanned) continue;
            disarm_scan_timeout(dev);
            WifiScanCompleted done;
            done.device_id = dev;
            done.access_points = aps[dev];
            queue_.push(std::move(done));
        }
        for (auto it = last_scans_.begin(); it != last_scans_.end();)
            it = scans.count(it->first) ? std::next(it) : last_scans_.erase(it);
    }

    void arm_scan_timeout(const std::string& dev) {
        disarm_scan_timeout(dev);
        pending_scans_[dev] = conn_->add_timer(std::chrono::milliseconds(config_.scan_timeout_ms), [this, dev] {
            pending_scans_.erase(dev);
            WifiScanCompleted done;
            done.device_id = dev;
            done.ok = false;
            done.error = "the scan did not complete within " + std::to_string(config_.scan_timeout_ms) + " ms";
            done.access_points = nm::access_points(objects_, dev);
            queue_.push(std::move(done));
        });
    }

    void disarm_scan_timeout(const std::string& dev) {
        auto it = pending_scans_.find(dev);
        if (it == pending_scans_.end()) return;
        conn_->cancel_timer(it->second);
        pending_scans_.erase(it);
    }

    NetworkConfig config_;
    NetworkEventQueue queue_;
    mutable std::mutex mu_;
    NetworkState state_;                                           // guarded by mu_
    std::map<std::string, std::vector<WifiAccessPoint>> aps_;      // guarded by mu_

    // Bus-thread state.
    bool nm_up_ = false;
    nm::Objects objects_;
    std::map<std::string, int64_t> last_scans_;
    std::map<std::string, uint64_t> pending_scans_;  // device -> timeout timer
    bool publish_scheduled_ = false;

    std::unique_ptr<dbus::Connection> conn_;  // last: destroyed (thread joined) first
};

}  // namespace

std::unique_ptr<NetworkService> NetworkService::create(const NetworkConfig& config, std::string* error) {
    auto s = std::make_unique<LinuxNetworkService>();
    if (!s->start(config, error)) return nullptr;
    return s;
}

}  // namespace brosys
