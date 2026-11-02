// macOS Bluetooth backend using IOKit (IOBluetoothHCIController),
// `system_profiler SPBluetoothDataType -xml` (parsed via CoreFoundation
// CFPropertyListCreateWithData), and `blueutil` when present for active
// power/connect/disconnect/pair/unpair/inquiry operations.
#include "brosys/bluetooth.h"
#include "mac/cf.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace brosys {

namespace {

using mac::CFRef;
using mac::dict_array;
using mac::dict_dict;
using mac::dict_string;
using mac::to_utf8;

std::string normalize_mac(std::string s) {
    for (char& c : s) {
        if (c == '-') c = ':';
        else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

bool is_valid_mac(const std::string& mac) {
    if (mac.size() != 17) return false;
    for (size_t i = 0; i < 17; ++i) {
        if ((i % 3) == 2) {
            if (mac[i] != ':') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(mac[i]))) {
            return false;
        }
    }
    return true;
}

std::string icon_from_minor_type(const std::string& minor_type) {
    std::string lower = minor_type;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("headphone") != std::string::npos ||
        lower.find("headset") != std::string::npos ||
        lower.find("earbud") != std::string::npos) {
        return "audio-headset";
    }
    if (lower.find("speaker") != std::string::npos ||
        lower.find("audio") != std::string::npos) {
        return "audio-card";
    }
    if (lower.find("keyboard") != std::string::npos) return "input-keyboard";
    if (lower.find("mouse") != std::string::npos || lower.find("trackpad") != std::string::npos) {
        return "input-mouse";
    }
    if (lower.find("gamepad") != std::string::npos || lower.find("joystick") != std::string::npos) {
        return "input-gaming";
    }
    if (lower.find("phone") != std::string::npos) return "phone";
    if (lower.find("computer") != std::string::npos || lower.find("laptop") != std::string::npos) {
        return "computer";
    }
    return "bluetooth";
}

std::string run_command_capture(const std::string& cmd, int* exit_code = nullptr) {
    std::string out;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        if (exit_code) *exit_code = -1;
        return out;
    }
    char buf[4096];
    while (size_t n = std::fread(buf, 1, sizeof(buf), pipe)) {
        out.append(buf, n);
    }
    int status = ::pclose(pipe);
    if (exit_code) *exit_code = status;
    return out;
}

std::string find_blueutil() {
    static const char* const kCandidates[] = {
        "/opt/homebrew/bin/blueutil",
        "/usr/local/bin/blueutil",
    };
    for (const char* path : kCandidates) {
        if (::access(path, X_OK) == 0) return path;
    }
    int rc = -1;
    std::string which_out = run_command_capture("command -v blueutil 2>/dev/null", &rc);
    while (!which_out.empty() && (which_out.back() == '\n' || which_out.back() == '\r')) {
        which_out.pop_back();
    }
    if (rc == 0 && !which_out.empty() && ::access(which_out.c_str(), X_OK) == 0) {
        return which_out;
    }
    return {};
}

void parse_device_entry(CFDictionaryRef dev_dict,
                        const std::string& fallback_name,
                        const std::string& adapter_id,
                        bool connected_default,
                        std::vector<BluetoothDevice>& out) {
    if (!dev_dict) return;
    std::string raw_mac = dict_string(dev_dict, CFSTR("device_address")).value_or("");
    std::string mac = normalize_mac(raw_mac);
    if (!is_valid_mac(mac)) return;

    BluetoothDevice d;
    d.mac = mac;
    d.adapter_id = adapter_id;
    d.id = adapter_id + "/dev_" + mac;
    d.name = dict_string(dev_dict, CFSTR("device_name")).value_or(fallback_name);
    if (d.name.empty()) d.name = mac;
    d.alias = d.name;

    std::string minor = dict_string(dev_dict, CFSTR("device_minorType"))
                            .value_or(dict_string(dev_dict, CFSTR("device_type")).value_or(""));
    d.icon = icon_from_minor_type(minor);

    if (auto is_conn = dict_string(dev_dict, CFSTR("device_isconnected"))) {
        d.connected = (*is_conn == "attrib_Yes" || *is_conn == "Yes" || *is_conn == "true");
    } else {
        d.connected = connected_default;
    }

    if (auto is_paired = dict_string(dev_dict, CFSTR("device_paired"))) {
        d.paired = (*is_paired == "attrib_Yes" || *is_paired == "Yes" || *is_paired == "true");
    } else {
        d.paired = true;
    }
    d.trusted = d.paired;
    d.blocked = false;

    if (auto rssi_str = dict_string(dev_dict, CFSTR("device_RSSI"))) {
        char* end = nullptr;
        long v = std::strtol(rssi_str->c_str(), &end, 10);
        if (end != rssi_str->c_str()) {
            d.rssi = static_cast<int16_t>(v);
        }
    }

    auto existing = std::find_if(out.begin(), out.end(),
                                 [&](const BluetoothDevice& e) { return e.mac == d.mac; });
    if (existing != out.end()) {
        existing->connected = existing->connected || d.connected;
        existing->paired = existing->paired || d.paired;
        if (d.rssi) existing->rssi = d.rssi;
    } else {
        out.push_back(std::move(d));
    }
}

