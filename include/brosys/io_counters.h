// Machine-wide disk and network byte counters: cumulative since boot (or
// since the device appeared), read for a rate by taking two snapshots and
// dividing the difference by the difference of their timestamps.
//
// Linux: /proc/diskstats (whole physical disks: a name in /sys/block with a
// `device` link, so partitions, loop, dm and zram devices are left out) and
// /proc/net/dev (every interface; `loopback` from /sys/class/net/<if>/type).
// Windows: IOCTL_DISK_PERFORMANCE on each \\.\PhysicalDriveN (opened with no
// access rights, so no administrator is needed) and GetIfTable2 (filter
// interfaces left out, and interfaces that are not up).
// macOS: IOBlockStorageDriver's "Statistics" (named by its media's BSD name)
// and the interfaces that are up from sysctl NET_RT_IFLIST2, their counters
// from the interface MIB (IFDATA_GENERAL: exact, where the route message's
// are rounded to 1 KiB for an unprivileged reader).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace brosys {

struct DiskCounters {
    std::string name;          // "sda", "nvme0n1"; "PhysicalDrive0"; "disk0"
    uint64_t read_bytes = 0;
    uint64_t write_bytes = 0;
    uint64_t reads = 0;        // completed operations
    uint64_t writes = 0;

    bool operator==(const DiskCounters&) const = default;
};

struct NetCounters {
    std::string name;          // "eth0", "wlp3s0"; "Ethernet", "Wi-Fi"; "en0"
    bool loopback = false;
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
    uint64_t rx_packets = 0;
    uint64_t tx_packets = 0;

    bool operator==(const NetCounters&) const = default;
};

struct IoCounters {
    double timestamp_ms = 0;   // monotonic (steady clock), when the counters were read
    std::vector<DiskCounters> disks;
    std::vector<NetCounters> interfaces;
};

// The counters now. A source that cannot be read leaves its list empty.
IoCounters read_io_counters();

// The steady clock in milliseconds (IoCounters::timestamp_ms's clock).
double monotonic_ms();

// The Linux files' parsers (platform-neutral, so every platform tests them).
// parse_diskstats keeps every device line, partitions included: the backend
// filters. Sectors are 512 bytes whatever the device's sector size.
std::vector<DiskCounters> parse_diskstats(std::string_view text);
std::vector<NetCounters> parse_net_dev(std::string_view text);

}  // namespace brosys
