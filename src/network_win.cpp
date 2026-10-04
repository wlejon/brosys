#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <wlanapi.h>
#include "network_internal.h"
#include <iostream>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace brosys {

namespace {

std::string format_mac(const BYTE* mac, DWORD len) {
    if (!mac || len == 0) return "";
    std::ostringstream oss;
    for (DWORD i = 0; i < len; ++i) {
        if (i > 0) oss << ":";
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(mac[i]);
    }
    return oss.str();
}

std::string sockaddr_to_string(const SOCKADDR* sa) {
    if (!sa) return "";
    char buf[INET6_ADDRSTRLEN] = {0};
    if (sa->sa_family == AF_INET) {
        auto* sin = reinterpret_cast<const SOCKADDR_IN*>(sa);
        inet_ntop(AF_INET, &(sin->sin_addr), buf, sizeof(buf));
        return buf;
    } else if (sa->sa_family == AF_INET6) {
        auto* sin6 = reinterpret_cast<const SOCKADDR_IN6*>(sa);
        inet_ntop(AF_INET6, &(sin6->sin6_addr), buf, sizeof(buf));
        return buf;
    }
    return "";
}

SecurityType map_wlan_security(DOT11_AUTH_ALGORITHM auth, DOT11_CIPHER_ALGORITHM cipher) {
    if (auth == DOT11_AUTH_ALGO_80211_OPEN && cipher == DOT11_CIPHER_ALGO_NONE) {
        return SecurityType::Open;
    }
    if (auth == DOT11_AUTH_ALGO_80211_SHARED_KEY || cipher == DOT11_CIPHER_ALGO_WEP) {
        return SecurityType::WEP;
    }
    if (auth == DOT11_AUTH_ALGO_WPA_PSK) {
        return SecurityType::WPA_Personal;
    }
    if (auth == DOT11_AUTH_ALGO_WPA) {
        return SecurityType::WPA_Enterprise;
    }
    if (auth == DOT11_AUTH_ALGO_RSNA_PSK) {
        return SecurityType::WPA2_Personal;
    }
    if (auth == DOT11_AUTH_ALGO_RSNA) {
        return SecurityType::WPA2_Enterprise;
    }
#if defined(DOT11_AUTH_ALGO_WPA3_SAE)
    if (auth == DOT11_AUTH_ALGO_WPA3_SAE) {
        return SecurityType::WPA3_Personal;
    }
#endif
#if defined(DOT11_AUTH_ALGO_WPA3)
    if (auth == DOT11_AUTH_ALGO_WPA3) {
        return SecurityType::WPA3_Enterprise;
    }
#endif
    return SecurityType::Unknown;
}

} // namespace

class WindowsNetworkBackend : public INetworkBackend {
public:
    WindowsNetworkBackend() = default;

