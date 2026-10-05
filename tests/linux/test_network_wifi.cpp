// Opt-in: a real Wi-Fi scan through NetworkManager on a mac80211_hwsim radio.
//
// The test only scans; preparing the radios changes the machine, so it is
// done by hand and the test skips unless told where to look:
//
//   sudo modprobe mac80211_hwsim radios=2          # two virtual radios, e.g. wlan0 / wlan1
//   sudo nmcli device set wlan1 managed no         # the AP radio is not NM's (runtime only)
//   sleep 2                                        # NM resets the MAC while unmanaging; hostapd after that
//   sudo hostapd -B -P /tmp/brosys-hostapd.pid -i wlan1 tests/linux/wifi/hostapd-brosys.conf
//   sudo BROSYS_TEST_WIFI_DEVICE=wlan0 BROSYS_TEST_WIFI_SSID=brosys-test-ap
//     BROSYS_TEST_WIFI_BSSID=$(cat /sys/class/net/wlan1/address) ./test_network_wifi   (one line)
//   sudo kill $(cat /tmp/brosys-hostapd.pid); sudo modprobe -r mac80211_hwsim cfg80211
//
// (sudo: polkit refuses RequestScan to sessions that are not active and
// local, such as ssh; root is allowed.)
//
// NetworkManager never connects anything (no Wi-Fi profile exists); the
// scanning radio only scans. Checked: the scan completes (LastScan moves),
// the AP is listed with the right SSID / BSSID / channel / frequency /
// security, and the list matches `nmcli dev wifi list`.
#include "brosys/network.h"
#include "check.h"
#include "linux/fakes/event_log.h"
#include "linux/support/proc.h"

#include <cstdlib>
#include <sstream>

using namespace std::chrono_literals;

namespace {

constexpr const char* kName = "test_network_wifi";

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void run_test() {
    std::string ifname = env_or_empty("BROSYS_TEST_WIFI_DEVICE");
    std::string ssid = env_or_empty("BROSYS_TEST_WIFI_SSID");
    std::string bssid = lower(env_or_empty("BROSYS_TEST_WIFI_BSSID"));
    if (ifname.empty() || ssid.empty())
        bstest::skip(kName, "opt-in: set BROSYS_TEST_WIFI_DEVICE / _SSID / _BSSID (see the test's header)");

    std::string err;
    brosys::NetworkConfig cfg;
    cfg.scan_timeout_ms = 30000;
    auto net = brosys::NetworkService::create(cfg, &err);
    if (!net) bstest::skip(kName, "NetworkManager unavailable: " + err);
    bstest::EventLog<brosys::NetworkEvent> log(net->events());

    std::string dev;
    for (auto& d : net->state().devices)
        if (d.interface_name == ifname) {
            CHECK(d.type == brosys::LinkType::WiFi);
            dev = d.id;
        }
    REQUIRE(!dev.empty());
    std::printf("scanning on %s (%s)\n", ifname.c_str(), dev.c_str());

    // NM refuses scans requested too soon after the previous one; retry a few times.
    std::optional<brosys::WifiScanCompleted> done;
    for (int attempt = 0; attempt < 6 && !done; ++attempt) {
        log.skip_all();
        auto r = net->request_wifi_scan(dev);
        if (!r.ok) {
            std::printf("scan refused (%s), retrying\n", r.error.c_str());
            std::this_thread::sleep_for(3s);
            continue;
        }
        done = log.wait<brosys::WifiScanCompleted>(
            [&](const brosys::WifiScanCompleted& c) { return c.device_id == dev; }, 35000ms);
    }
    REQUIRE(done.has_value());
    CHECK(done->ok);
    const brosys::WifiAccessPoint* ap = nullptr;
    for (auto& a : done->access_points) {
        std::printf("  ap '%s' %s ch %u %u MHz %u%% %s%s\n", a.ssid.c_str(), a.bssid.c_str(), a.channel,
                    a.frequency_mhz, a.strength_percent, brosys::to_string(a.security), a.active ? " active" : "");
        if (a.ssid == ssid) ap = &a;
    }
    REQUIRE(ap != nullptr);
    if (!bssid.empty()) CHECK_EQ(ap->bssid, bssid);
    CHECK_EQ(ap->channel, 6u);
    CHECK_EQ(ap->frequency_mhz, 2437u);
    CHECK(ap->security == brosys::WifiSecurity::Wpa2Personal);
    CHECK(!ap->active);  // never associated
    CHECK_EQ(ap->device_id, dev);
    CHECK(!net->access_points(dev).empty());

    // nmcli's view of the same device (no rescan).
    auto r = bstest::run({"nmcli", "-t", "-f", "SSID,BSSID,CHAN,FREQ,SECURITY", "device", "wifi", "list", "ifname",
                          ifname, "--rescan", "no"});
    std::istringstream in(r.out);
    std::string line;
    bool seen = false;
    while (std::getline(in, line)) {
        if (line.rfind(ssid + ":", 0) != 0) continue;
        seen = true;
        CHECK(line.find(":6:") != std::string::npos);
        CHECK(line.find("2437") != std::string::npos);
        CHECK(line.find("WPA2") != std::string::npos);
    }
    CHECK(seen);

    // Still disconnected: nothing was joined.
    for (auto& d : net->state().devices)
        if (d.id == dev) CHECK(d.state != brosys::LinkState::Connected && d.connection.empty());
}

}  // namespace

int main() {
    run_test();
    return bstest::finish(kName);
}
