#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>

#include "win/wifi.h"

#include "win/util.h"

#include <cstdio>

namespace brosys::win {

std::string format_mac(const uint8_t* mac, size_t len) {
    std::string out;
    char b[4];
    for (size_t i = 0; i < len; ++i) {
        std::snprintf(b, sizeof b, i ? ":%02x" : "%02x", mac[i]);
        out += b;
    }
    return out;
}

uint32_t channel_from_mhz(uint32_t mhz) {
    if (mhz == 2484) return 14;
    if (mhz >= 2412 && mhz < 2484) return (mhz - 2407) / 5;
    if (mhz >= 5955 && mhz <= 7115) return (mhz - 5950) / 5;  // 6 GHz
    if (mhz >= 5000 && mhz < 5955) return (mhz - 5000) / 5;
    if (mhz >= 4915 && mhz < 5000) return (mhz - 4000) / 5;
    return 0;
}

namespace {

// Strength order for picking the best AKM a BSS offers.
int rank(WifiSecurity s) {
    switch (s) {
        case WifiSecurity::Open: return 0;
        case WifiSecurity::Owe: return 1;
        case WifiSecurity::Wep: return 2;
        case WifiSecurity::WpaPersonal: return 3;
        case WifiSecurity::WpaEnterprise: return 4;
        case WifiSecurity::Wpa2Personal: return 5;
        case WifiSecurity::Wpa2Enterprise: return 6;
        case WifiSecurity::Wpa3Personal: return 7;
        case WifiSecurity::Wpa3Enterprise: return 8;
        case WifiSecurity::Unknown: break;
    }
    return -1;
}

void consider(WifiSecurity& best, WifiSecurity s) {
    if (rank(s) > rank(best)) best = s;
}

// Parses "version(2) group(4) pairwise-count(2) suites(4n) akm-count(2) akms(4n)"
// starting at p (after any OUI header), calling `akm` with each AKM suite.
template <class F>
void parse_suites(const uint8_t* p, size_t n, F akm) {
    size_t off = 2 + 4;  // version + group cipher
    if (n < off + 2) return;
    size_t pairwise = p[off] | (p[off + 1] << 8);
    off += 2 + 4 * pairwise;
    if (n < off + 2) return;
    size_t akms = p[off] | (p[off + 1] << 8);
    off += 2;
    for (size_t i = 0; i < akms && off + 4 <= n; ++i, off += 4) akm(p + off);
}

}  // namespace

WifiSecurity security_from_ies(const uint8_t* ies, size_t size, bool privacy) {
    WifiSecurity best = WifiSecurity::Unknown;
    bool rsn_or_wpa = false;
    size_t off = 0;
    while (off + 2 <= size) {
        uint8_t id = ies[off], len = ies[off + 1];
        const uint8_t* body = ies + off + 2;
        if (off + 2 + len > size) break;
        if (id == 48) {  // RSN
            rsn_or_wpa = true;
            parse_suites(body, len, [&](const uint8_t* s) {
                if (s[0] != 0x00 || s[1] != 0x0f || s[2] != 0xac) return;
                switch (s[3]) {
                    case 1: case 3: case 5: consider(best, WifiSecurity::Wpa2Enterprise); break;
                    case 2: case 4: case 6: consider(best, WifiSecurity::Wpa2Personal); break;
                    case 8: case 9: case 24: case 25: consider(best, WifiSecurity::Wpa3Personal); break;
                    case 11: case 12: case 13: consider(best, WifiSecurity::Wpa3Enterprise); break;
                    case 18: consider(best, WifiSecurity::Owe); break;
                    default: break;
                }
            });
        } else if (id == 221 && len >= 4 && body[0] == 0x00 && body[1] == 0x50 && body[2] == 0xf2 && body[3] == 1) {
            rsn_or_wpa = true;  // WPA (v1) vendor IE
            parse_suites(body + 4, len - 4, [&](const uint8_t* s) {
                if (s[0] != 0x00 || s[1] != 0x50 || s[2] != 0xf2) return;
                if (s[3] == 1) consider(best, WifiSecurity::WpaEnterprise);
                if (s[3] == 2) consider(best, WifiSecurity::WpaPersonal);
            });
        }
        off += 2 + static_cast<size_t>(len);
    }
    if (best != WifiSecurity::Unknown) return best;
    if (rsn_or_wpa) return WifiSecurity::Unknown;
    return privacy ? WifiSecurity::Wep : WifiSecurity::Open;
}

WifiClient::~WifiClient() { close(); }

bool WifiClient::open(Notify notify, std::string* error) {
    DWORD version = 0;
    DWORD r = WlanOpenHandle(2, nullptr, &version, &handle_);
    if (r != ERROR_SUCCESS) {
        handle_ = nullptr;
        if (error) *error = win32_error("WlanOpenHandle", r);
        return false;
    }
    notify_ = std::move(notify);
    r = WlanRegisterNotification(handle_, WLAN_NOTIFICATION_SOURCE_ACM | WLAN_NOTIFICATION_SOURCE_MSM, TRUE,
                                 &WifiClient::on_notification, this, nullptr, nullptr);
    if (r != ERROR_SUCCESS) {
        if (error) *error = win32_error("WlanRegisterNotification", r);
        WlanCloseHandle(handle_, nullptr);
        handle_ = nullptr;
        return false;
    }
    return true;
}

void WifiClient::close() {
    if (!handle_) return;
    // Unregistering waits for callbacks in flight.
    WlanRegisterNotification(handle_, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
    WlanCloseHandle(handle_, nullptr);
    handle_ = nullptr;
}

void WINAPI WifiClient::on_notification(PWLAN_NOTIFICATION_DATA data, PVOID context) {
    auto* self = static_cast<WifiClient*>(context);
    if (!data || !self || !self->notify_) return;
    if (data->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM) {
        switch (data->NotificationCode) {
            case wlan_notification_acm_scan_complete: self->notify_(data->InterfaceGuid, 1, 0); return;
            case wlan_notification_acm_scan_fail: {
                DWORD reason = data->dwDataSize >= sizeof(DWORD) && data->pData ? *static_cast<DWORD*>(data->pData) : 0;
                self->notify_(data->InterfaceGuid, 2, reason);
                return;
            }
            case wlan_notification_acm_scan_list_refresh:
            case wlan_notification_acm_network_available:
            case wlan_notification_acm_network_not_available:
                self->notify_(data->InterfaceGuid, 3, 0);  // AP list changed, state did not
                return;
            default: break;
        }
    }
    self->notify_(data->InterfaceGuid, 0, 0);
}

std::vector<WifiInterface> WifiClient::interfaces() {
    std::vector<WifiInterface> out;
    if (!handle_) return out;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(handle_, nullptr, &list) != ERROR_SUCCESS || !list) return out;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const auto& info = list->InterfaceInfo[i];
        WifiInterface w;
        w.guid = info.InterfaceGuid;
        w.description = to_utf8(info.strInterfaceDescription);
        NET_LUID luid{};
        if (ConvertInterfaceGuidToLuid(&info.InterfaceGuid, &luid) == NO_ERROR) w.device_id = std::to_string(luid.Value);
        w.connected = info.isState == wlan_interface_state_connected;

        DWORD size = 0;
        void* data = nullptr;
        if (WlanQueryInterface(handle_, &info.InterfaceGuid, wlan_intf_opcode_radio_state, nullptr, &size, &data,
                               nullptr) == ERROR_SUCCESS &&
            data) {
            auto* rs = static_cast<WLAN_RADIO_STATE*>(data);
            for (DWORD p = 0; p < rs->dwNumberOfPhys && p < WLAN_MAX_PHY_INDEX; ++p) {
                w.software_on |= rs->PhyRadioState[p].dot11SoftwareRadioState == dot11_radio_state_on;
                w.hardware_on |= rs->PhyRadioState[p].dot11HardwareRadioState == dot11_radio_state_on;
            }
            WlanFreeMemory(data);
        }
        data = nullptr;
        if (w.connected &&
            WlanQueryInterface(handle_, &info.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &size, &data,
                               nullptr) == ERROR_SUCCESS &&
            data) {
            auto* ca = static_cast<WLAN_CONNECTION_ATTRIBUTES*>(data);
            w.profile = to_utf8(ca->strProfileName);
            auto& ssid = ca->wlanAssociationAttributes.dot11Ssid;
            w.ssid.assign(reinterpret_cast<const char*>(ssid.ucSSID), ssid.uSSIDLength);
            w.bssid = format_mac(ca->wlanAssociationAttributes.dot11Bssid, 6);
            WlanFreeMemory(data);
        }
        out.push_back(std::move(w));
    }
    WlanFreeMemory(list);
    return out;
}

