// Bluetooth: adapters and device management.
//
// Linux: BlueZ 5 over the system bus (org.bluez.Adapter1, org.bluez.Device1,
// org.freedesktop.DBus.ObjectManager at /).
#pragma once

#include "brosys/common.h"
#include "brosys/event_queue.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace brosys {

struct BluetoothAdapter {
    std::string id;         // BlueZ object path, e.g. "/org/bluez/hci0"
    std::string address;    // MAC address, e.g. "00:11:22:33:44:55"
    std::string name;       // controller name
    std::string alias;      // user-friendly alias
    bool powered = false;
    bool discovering = false;
    bool pairable = true;
    bool discoverable = false;

    bool operator==(const BluetoothAdapter&) const = default;
};

struct BluetoothDevice {
    std::string id;         // BlueZ object path, e.g. "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
    std::string adapter_id; // object path of adapter
    std::string mac;        // MAC address, e.g. "AA:BB:CC:DD:EE:FF"
    std::string name;
    std::string alias;
    std::string icon;
    bool paired = false;
    bool connected = false;
    bool trusted = false;
    bool blocked = false;
    std::optional<int16_t> rssi;

    bool operator==(const BluetoothDevice&) const = default;
};

// ---------------------------------------------------------------- events

struct AdapterChanged {
    BluetoothAdapter adapter;
};

struct DeviceFound {
    BluetoothDevice device;
};

struct DeviceChanged {
    BluetoothDevice device;
};

struct DeviceRemoved {
    std::string mac;
    std::string id;
};

using BluetoothEvent = std::variant<AdapterChanged, DeviceFound, DeviceChanged, DeviceRemoved>;
using BluetoothEventQueue = MessageQueue<BluetoothEvent>;

struct BluetoothConfig {
    // Linux: system-bus address override; empty = default system bus
    std::string system_bus_address;
};

class BluetoothService {
public:
    static std::unique_ptr<BluetoothService> create(const BluetoothConfig& config, std::string* error);
    virtual ~BluetoothService() = default;

    virtual BluetoothEventQueue& events() = 0;

    virtual std::vector<BluetoothAdapter> adapters() const = 0;
    virtual std::optional<BluetoothAdapter> default_adapter() const = 0;
    virtual std::vector<BluetoothDevice> devices() const = 0;
    virtual std::optional<BluetoothDevice> device(const std::string& mac) const = 0;

    // Adapter controls (adapter_id empty = default adapter)
    virtual Result set_powered(bool powered, const std::string& adapter_id = "") = 0;
    virtual Result start_discovery(const std::string& adapter_id = "") = 0;
    virtual Result stop_discovery(const std::string& adapter_id = "") = 0;

    // Device operations (by MAC address)
    virtual Result connect_device(const std::string& mac) = 0;
    virtual Result disconnect_device(const std::string& mac) = 0;
    virtual Result pair_device(const std::string& mac) = 0;
    virtual Result remove_device(const std::string& mac) = 0;
};

}  // namespace brosys
