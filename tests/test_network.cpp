#include "brosys/network.h"
#include "test_common.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    init_test();
    std::cout << "[test_network] Testing network subsystem...\n";

    // 1. Live system query
    auto status = brosys::network::get_status();
    std::cout << "  Live Online: " << (status.is_online ? "Yes" : "No") << "\n";
    std::cout << "  Connection Type: " << static_cast<int>(status.active_type) << "\n";
    std::cout << "  Interface Name: " << status.connection_name << "\n";
    std::cout << "  IP Address: " << status.ip_address << "\n";
    std::cout << "  MAC Address: " << status.mac_address << "\n";

    bool wifi_avail = brosys::network::is_wifi_available();
    std::cout << "  Wi-Fi Available: " << (wifi_avail ? "Yes" : "No") << "\n";

    if (wifi_avail) {
        auto aps = brosys::network::scan_wifi();
        std::cout << "  Wi-Fi Access Points found: " << aps.size() << "\n";
        for (const auto& ap : aps) {
            std::cout << "    - SSID: " << ap.ssid
                      << ", Signal: " << ap.signal_strength_percent << "% (" << ap.signal_strength_dbm << " dBm)"
                      << ", Security: " << static_cast<int>(ap.security) << "\n";
        }
    }

    // 2. Test NetworkManager with Mocking
    brosys::NetworkManager mgr;
    assert(!mgr.is_mocked());

    brosys::NetworkStatus mock_st;
    mock_st.is_online = true;
    mock_st.active_type = brosys::ConnectionType::WiFi;
    mock_st.state = brosys::ConnectionState::Connected;
    mock_st.connection_name = "Office-5GHz";
    mock_st.ip_address = "192.168.1.150";
    mock_st.gateway = "192.168.1.1";
    mock_st.dns = "1.1.1.1";
    mock_st.mac_address = "aa:bb:cc:dd:ee:ff";

    std::vector<brosys::WiFiAccessPoint> mock_aps;
    brosys::WiFiAccessPoint ap1;
    ap1.ssid = "Office-5GHz";
    ap1.bssid = "00:11:22:33:44:55";
    ap1.signal_strength_percent = 92;
    ap1.signal_strength_dbm = -54;
    ap1.security = brosys::SecurityType::WPA3_Personal;
    ap1.is_connected = true;
    mock_aps.push_back(ap1);

    brosys::WiFiAccessPoint ap2;
    ap2.ssid = "Guest-WiFi";
    ap2.bssid = "00:11:22:33:44:66";
    ap2.signal_strength_percent = 60;
    ap2.signal_strength_dbm = -70;
    ap2.security = brosys::SecurityType::Open;
    ap2.is_connected = false;
    mock_aps.push_back(ap2);

    mgr.set_mock_status(mock_st);
    mgr.set_mock_wifi_networks(mock_aps);
    assert(mgr.is_mocked());

    // Verify mock queries
    auto q_st = mgr.get_status();
    assert(q_st.is_online == true);
    assert(q_st.active_type == brosys::ConnectionType::WiFi);
    assert(q_st.ip_address == "192.168.1.150");
    assert(q_st.mac_address == "aa:bb:cc:dd:ee:ff");

    assert(mgr.is_wifi_available());
    auto q_aps = mgr.scan_wifi();
    assert(q_aps.size() == 2);
    assert(q_aps[0].ssid == "Office-5GHz");
    assert(q_aps[0].is_connected == true);
    assert(q_aps[1].security == brosys::SecurityType::Open);

    // 3. Test async scan
    std::atomic<bool> async_done = false;
    mgr.scan_wifi_async([&](const std::vector<brosys::WiFiAccessPoint>& results) {
        assert(results.size() == 2);
        async_done.store(true);
    });

    for (int i = 0; i < 50; ++i) {
        if (async_done.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(async_done.load());

    mgr.clear_mock_network();
    assert(!mgr.is_mocked());

    std::cout << "[test_network] PASSED\n";
    return 0;
}
