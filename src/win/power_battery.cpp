#include "win/power_battery.h"

#include "win/util.h"

#include <windows.h>
#include <winioctl.h>
#include <batclass.h>
#include <devguid.h>
#include <setupapi.h>

#include <initguid.h>
// GUID_DEVICE_BATTERY: the battery device interface class.
DEFINE_GUID(BROSYS_GUID_DEVICE_BATTERY, 0x72631e54, 0x78a4, 0x11d0, 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a);

#include <string>

namespace brosys::win {

namespace {

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

BatteryTechnology chemistry(const UCHAR c[4]) {
    std::string s(reinterpret_cast<const char*>(c), 4);
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    for (auto& ch : s) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    if (s == "LION" || s == "LI-I" || s == "LI I") return BatteryTechnology::LithiumIon;
    if (s == "LIP" || s == "LIPO" || s == "LI-P") return BatteryTechnology::LithiumPolymer;
    if (s == "LIFE" || s == "LFP") return BatteryTechnology::LithiumIronPhosphate;
    if (s == "PBAC") return BatteryTechnology::LeadAcid;
    if (s == "NICD") return BatteryTechnology::NickelCadmium;
    if (s == "NIMH") return BatteryTechnology::NickelMetalHydride;
    return BatteryTechnology::Unknown;
}

std::string query_string(HANDLE h, ULONG tag, BATTERY_QUERY_INFORMATION_LEVEL level) {
    BATTERY_QUERY_INFORMATION q{};
    q.BatteryTag = tag;
    q.InformationLevel = level;
    wchar_t buf[256] = {};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof q, buf, sizeof buf - sizeof(wchar_t), &got,
                         nullptr))
        return {};
    return to_utf8(std::wstring_view(buf, wcsnlen(buf, got / sizeof(wchar_t))));
}

bool read_battery(const wchar_t* device_path, PowerDevice& dev) {
    Handle h;
    h.h = CreateFileW(device_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h.h == INVALID_HANDLE_VALUE) return false;

    ULONG wait = 0, tag = 0;
    DWORD got = 0;
    if (!DeviceIoControl(h.h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof wait, &tag, sizeof tag, &got, nullptr) || !tag)
        return false;  // no battery in this slot

    BATTERY_QUERY_INFORMATION q{};
    q.BatteryTag = tag;
    q.InformationLevel = BatteryInformation;
    BATTERY_INFORMATION info{};
    if (!DeviceIoControl(h.h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof q, &info, sizeof info, &got, nullptr))
        return false;

    dev.id = to_utf8(device_path);
    bool ups = (info.Capabilities & BATTERY_IS_SHORT_TERM) != 0;
    dev.kind = ups ? PowerDeviceKind::Ups : PowerDeviceKind::Battery;
    dev.power_supply = (info.Capabilities & BATTERY_SYSTEM_BATTERY) != 0;
    dev.technology = chemistry(info.Chemistry);
    bool relative = (info.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0;
    if (!relative) {
        if (info.FullChargedCapacity && info.FullChargedCapacity != BATTERY_UNKNOWN_CAPACITY)
            dev.energy_full_wh = info.FullChargedCapacity / 1000.0;
        if (info.DesignedCapacity && info.DesignedCapacity != BATTERY_UNKNOWN_CAPACITY)
            dev.energy_full_design_wh = info.DesignedCapacity / 1000.0;
    }
    dev.model = query_string(h.h, tag, BatteryDeviceName);
    dev.vendor = query_string(h.h, tag, BatteryManufactureName);
    dev.serial = query_string(h.h, tag, BatterySerialNumber);

    BATTERY_WAIT_STATUS ws{};
    ws.BatteryTag = tag;
    BATTERY_STATUS st{};
    if (DeviceIoControl(h.h, IOCTL_BATTERY_QUERY_STATUS, &ws, sizeof ws, &st, sizeof st, &got, nullptr)) {
        bool charging = (st.PowerState & BATTERY_CHARGING) != 0;
        bool discharging = (st.PowerState & BATTERY_DISCHARGING) != 0;
        bool online = (st.PowerState & BATTERY_POWER_ON_LINE) != 0;
        if (st.Capacity != BATTERY_UNKNOWN_CAPACITY) {
            if (relative) {
                dev.percent = static_cast<double>(st.Capacity);  // relative units are percent
            } else {
                dev.energy_wh = st.Capacity / 1000.0;
                if (info.FullChargedCapacity && info.FullChargedCapacity != BATTERY_UNKNOWN_CAPACITY) {
                    double p = 100.0 * st.Capacity / info.FullChargedCapacity;
                    dev.percent = p > 100.0 ? 100.0 : p;
                }
            }
        }
        if (st.Rate != BATTERY_UNKNOWN_RATE && st.Rate != 0 && !relative) {
            LONG rate = static_cast<LONG>(st.Rate);
            dev.energy_rate_w = (rate < 0 ? -rate : rate) / 1000.0;
        }
        if (charging) dev.state = BatteryState::Charging;
        else if (discharging) dev.state = BatteryState::Discharging;
        else if (online && dev.percent && *dev.percent >= 99.5) dev.state = BatteryState::FullyCharged;
        else if (online) dev.state = BatteryState::PendingCharge;
        else if (dev.percent && *dev.percent <= 0.5) dev.state = BatteryState::Empty;

        if (charging && dev.energy_rate_w && *dev.energy_rate_w > 0 && dev.energy_wh && dev.energy_full_wh &&
            *dev.energy_full_wh > *dev.energy_wh)
            dev.time_to_full_s = static_cast<int64_t>((*dev.energy_full_wh - *dev.energy_wh) / *dev.energy_rate_w * 3600.0);
    }
    if (dev.state == BatteryState::Discharging) {
        q.InformationLevel = BatteryEstimatedTime;
        q.AtRate = 0;
        ULONG seconds = 0;
        if (DeviceIoControl(h.h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof q, &seconds, sizeof seconds, &got,
                            nullptr) &&
            seconds != BATTERY_UNKNOWN_TIME)
            dev.time_to_empty_s = static_cast<int64_t>(seconds);
    }
    return true;
}

}  // namespace

std::vector<PowerDevice> enumerate_batteries() {
    std::vector<PowerDevice> out;
    HDEVINFO set = SetupDiGetClassDevsW(&BROSYS_GUID_DEVICE_BATTERY, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    for (DWORD i = 0; i < 64; ++i) {
        SP_DEVICE_INTERFACE_DATA did{};
        did.cbSize = sizeof did;
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &BROSYS_GUID_DEVICE_BATTERY, i, &did)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &did, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, need, nullptr, nullptr)) continue;
        PowerDevice dev;
        if (read_battery(detail->DevicePath, dev)) out.push_back(std::move(dev));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

}  // namespace brosys::win