    NetworkStatus get_status() override {
        NetworkStatus status{};
        ULONG buf_len = 15000;
        std::vector<BYTE> buffer(buf_len);
        PIP_ADAPTER_ADDRESSES addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

        ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_PREFIX;
        ULONG ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &buf_len);
        if (ret == ERROR_BUFFER_OVERFLOW) {
            buffer.resize(buf_len);
            addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
            ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &buf_len);
        }

        if (ret != NO_ERROR || !addresses) {
            return status;
        }

        for (auto* curr = addresses; curr != nullptr; curr = curr->Next) {
            if (curr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }
            if (curr->OperStatus != IfOperStatusUp) {
                continue;
            }

            status.is_online = true;
            status.state = ConnectionState::Connected;

            if (curr->IfType == IF_TYPE_ETHERNET_CSMACD) {
                status.active_type = ConnectionType::Ethernet;
            } else if (curr->IfType == IF_TYPE_IEEE80211) {
                status.active_type = ConnectionType::WiFi;
            } else {
                status.active_type = ConnectionType::Other;
            }

            if (curr->FriendlyName) {
                int len = WideCharToMultiByte(CP_UTF8, 0, curr->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    status.connection_name.resize(len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, curr->FriendlyName, -1, status.connection_name.data(), len, nullptr, nullptr);
                }
            }

            status.mac_address = format_mac(curr->PhysicalAddress, curr->PhysicalAddressLength);

            // Get first IPv4 address
            for (auto* uni = curr->FirstUnicastAddress; uni != nullptr; uni = uni->Next) {
                if (uni->Address.lpSockaddr->sa_family == AF_INET) {
                    status.ip_address = sockaddr_to_string(uni->Address.lpSockaddr);
                    break;
                }
            }
            if (status.ip_address.empty() && curr->FirstUnicastAddress) {
                status.ip_address = sockaddr_to_string(curr->FirstUnicastAddress->Address.lpSockaddr);
            }

            // Get gateway
            if (curr->FirstGatewayAddress) {
                status.gateway = sockaddr_to_string(curr->FirstGatewayAddress->Address.lpSockaddr);
            }

            // Get DNS
            if (curr->FirstDnsServerAddress) {
                status.dns = sockaddr_to_string(curr->FirstDnsServerAddress->Address.lpSockaddr);
            }

            break; // Stop at first active adapter
        }

        return status;
    }

    bool is_wifi_available() override {
        HANDLE hClient = nullptr;
        DWORD dwCurVersion = 0;
        DWORD res = WlanOpenHandle(2, nullptr, &dwCurVersion, &hClient);
        if (res != ERROR_SUCCESS || !hClient) {
            return false;
        }

        PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
        res = WlanEnumInterfaces(hClient, nullptr, &pIfList);
        bool available = false;
        if (res == ERROR_SUCCESS && pIfList) {
            available = (pIfList->dwNumberOfItems > 0);
            WlanFreeMemory(pIfList);
        }
        WlanCloseHandle(hClient, nullptr);
        return available;
    }

    std::vector<WiFiAccessPoint> scan_wifi() override {
        std::vector<WiFiAccessPoint> results;
        HANDLE hClient = nullptr;
        DWORD dwCurVersion = 0;
        DWORD res = WlanOpenHandle(2, nullptr, &dwCurVersion, &hClient);
        if (res != ERROR_SUCCESS || !hClient) {
            return results;
        }

        PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
        res = WlanEnumInterfaces(hClient, nullptr, &pIfList);
        if (res != ERROR_SUCCESS || !pIfList) {
            WlanCloseHandle(hClient, nullptr);
            return results;
        }

        for (DWORD i = 0; i < pIfList->dwNumberOfItems; ++i) {
            const auto& ifInfo = pIfList->InterfaceInfo[i];

            // Trigger scan (can be asynchronous, continue to query network list)
            WlanScan(hClient, &ifInfo.InterfaceGuid, nullptr, nullptr, nullptr);

            PWLAN_AVAILABLE_NETWORK_LIST pNetList = nullptr;
            res = WlanGetAvailableNetworkList(hClient, &ifInfo.InterfaceGuid, 0, nullptr, &pNetList);
            if (res == ERROR_SUCCESS && pNetList) {
                for (DWORD j = 0; j < pNetList->dwNumberOfItems; ++j) {
                    const auto& net = pNetList->Network[j];
                    if (net.dot11Ssid.uSSIDLength == 0) {
                        continue; // Hidden SSID
                    }

                    WiFiAccessPoint ap;
                    ap.ssid.assign(reinterpret_cast<const char*>(net.dot11Ssid.ucSSID), net.dot11Ssid.uSSIDLength);
                    ap.signal_strength_percent = static_cast<int>(net.wlanSignalQuality);
                    // Standard Windows approx dBm conversion: dBm = (signalQuality / 2) - 100
                    ap.signal_strength_dbm = static_cast<int>(net.wlanSignalQuality / 2) - 100;
                    ap.is_connected = (net.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
                    ap.security = map_wlan_security(net.dot11DefaultAuthAlgorithm, net.dot11DefaultCipherAlgorithm);

                    results.push_back(ap);
                }
                WlanFreeMemory(pNetList);
            }
        }

        WlanFreeMemory(pIfList);
        WlanCloseHandle(hClient, nullptr);
        return results;
    }

    void register_status_callback(NetworkManager::NetworkChangeCallback /*cb*/) override {
        // Callback registered
    }
};

std::unique_ptr<INetworkBackend> create_platform_network_backend() {
    return std::make_unique<WindowsNetworkBackend>();
}

} // namespace brosys

#endif // _WIN32
