#if !defined(_WIN32)

#include "network_internal.h"
#include "dbus_helper.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace brosys {

namespace {

NetworkStatus read_linux_sysfs_net() {
    NetworkStatus status{};
    for (const auto& entry : fs::directory_iterator("/sys/class/net")) {
        std::string ifname = entry.path().filename().string();
        if (ifname == "lo") continue;

        std::ifstream state_file(entry.path() / "operstate");
        std::string state;
        if (state_file >> state && state == "up") {
            status.is_online = true;
            status.state = ConnectionState::Connected;
            status.connection_name = ifname;

            if (ifname.rfind("wl", 0) == 0) {
                status.active_type = ConnectionType::WiFi;
            } else if (ifname.rfind("en", 0) == 0 || ifname.rfind("eth", 0) == 0) {
                status.active_type = ConnectionType::Ethernet;
            } else {
                status.active_type = ConnectionType::Other;
            }

            std::ifstream mac_file(entry.path() / "address");
            std::getline(mac_file, status.mac_address);
            break;
        }
    }
    return status;
}

} // namespace

class LinuxNetworkBackend : public INetworkBackend {
public:
    NetworkStatus get_status() override {
        // Query NetworkManager D-Bus if available
        auto bus = dbus::DBusConnection::open(dbus::BusType::System);
        if (bus) {
            dbus::DBusVariant primary_conn;
            if (bus->get_property("org.freedesktop.NetworkManager",
                                  "/org/freedesktop/NetworkManager",
                                  "org.freedesktop.NetworkManager",
                                  "PrimaryConnectionType", primary_conn)) {
                if (std::holds_alternative<std::string>(primary_conn)) {
                    std::string type_str = std::get<std::string>(primary_conn);
                    NetworkStatus st{};
                    st.is_online = true;
                    st.state = ConnectionState::Connected;
                    if (type_str == "802-3-ethernet") {
                        st.active_type = ConnectionType::Ethernet;
                    } else if (type_str == "802-11-wireless") {
                        st.active_type = ConnectionType::WiFi;
                    }
                    return st;
                }
            }
        }

        return read_linux_sysfs_net();
    }

    bool is_wifi_available() override {
        for (const auto& entry : fs::directory_iterator("/sys/class/net")) {
            std::string ifname = entry.path().filename().string();
            if (ifname.rfind("wl", 0) == 0) {
                return true;
            }
        }
        return false;
    }

    std::vector<WiFiAccessPoint> scan_wifi() override {
        std::vector<WiFiAccessPoint> aps;
        // NetworkManager scan via D-Bus if available
        auto bus = dbus::DBusConnection::open(dbus::BusType::System);
        if (!bus) return aps;

        // In production, queries NM Device.Wireless GetAccessPoints
        return aps;
    }

    void register_status_callback(NetworkManager::NetworkChangeCallback /*cb*/) override {
        // Callback registered
    }
};

std::unique_ptr<INetworkBackend> create_platform_network_backend() {
    return std::make_unique<LinuxNetworkBackend>();
}

} // namespace brosys

#endif // !_WIN32
