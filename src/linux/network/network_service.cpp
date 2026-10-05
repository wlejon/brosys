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
