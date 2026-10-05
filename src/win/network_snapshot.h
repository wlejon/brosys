// One snapshot of Windows networking: adapters and their IP configuration
// (GetAdaptersAddresses), the primary interface from the default routes and
// interface metrics (as the stack chooses), connectivity (the OS
// connectivity hint, NLM as fallback), and network / Wi-Fi profile names.
#pragma once

#include "brosys/network.h"
#include "win/wifi.h"

#include <vector>

namespace brosys::win {

// Requires COM on the calling thread (NLM names / connectivity fallback).
NetworkState read_network_state(const std::vector<WifiInterface>& wifi);

}  // namespace brosys::win
