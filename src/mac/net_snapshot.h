// macOS network snapshot: SystemConfiguration (hardware ports, services,
// the dynamic store's State:/Network keys) plus getifaddrs, combined with
// CoreWLAN's view of the Wi-Fi interfaces.
#pragma once

#include "brosys/network.h"
#include "mac/net_os.h"

#include <vector>

namespace brosys::mac {

NetworkState read_network_state(const std::vector<WifiInterfaceInfo>& wifi, Connectivity connectivity);

// SCDynamicStore key patterns whose change can alter the snapshot.
std::vector<std::string> network_watch_patterns();

}  // namespace brosys::mac