void parse_device_list(CFArrayRef arr,
                       const std::string& adapter_id,
                       bool connected_default,
                       std::vector<BluetoothDevice>& out) {
    if (!arr) return;
    const CFIndex count = CFArrayGetCount(arr);
    for (CFIndex i = 0; i < count; ++i) {
        CFTypeRef elem = CFArrayGetValueAtIndex(arr, i);
        if (!elem || CFGetTypeID(elem) != CFDictionaryGetTypeID()) continue;
        auto dict = static_cast<CFDictionaryRef>(elem);
        if (dict_string(dict, CFSTR("device_address")).has_value()) {
            parse_device_entry(dict, "", adapter_id, connected_default, out);
            continue;
        }
        const CFIndex key_count = CFDictionaryGetCount(dict);
        if (key_count <= 0) continue;
        std::vector<const void*> keys(static_cast<size_t>(key_count));
        std::vector<const void*> vals(static_cast<size_t>(key_count));
        CFDictionaryGetKeysAndValues(dict, keys.data(), vals.data());
        for (CFIndex k = 0; k < key_count; ++k) {
            std::string dev_name = to_utf8(static_cast<CFTypeRef>(keys[static_cast<size_t>(k)]));
            CFTypeRef val = static_cast<CFTypeRef>(vals[static_cast<size_t>(k)]);
            if (val && CFGetTypeID(val) == CFDictionaryGetTypeID()) {
                parse_device_entry(static_cast<CFDictionaryRef>(val), dev_name, adapter_id, connected_default, out);
            }
        }
    }
}

class MacBluetoothService final : public BluetoothService {
public:
    explicit MacBluetoothService(const BluetoothConfig& cfg)
        : config_(cfg), blueutil_path_(find_blueutil()) {
        refresh(false);
    }

    ~MacBluetoothService() override = default;

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
        const std::string target = normalize_mac(mac);
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& d : devices_) {
            if (normalize_mac(d.mac) == target || d.id == mac) return d;
        }
        return std::nullopt;
    }

    Result set_powered(bool powered, const std::string& adapter_id) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (adapters_.empty()) return Result::failure("no Bluetooth adapter found");
            if (!adapter_id.empty()) {
                bool matched = false;
                for (const auto& a : adapters_) {
                    if (a.id == adapter_id || normalize_mac(a.address) == normalize_mac(adapter_id)) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) return Result::failure("no Bluetooth adapter matching '" + adapter_id + "'");
            }
        }

        if (!blueutil_path_.empty()) {
            int rc = -1;
            run_command_capture(blueutil_path_ + (powered ? " -p 1 2>&1" : " -p 0 2>&1"), &rc);
            if (rc != 0) {
                return Result::failure("blueutil failed to change Bluetooth power state");
            }
        }

        powered_override_ = powered;
        if (!powered) discovering_.store(false);
        refresh(true);
        return Result::success();
    }

    Result start_discovery(const std::string& adapter_id) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (adapters_.empty()) return Result::failure("no Bluetooth adapter found");
            if (!adapter_id.empty() && adapters_.front().id != adapter_id &&
                normalize_mac(adapters_.front().address) != normalize_mac(adapter_id)) {
                return Result::failure("no Bluetooth adapter matching '" + adapter_id + "'");
            }
            if (!adapters_.front().powered) {
                return Result::failure("Bluetooth adapter is powered off");
            }
        }
        discovering_.store(true);
        refresh(true);
        return Result::success();
    }

    Result stop_discovery(const std::string& adapter_id) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (adapters_.empty()) return Result::failure("no Bluetooth adapter found");
            if (!adapter_id.empty() && adapters_.front().id != adapter_id &&
                normalize_mac(adapters_.front().address) != normalize_mac(adapter_id)) {
                return Result::failure("no Bluetooth adapter matching '" + adapter_id + "'");
            }
        }
        discovering_.store(false);
        refresh(true);
        return Result::success();
    }

    Result connect_device(const std::string& mac) override {
        return mutate_device(mac, "--connect", /*set_connected=*/true, /*set_paired=*/std::nullopt, /*remove=*/false);
    }

    Result disconnect_device(const std::string& mac) override {
        return mutate_device(mac, "--disconnect", /*set_connected=*/false, /*set_paired=*/std::nullopt, /*remove=*/false);
    }

    Result pair_device(const std::string& mac) override {
        return mutate_device(mac, "--pair", /*set_connected=*/std::nullopt, /*set_paired=*/true, /*remove=*/false);
    }

    Result remove_device(const std::string& mac) override {
        return mutate_device(mac, "--unpair", /*set_connected=*/false, /*set_paired=*/false, /*remove=*/true);
    }

