// Windows Bluetooth backend using Win32 Bluetooth APIs (<bluetoothapis.h> / Bthprops.lib).
// Enumerates local Bluetooth radios and paired/remembered/connected/in-range devices,
// and supports adapter discovery/power and device operations. Even when no physical
// Bluetooth radio is attached, BluetoothService::create() succeeds with empty adapters().
#include "brosys/bluetooth.h"
#include "win/util.h"

#include <windows.h>
#include <bluetoothapis.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace brosys {

namespace {

// BLUETOOTH_ADDRESS::rgBytes is little-endian: rgBytes[5] is the MSB (first OUI byte).
std::string format_bt_mac(const BYTE rgBytes[6]) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  rgBytes[5], rgBytes[4], rgBytes[3], rgBytes[2], rgBytes[1], rgBytes[0]);
    return buf;
}

std::string upper_mac(std::string s) {
    for (char& c : s) {
        if (c == '-') c = ':';
        else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

bool parse_bt_mac(const std::string& mac_str, BLUETOOTH_ADDRESS* out) {
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    std::string norm = upper_mac(mac_str);
    unsigned int b[6] = {};
    if (std::sscanf(norm.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X",
                    &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        out->rgBytes[5 - i] = static_cast<BYTE>(b[i] & 0xFFu);
    }
    return true;
}

std::string icon_from_cod(ULONG cod) {
    ULONG major = (cod >> 8) & 0x1F;
    ULONG minor = (cod >> 2) & 0x3F;
    switch (major) {
        case 1: return "computer";
        case 2: return "phone";
        case 4:
            if (minor == 1 || minor == 2 || minor == 6) return "audio-headset";
            return "audio-card";
        case 5:
            if ((minor & 0x10) != 0) return "input-keyboard";
            if ((minor & 0x20) != 0) return "input-mouse";
            return "input-gaming";
        case 6: return "camera-photo";
        default: return "bluetooth";
    }
}

// Standard Bluetooth service GUIDs for connect/disconnect via BluetoothSetServiceState
constexpr GUID kHandsfreeServiceClass = {0x0000111E, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
constexpr GUID kAudioSinkServiceClass = {0x0000110B, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
constexpr GUID kHeadsetServiceClass   = {0x00001108, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
constexpr GUID kHidServiceClass       = {0x00001124, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

class WinBluetoothService final : public BluetoothService {
public:
    explicit WinBluetoothService(const BluetoothConfig& cfg) : config_(cfg) {
        refresh(false, false);
    }

    ~WinBluetoothService() override {
        stop_inquiry();
    }

    BluetoothEventQueue& events() override { return queue_; }

    std::vector<BluetoothAdapter> adapters() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return adapters_;
    }

    std::optional<BluetoothAdapter> default_adapter() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (adapters_.empty()) return std::nullopt;
        return adapters_.front();
    }

    std::vector<BluetoothDevice> devices() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return devices_;
    }

    std::optional<BluetoothDevice> device(const std::string& mac) const override {
        const std::string target = upper_mac(mac);
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& d : devices_) {
            if (upper_mac(d.mac) == target || d.id == mac) return d;
        }
        return std::nullopt;
    }

    Result set_powered(bool powered, const std::string& adapter_id) override {
        HANDLE hRadio = find_radio_handle(adapter_id);
        if (!hRadio) {
            return Result::failure("no Bluetooth adapter found");
        }
        BluetoothEnableIncomingConnections(hRadio, powered ? TRUE : FALSE);
        if (!powered) {
            BluetoothEnableDiscovery(hRadio, FALSE);
        }
        CloseHandle(hRadio);
        refresh(false, true);
        return Result::success();
    }

    Result start_discovery(const std::string& adapter_id) override {
        HANDLE hRadio = find_radio_handle(adapter_id);
        if (!hRadio) {
            return Result::failure("no Bluetooth adapter found");
        }
        CloseHandle(hRadio);

        discovering_.store(true);
        refresh(false, true);

        stop_inquiry();
        inquiry_stop_.store(false);
        inquiry_thread_ = std::thread([this] {
            if (!inquiry_stop_.load()) {
                refresh(true, true);
            }
        });
        return Result::success();
    }

    Result stop_discovery(const std::string& adapter_id) override {
        HANDLE hRadio = find_radio_handle(adapter_id);
        if (!hRadio) {
            return Result::failure("no Bluetooth adapter found");
        }
        CloseHandle(hRadio);

        discovering_.store(false);
        stop_inquiry();
        refresh(false, true);
        return Result::success();
    }

    Result connect_device(const std::string& mac) override {
        return set_device_services(mac, true);
    }

    Result disconnect_device(const std::string& mac) override {
        return set_device_services(mac, false);
    }

    Result pair_device(const std::string& mac) override {
        BLUETOOTH_ADDRESS addr{};
        if (!parse_bt_mac(mac, &addr)) {
            return Result::failure("invalid Bluetooth MAC address: " + mac);
        }
        HANDLE hRadio = find_radio_handle("");
        BLUETOOTH_DEVICE_INFO dev_info{};
        dev_info.dwSize = sizeof(dev_info);
        dev_info.Address = addr;
        DWORD r = BluetoothGetDeviceInfo(hRadio, &dev_info);
        if (r != ERROR_SUCCESS) {
            if (hRadio) CloseHandle(hRadio);
            return Result::failure(win::win32_error("BluetoothGetDeviceInfo", r));
        }
        if (dev_info.fAuthenticated) {
            if (hRadio) CloseHandle(hRadio);
            return Result::success();
        }
        r = BluetoothAuthenticateDeviceEx(nullptr, hRadio, &dev_info, nullptr, MITMProtectionNotRequired);
        if (hRadio) CloseHandle(hRadio);
        if (r != ERROR_SUCCESS && r != ERROR_NO_MORE_ITEMS) {
            return Result::failure(win::win32_error("BluetoothAuthenticateDeviceEx", r));
        }
        refresh(false, true);
        return Result::success();
    }

    Result remove_device(const std::string& mac) override {
        BLUETOOTH_ADDRESS addr{};
        if (!parse_bt_mac(mac, &addr)) {
            return Result::failure("invalid Bluetooth MAC address: " + mac);
        }
        DWORD r = BluetoothRemoveDevice(&addr);
        if (r != ERROR_SUCCESS) {
            return Result::failure(win::win32_error("BluetoothRemoveDevice", r));
        }
        refresh(false, true);
        return Result::success();
    }

private:
    void stop_inquiry() {
        inquiry_stop_.store(true);
        if (inquiry_thread_.joinable()) {
            inquiry_thread_.join();
        }
    }

    static HANDLE find_radio_handle(const std::string& adapter_id) {
        BLUETOOTH_FIND_RADIO_PARAMS params{};
        params.dwSize = sizeof(params);
        HANDLE hRadio = nullptr;
        HBLUETOOTH_RADIO_FIND hFind = BluetoothFindFirstRadio(&params, &hRadio);
        if (!hFind) return nullptr;

        do {
            if (adapter_id.empty()) {
                BluetoothFindRadioClose(hFind);
                return hRadio;
            }
            BLUETOOTH_RADIO_INFO info{};
            info.dwSize = sizeof(info);
            if (BluetoothGetRadioInfo(hRadio, &info) == ERROR_SUCCESS) {
                std::string mac = format_bt_mac(info.address.rgBytes);
                std::string id = "hci:" + mac;
                if (adapter_id == id || upper_mac(adapter_id) == mac) {
                    BluetoothFindRadioClose(hFind);
                    return hRadio;
                }
            }
            CloseHandle(hRadio);
            hRadio = nullptr;
        } while (BluetoothFindNextRadio(hFind, &hRadio));

        BluetoothFindRadioClose(hFind);
        return nullptr;
    }

    Result set_device_services(const std::string& mac, bool enable) {
        BLUETOOTH_ADDRESS addr{};
        if (!parse_bt_mac(mac, &addr)) {
            return Result::failure("invalid Bluetooth MAC address: " + mac);
        }
        HANDLE hRadio = find_radio_handle("");
        if (!hRadio) {
            return Result::failure("no Bluetooth adapter found");
        }
        BLUETOOTH_DEVICE_INFO dev_info{};
        dev_info.dwSize = sizeof(dev_info);
        dev_info.Address = addr;
        DWORD r = BluetoothGetDeviceInfo(hRadio, &dev_info);
        if (r != ERROR_SUCCESS) {
            CloseHandle(hRadio);
            return Result::failure(win::win32_error("BluetoothGetDeviceInfo", r));
        }

        const GUID services[] = {
            kAudioSinkServiceClass,
            kHandsfreeServiceClass,
            kHeadsetServiceClass,
            kHidServiceClass,
        };
        DWORD flags = enable ? BLUETOOTH_SERVICE_ENABLE : BLUETOOTH_SERVICE_DISABLE;
        bool any_ok = false;
        DWORD last_err = ERROR_SUCCESS;
        for (const auto& guid : services) {
            DWORD sr = BluetoothSetServiceState(hRadio, &dev_info, &guid, flags);
            if (sr == ERROR_SUCCESS) {
                any_ok = true;
            } else {
                last_err = sr;
            }
        }
        CloseHandle(hRadio);
        if (!any_ok && last_err != ERROR_SUCCESS) {
            return Result::failure(win::win32_error("BluetoothSetServiceState", last_err));
        }
        refresh(false, true);
        return Result::success();
    }

    void refresh(bool issue_inquiry, bool emit_events) {
        std::vector<BluetoothAdapter> new_adapters;
        std::vector<BluetoothDevice> new_devices;

        BLUETOOTH_FIND_RADIO_PARAMS radio_params{};
        radio_params.dwSize = sizeof(radio_params);
        HANDLE hRadio = nullptr;
        HBLUETOOTH_RADIO_FIND hFind = BluetoothFindFirstRadio(&radio_params, &hRadio);
        if (hFind) {
            do {
                BLUETOOTH_RADIO_INFO rinfo{};
                rinfo.dwSize = sizeof(rinfo);
                std::string adapter_id = "hci0";
                if (BluetoothGetRadioInfo(hRadio, &rinfo) == ERROR_SUCCESS) {
                    BluetoothAdapter a;
                    a.address = format_bt_mac(rinfo.address.rgBytes);
                    a.id = "hci:" + a.address;
                    adapter_id = a.id;
                    a.name = win::to_utf8(rinfo.szName);
                    a.alias = a.name;
                    a.powered = true;
                    a.discovering = discovering_.load();
                    a.pairable = BluetoothIsConnectable(hRadio) != FALSE;
                    a.discoverable = BluetoothIsDiscoverable(hRadio) != FALSE;
                    new_adapters.push_back(std::move(a));
                }

                BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
                search.dwSize = sizeof(search);
                search.fReturnAuthenticated = TRUE;
                search.fReturnRemembered = TRUE;
                search.fReturnUnknown = TRUE;
                search.fReturnConnected = TRUE;
                search.fIssueInquiry = issue_inquiry ? TRUE : FALSE;
                search.cTimeoutMultiplier = issue_inquiry ? 2 : 0;
                search.hRadio = hRadio;

                BLUETOOTH_DEVICE_INFO dinfo{};
                dinfo.dwSize = sizeof(dinfo);
                HBLUETOOTH_DEVICE_FIND hDev = BluetoothFindFirstDevice(&search, &dinfo);
                if (hDev) {
                    do {
                        BluetoothDevice d;
                        d.mac = format_bt_mac(dinfo.Address.rgBytes);
                        d.adapter_id = adapter_id;
                        d.id = adapter_id + "/dev_" + d.mac;
                        d.name = win::to_utf8(dinfo.szName);
                        d.alias = d.name.empty() ? d.mac : d.name;
                        d.icon = icon_from_cod(dinfo.ulClassofDevice);
                        d.paired = dinfo.fAuthenticated != FALSE;
                        d.connected = dinfo.fConnected != FALSE;
                        d.trusted = dinfo.fRemembered != FALSE;
                        d.blocked = false;
                        new_devices.push_back(std::move(d));
                    } while (BluetoothFindNextDevice(hDev, &dinfo));
                    BluetoothFindDeviceClose(hDev);
                }

                CloseHandle(hRadio);
                hRadio = nullptr;
            } while (BluetoothFindNextRadio(hFind, &hRadio));
            BluetoothFindRadioClose(hFind);
        }

        std::vector<BluetoothAdapter> old_adapters;
        std::vector<BluetoothDevice> old_devices;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            old_adapters = std::move(adapters_);
            old_devices = std::move(devices_);
            adapters_ = new_adapters;
            devices_ = new_devices;
        }

        if (!emit_events) return;

        for (const auto& a : new_adapters) {
            auto it = std::find_if(old_adapters.begin(), old_adapters.end(),
                                   [&](const BluetoothAdapter& o) { return o.id == a.id; });
            if (it == old_adapters.end() || !(*it == a)) {
                queue_.push(AdapterChanged{a});
            }
        }

        for (const auto& d : new_devices) {
            auto it = std::find_if(old_devices.begin(), old_devices.end(),
                                   [&](const BluetoothDevice& o) { return o.mac == d.mac; });
            if (it == old_devices.end()) {
                queue_.push(DeviceFound{d});
            } else if (!(*it == d)) {
                queue_.push(DeviceChanged{d});
            }
        }

        for (const auto& o : old_devices) {
            auto it = std::find_if(new_devices.begin(), new_devices.end(),
                                   [&](const BluetoothDevice& d) { return d.mac == o.mac; });
            if (it == new_devices.end()) {
                queue_.push(DeviceRemoved{o.mac, o.id});
            }
        }
    }

    BluetoothConfig config_;
    BluetoothEventQueue queue_;
    mutable std::mutex mutex_;
    std::vector<BluetoothAdapter> adapters_;
    std::vector<BluetoothDevice> devices_;
    std::atomic<bool> discovering_{false};
    std::atomic<bool> inquiry_stop_{false};
    std::thread inquiry_thread_;
};

}  // namespace

std::unique_ptr<BluetoothService> BluetoothService::create(const BluetoothConfig& config, std::string* error) {
    if (error) error->clear();
    return std::make_unique<WinBluetoothService>(config);
}

}  // namespace brosys