std::vector<WifiAccessPoint> WifiClient::access_points(const WifiInterface& iface) {
    std::vector<WifiAccessPoint> out;
    if (!handle_) return out;
    PWLAN_BSS_LIST bss = nullptr;
    if (WlanGetNetworkBssList(handle_, &iface.guid, nullptr, dot11_BSS_type_any, FALSE, nullptr, &bss) != ERROR_SUCCESS ||
        !bss)
        return out;
    for (DWORD i = 0; i < bss->dwNumberOfItems; ++i) {
        const WLAN_BSS_ENTRY& e = bss->wlanBssEntries[i];
        WifiAccessPoint ap;
        ap.device_id = iface.device_id;
        ap.ssid.assign(reinterpret_cast<const char*>(e.dot11Ssid.ucSSID), e.dot11Ssid.uSSIDLength);
        ap.bssid = format_mac(e.dot11Bssid, 6);
        ap.strength_percent = static_cast<uint8_t>(e.uLinkQuality > 100 ? 100 : e.uLinkQuality);
        ap.rssi_dbm = static_cast<int32_t>(e.lRssi);
        ap.frequency_mhz = e.ulChCenterFrequency / 1000;  // kHz
        ap.channel = channel_from_mhz(ap.frequency_mhz);
        const auto* ies = reinterpret_cast<const uint8_t*>(&e) + e.ulIeOffset;
        ap.security = security_from_ies(ies, e.ulIeSize, (e.usCapabilityInformation & 0x0010) != 0);
        ap.active = iface.connected && ap.bssid == iface.bssid;
        out.push_back(std::move(ap));
    }
    WlanFreeMemory(bss);
    return out;
}

DWORD WifiClient::scan(const GUID& iface) {
    if (!handle_) return ERROR_INVALID_HANDLE;
    return WlanScan(handle_, &iface, nullptr, nullptr, nullptr);
}

}  // namespace brosys::win
