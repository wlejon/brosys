// winsock2 / ws2tcpip before windows.h (see network.cpp).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "win/network_snapshot.h"

#include "win/util.h"
#include <netlistmgr.h>
#include <wrl/client.h>

#include <map>
#include <optional>

namespace brosys::win {

namespace {

using Microsoft::WRL::ComPtr;

std::string addr_text(const SOCKADDR* sa) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (sa->sa_family == AF_INET)
        inet_ntop(AF_INET, &reinterpret_cast<const SOCKADDR_IN*>(sa)->sin_addr, buf, sizeof buf);
    else if (sa->sa_family == AF_INET6)
        inet_ntop(AF_INET6, &reinterpret_cast<const SOCKADDR_IN6*>(sa)->sin6_addr, buf, sizeof buf);
    return buf;
}

LinkType link_type(IFTYPE t) {
    switch (t) {
        case IF_TYPE_ETHERNET_CSMACD: return LinkType::Ethernet;
        case IF_TYPE_IEEE80211: return LinkType::WiFi;
        case IF_TYPE_SOFTWARE_LOOPBACK: return LinkType::Loopback;
        case IF_TYPE_WWANPP:
        case IF_TYPE_WWANPP2: return LinkType::Cellular;
        case IF_TYPE_TUNNEL: return LinkType::Tunnel;
        case IF_TYPE_PPP: return LinkType::Vpn;
        case IF_TYPE_PROP_VIRTUAL: return LinkType::Virtual;
        default: return LinkType::Other;
    }
}

// Connectivity from the OS hint (Windows 10 2004+), loaded dynamically.
struct ConnectivityHint {
    int level;  // NL_NETWORK_CONNECTIVITY_LEVEL_HINT
    int cost;
    BOOLEAN approaching_data_limit;
    BOOLEAN over_data_limit;
    BOOLEAN roaming;
};
using GetHintFn = DWORD(WINAPI*)(ConnectivityHint*);

std::optional<Connectivity> hint_connectivity() {
    static GetHintFn fn = reinterpret_cast<GetHintFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"iphlpapi.dll"), "GetNetworkConnectivityHint")));
    if (!fn) return std::nullopt;
    ConnectivityHint h{};
    if (fn(&h) != NO_ERROR) return std::nullopt;
    switch (h.level) {
        case 1: return Connectivity::None;          // NetworkConnectivityLevelHintNone
        case 2: return Connectivity::Limited;       // LocalAccess
        case 3: return Connectivity::Full;          // InternetAccess
        case 4: return Connectivity::Portal;        // ConstrainedInternetAccess
        case 5: return Connectivity::Limited;       // Hidden
        default: return Connectivity::Unknown;
    }
}

struct NlmInfo {
    std::map<std::string, std::string> names;  // adapter GUID "{...}" (upper) -> network name
    std::optional<Connectivity> connectivity;
};

NlmInfo read_nlm() {
    NlmInfo out;
    ComPtr<INetworkListManager> nlm;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_INetworkListManager,
                                reinterpret_cast<void**>(nlm.GetAddressOf()))))
        return out;
    NLM_CONNECTIVITY c{};
    if (SUCCEEDED(nlm->GetConnectivity(&c))) {
        if (c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) out.connectivity = Connectivity::Full;
        else if (c & (NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_LOCALNETWORK |
                      NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV6_SUBNET))
            out.connectivity = Connectivity::Limited;
        else out.connectivity = Connectivity::None;
    }
    ComPtr<IEnumNetworkConnections> conns;
    if (FAILED(nlm->GetNetworkConnections(&conns)) || !conns) return out;
    while (true) {
        ComPtr<INetworkConnection> conn;
        ULONG got = 0;
        if (conns->Next(1, &conn, &got) != S_OK || !got) break;
        GUID adapter{};
        ComPtr<INetwork> net;
        BSTR name = nullptr;
        if (SUCCEEDED(conn->GetAdapterId(&adapter)) && SUCCEEDED(conn->GetNetwork(&net)) && net &&
            SUCCEEDED(net->GetName(&name)) && name) {
            wchar_t g[64] = {};
            StringFromGUID2(adapter, g, 64);
            std::string key = to_utf8(g);
            for (auto& ch : key) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
            out.names[key] = to_utf8(name);
        }
        if (name) SysFreeString(name);
    }
    return out;
}

struct DefaultRoutes {
    uint64_t best4 = 0, best6 = 0;  // LUIDs (0 none)
};

