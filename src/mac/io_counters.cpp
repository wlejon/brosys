// macOS disk and network counters: each IOBlockStorageDriver's "Statistics"
// dictionary (named by the BSD name of the media beneath it), and sysctl
// NET_RT_IFLIST2's if_msghdr2, whose if_data64 counters do not wrap at 4 GiB.
#include "brosys/io_counters.h"
#include "mac/cf.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/storage/IOMedia.h>
#include <net/if.h>
#include <net/if_mib.h>
#include <net/if_types.h>
#include <net/route.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <string>
#include <vector>

namespace brosys {

namespace {

uint64_t stat_value(CFDictionaryRef stats, const char* key) {
    mac::CFRef<CFStringRef> k(CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8));
    const auto v = mac::dict_int(stats, k.get());
    return v && *v > 0 ? static_cast<uint64_t>(*v) : 0;
}

void read_disks(std::vector<DiskCounters>& out) {
    io_iterator_t it = 0;
    // MACH_PORT_NULL is the default main port on every macOS version.
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(kIOBlockStorageDriverClass), &it) != KERN_SUCCESS)
        return;
    while (io_registry_entry_t driver = IOIteratorNext(it)) {
        mac::CFRef<CFTypeRef> statsRef(IORegistryEntryCreateCFProperty(
            driver, CFSTR(kIOBlockStorageDriverStatisticsKey), kCFAllocatorDefault, 0));
        std::string name;
        io_registry_entry_t media = 0;
        if (IORegistryEntryGetChildEntry(driver, kIOServicePlane, &media) == KERN_SUCCESS) {
            mac::CFRef<CFTypeRef> bsd(IORegistryEntryCreateCFProperty(media, CFSTR("BSD Name"), kCFAllocatorDefault, 0));
            name = mac::to_utf8(bsd.get());
            IOObjectRelease(media);
        }
        IOObjectRelease(driver);
        if (!statsRef || CFGetTypeID(statsRef.get()) != CFDictionaryGetTypeID() || name.empty()) continue;
        const auto stats = static_cast<CFDictionaryRef>(statsRef.get());
        DiskCounters d;
        d.name = std::move(name);
        d.read_bytes = stat_value(stats, kIOBlockStorageDriverStatisticsBytesReadKey);
        d.write_bytes = stat_value(stats, kIOBlockStorageDriverStatisticsBytesWrittenKey);
        d.reads = stat_value(stats, kIOBlockStorageDriverStatisticsReadsKey);
        d.writes = stat_value(stats, kIOBlockStorageDriverStatisticsWritesKey);
        out.push_back(std::move(d));
    }
    IOObjectRelease(it);
}

void read_interfaces(std::vector<NetCounters>& out) {
    int mib[6] = {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0};
    size_t len = 0;
    if (sysctl(mib, 6, nullptr, &len, nullptr, 0) != 0 || len == 0) return;
    std::vector<char> buf(len + len / 8);   // room for an interface added between the two calls
    len = buf.size();
    if (sysctl(mib, 6, buf.data(), &len, nullptr, 0) != 0) return;
    for (size_t at = 0; at + sizeof(if_msghdr) <= len;) {
        const auto* hdr = reinterpret_cast<const if_msghdr*>(buf.data() + at);
        if (hdr->ifm_msglen == 0) break;
        if (hdr->ifm_type == RTM_IFINFO2 && at + sizeof(if_msghdr2) <= len) {
            const auto* m = reinterpret_cast<const if_msghdr2*>(hdr);
            char ifname[IF_NAMESIZE] = {};
            if ((m->ifm_flags & IFF_UP) && if_indextoname(m->ifm_index, ifname)) {
                NetCounters n;
                n.name = ifname;
                n.loopback = (m->ifm_flags & IFF_LOOPBACK) != 0;
                // The route message's counters are rounded down to 1 KiB for an
                // unprivileged reader; the interface MIB's are exact.
                ifmibdata mib{};
                size_t mibLen = sizeof(mib);
                int q[6] = {CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA, static_cast<int>(m->ifm_index), IFDATA_GENERAL};
                const bool exact = sysctl(q, 6, &mib, &mibLen, nullptr, 0) == 0 && mibLen >= sizeof(mib);
                n.rx_bytes = exact ? mib.ifmd_data.ifi_ibytes : m->ifm_data.ifi_ibytes;
                n.tx_bytes = exact ? mib.ifmd_data.ifi_obytes : m->ifm_data.ifi_obytes;
                n.rx_packets = exact ? mib.ifmd_data.ifi_ipackets : m->ifm_data.ifi_ipackets;
                n.tx_packets = exact ? mib.ifmd_data.ifi_opackets : m->ifm_data.ifi_opackets;
                out.push_back(std::move(n));
            }
        }
        at += hdr->ifm_msglen;
    }
}

}  // namespace

IoCounters read_io_counters() {
    IoCounters c;
    read_disks(c.disks);
    read_interfaces(c.interfaces);
    c.timestamp_ms = monotonic_ms();
    return c;
}

}  // namespace brosys