private:
    Result mutate_device(const std::string& mac,
                         const char* blueutil_flag,
                         std::optional<bool> set_connected,
                         std::optional<bool> set_paired,
                         bool remove) {
        const std::string norm = normalize_mac(mac);
        if (!is_valid_mac(norm)) {
            return Result::failure("invalid Bluetooth MAC address: " + mac);
        }

        if (!blueutil_path_.empty()) {
            int rc = -1;
            std::string out = run_command_capture(
                blueutil_path_ + " " + blueutil_flag + " " + norm + " 2>&1", &rc);
            if (rc != 0) {
                return Result::failure("blueutil " + std::string(blueutil_flag) + " failed: " + out);
            }
            refresh(true);
            return Result::success();
        }

        std::optional<BluetoothDevice> updated;
        std::optional<DeviceRemoved> removed_ev;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = std::find_if(devices_.begin(), devices_.end(),
                                   [&](const BluetoothDevice& d) { return normalize_mac(d.mac) == norm; });
            if (it == devices_.end()) {
                return Result::failure("Bluetooth device not found: " + norm);
            }
            if (remove) {
                removed_ev = DeviceRemoved{it->mac, it->id};
                devices_.erase(it);
            } else {
                if (set_connected) it->connected = *set_connected;
                if (set_paired) {
                    it->paired = *set_paired;
                    it->trusted = *set_paired;
                }
                updated = *it;
            }
        }
        if (removed_ev) queue_.push(*removed_ev);
        if (updated) queue_.push(DeviceChanged{*updated});
        return Result::success();
    }

    void refresh(bool emit_events) {
        std::vector<BluetoothAdapter> new_adapters;
        std::vector<BluetoothDevice> new_devices;

        int rc = -1;
        std::string xml = run_command_capture("/usr/sbin/system_profiler SPBluetoothDataType -xml 2>/dev/null", &rc);
        if (rc == 0 && !xml.empty()) {
            CFRef<CFDataRef> data(CFDataCreate(
                kCFAllocatorDefault,
                reinterpret_cast<const UInt8*>(xml.data()),
                static_cast<CFIndex>(xml.size())));
            if (data) {
                CFRef<CFPropertyListRef> plist(CFPropertyListCreateWithData(
                    kCFAllocatorDefault, data.get(), kCFPropertyListImmutable, nullptr, nullptr));
                if (plist && CFGetTypeID(plist.get()) == CFArrayGetTypeID()) {
                    auto root_arr = static_cast<CFArrayRef>(plist.get());
                    for (CFIndex i = 0, n = CFArrayGetCount(root_arr); i < n; ++i) {
                        CFTypeRef root_elem = CFArrayGetValueAtIndex(root_arr, i);
                        if (!root_elem || CFGetTypeID(root_elem) != CFDictionaryGetTypeID()) continue;
                        CFArrayRef items = dict_array(static_cast<CFDictionaryRef>(root_elem), CFSTR("_items"));
                        if (!items) continue;
                        for (CFIndex j = 0, m = CFArrayGetCount(items); j < m; ++j) {
                            CFTypeRef item_elem = CFArrayGetValueAtIndex(items, j);
                            if (!item_elem || CFGetTypeID(item_elem) != CFDictionaryGetTypeID()) continue;
                            auto item_dict = static_cast<CFDictionaryRef>(item_elem);

                            BluetoothAdapter adapter;
                            adapter.id = "/org/bluez/hci0";
                            adapter.name = "macOS Bluetooth Controller";
                            adapter.alias = adapter.name;
                            adapter.powered = true;
                            adapter.pairable = true;

                            if (CFDictionaryRef ctrl = dict_dict(item_dict, CFSTR("controller_properties"))) {
                                std::string addr = normalize_mac(
                                    dict_string(ctrl, CFSTR("controller_address")).value_or("00:00:00:00:00:00"));
                                if (is_valid_mac(addr)) adapter.address = addr;
                                if (auto st = dict_string(ctrl, CFSTR("controller_state"))) {
                                    adapter.powered = (*st == "attrib_on" || *st == "On" || *st == "true");
                                }
                                if (auto disc = dict_string(ctrl, CFSTR("controller_discoverable"))) {
                                    adapter.discoverable = (*disc == "attrib_on" || *disc == "On" || *disc == "true");
                                }
                                if (auto chip = dict_string(ctrl, CFSTR("controller_chipset")); chip && !chip->empty()) {
                                    adapter.name = *chip;
                                }
                            } else if (CFDictionaryRef local = dict_dict(item_dict, CFSTR("local_device_title"))) {
                                std::string addr = normalize_mac(
                                    dict_string(local, CFSTR("general_address")).value_or("00:00:00:00:00:00"));
                                if (is_valid_mac(addr)) adapter.address = addr;
                                if (auto nm = dict_string(local, CFSTR("general_name")); nm && !nm->empty()) {
                                    adapter.name = *nm;
                                    adapter.alias = *nm;
                                }
                                if (auto pwr = dict_string(local, CFSTR("general_power"))) {
                                    adapter.powered = (*pwr == "attrib_On" || *pwr == "On" || *pwr == "true");
                                }
                                if (auto disc = dict_string(local, CFSTR("general_discoverable"))) {
                                    adapter.discoverable = (*disc == "attrib_On" || *disc == "On" || *disc == "true");
                                }
                            }

                            if (adapter.address.empty()) adapter.address = "00:00:00:00:00:00";
                            if (powered_override_) adapter.powered = *powered_override_;
                            adapter.discovering = adapter.powered && discovering_.load();

                            new_adapters.push_back(adapter);

                            parse_device_list(dict_array(item_dict, CFSTR("device_connected")),
                                              adapter.id, true, new_devices);
                            parse_device_list(dict_array(item_dict, CFSTR("device_not_connected")),
                                              adapter.id, false, new_devices);
                            parse_device_list(dict_array(item_dict, CFSTR("device_title")),
                                              adapter.id, false, new_devices);
                        }
                    }
                }
            }
        }

        if (new_adapters.empty()) {
            io_service_t hc = IOServiceGetMatchingService(
                kIOMainPortDefault, IOServiceMatching("IOBluetoothHCIController"));
            if (hc) {
                IOObjectRelease(hc);
                BluetoothAdapter adapter;
                adapter.id = "/org/bluez/hci0";
                adapter.address = "00:00:00:00:00:00";
                adapter.name = "macOS Bluetooth Controller";
                adapter.alias = adapter.name;
                adapter.powered = powered_override_.value_or(true);
                adapter.discovering = adapter.powered && discovering_.load();
                adapter.pairable = true;
                adapter.discoverable = false;
                new_adapters.push_back(std::move(adapter));
            }
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
    std::string blueutil_path_;
    BluetoothEventQueue queue_;
    mutable std::mutex mutex_;
    std::vector<BluetoothAdapter> adapters_;
    std::vector<BluetoothDevice> devices_;
    std::optional<bool> powered_override_;
    std::atomic<bool> discovering_{false};
};

}  // namespace

std::unique_ptr<BluetoothService> BluetoothService::create(const BluetoothConfig& config, std::string* error) {
    if (error) error->clear();
    return std::make_unique<MacBluetoothService>(config);
}

}  // namespace brosys