// The default route the stack would use per family: lowest route metric +
// interface metric among default routes on interfaces that are up.
DefaultRoutes default_routes(const std::map<uint64_t, bool>& up) {
    DefaultRoutes out;
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (GetIpForwardTable2(AF_UNSPEC, &table) != NO_ERROR || !table) return out;
    ULONG best_metric[2] = {ULONG_MAX, ULONG_MAX};
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2& r = table->Table[i];
        if (r.DestinationPrefix.PrefixLength != 0) continue;
        auto u = up.find(r.InterfaceLuid.Value);
        if (u == up.end() || !u->second) continue;
        ADDRESS_FAMILY fam = r.DestinationPrefix.Prefix.si_family;
        MIB_IPINTERFACE_ROW ifrow{};
        ifrow.Family = fam;
        ifrow.InterfaceLuid = r.InterfaceLuid;
        if (GetIpInterfaceEntry(&ifrow) != NO_ERROR || !ifrow.Connected) continue;
        ULONG metric = r.Metric + ifrow.Metric;
        int k = fam == AF_INET ? 0 : 1;
        if (metric < best_metric[k]) {
            best_metric[k] = metric;
            (k == 0 ? out.best4 : out.best6) = r.InterfaceLuid.Value;
        }
    }
    FreeMibTable(table);
    return out;
}

}  // namespace

NetworkState read_network_state(const std::vector<WifiInterface>& wifi) {
    NetworkState st;
    // The adapters ipconfig lists (no hidden filter / LWF stack layers).
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG size = 32 * 1024;
    std::vector<BYTE> buf;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && r == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &size);
    }
    NlmInfo nlm = read_nlm();
    std::map<uint64_t, bool> up;
    if (r == NO_ERROR) {
        for (auto* a = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()); a; a = a->Next) {
            NetDevice d;
            d.id = std::to_string(a->Luid.Value);
            d.interface_name = a->FriendlyName ? to_utf8(a->FriendlyName) : std::string();
            d.description = a->Description ? to_utf8(a->Description) : std::string();
            d.type = link_type(a->IfType);
            if (a->PhysicalAddressLength) d.mac = format_mac(a->PhysicalAddress, a->PhysicalAddressLength);
            uint64_t speed = a->TransmitLinkSpeed > a->ReceiveLinkSpeed ? a->TransmitLinkSpeed : a->ReceiveLinkSpeed;
            if (speed && speed != ~0ull) d.speed_mbps = speed / 1000000ull;
            switch (a->OperStatus) {
                case IfOperStatusUp: d.state = LinkState::Connected; break;
                case IfOperStatusDormant: d.state = LinkState::Connecting; break;
                case IfOperStatusDown:
                case IfOperStatusLowerLayerDown:
                    d.state = d.type == LinkType::WiFi ? LinkState::Disconnected : LinkState::Unavailable;
                    break;
                case IfOperStatusNotPresent: d.state = LinkState::Unavailable; break;
                default: d.state = LinkState::Unknown; break;
            }
            up[a->Luid.Value] = a->OperStatus == IfOperStatusUp;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                std::string text = addr_text(u->Address.lpSockaddr) + "/" + std::to_string(u->OnLinkPrefixLength);
                (u->Address.lpSockaddr->sa_family == AF_INET ? d.ipv4 : d.ipv6).addresses.push_back(text);
            }
            for (auto* g = a->FirstGatewayAddress; g; g = g->Next)
                (g->Address.lpSockaddr->sa_family == AF_INET ? d.ipv4 : d.ipv6).gateways.push_back(addr_text(g->Address.lpSockaddr));
            for (auto* s = a->FirstDnsServerAddress; s; s = s->Next)
                (s->Address.lpSockaddr->sa_family == AF_INET ? d.ipv4 : d.ipv6).dns.push_back(addr_text(s->Address.lpSockaddr));

            std::string guid = a->AdapterName ? a->AdapterName : "";
            for (auto& ch : guid) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
            if (d.state == LinkState::Connected) {
                auto n = nlm.names.find(guid);
                if (n != nlm.names.end()) d.connection = n->second;
            }
            for (auto& w : wifi)
                if (w.device_id == d.id && w.connected) d.connection = w.profile.empty() ? w.ssid : w.profile;
            st.devices.push_back(std::move(d));
        }
    }
    DefaultRoutes routes = default_routes(up);
    uint64_t primary = routes.best4 ? routes.best4 : routes.best6;
    for (auto& d : st.devices) {
        uint64_t luid = std::stoull(d.id);
        d.is_primary = primary && luid == primary;
        if (d.is_primary) st.primary_device = d.id;
        if (d.state != LinkState::Connected || d.type == LinkType::Loopback) continue;
        ActiveConnection c;
        c.id = d.id;
        c.name = d.connection.empty() ? d.interface_name : d.connection;
        c.type = d.type;
        c.state = d.state;
        c.device_ids = {d.id};
        c.default4 = routes.best4 == luid;
        c.default6 = routes.best6 == luid;
        st.active_connections.push_back(std::move(c));
    }

    auto hint = hint_connectivity();
    st.connectivity = hint ? *hint : nlm.connectivity.value_or(Connectivity::Unknown);
    st.networking_enabled = true;
    st.wifi_enabled = st.wifi_hardware_enabled = false;
    for (auto& w : wifi) {
        st.wifi_enabled |= w.software_on;
        st.wifi_hardware_enabled |= w.hardware_on;
    }
    return st;
}

}  // namespace brosys::win
