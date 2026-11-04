// Windows disk and network counters: IOCTL_DISK_PERFORMANCE on each
// \\.\PhysicalDriveN, GetIfTable2's interface rows. Neither needs elevation.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <winioctl.h>

#include "brosys/io_counters.h"
#include "win/util.h"

#include <string>

namespace brosys {

namespace {

constexpr int kMaxDrives = 64;
constexpr int kMaxMissesInARow = 8;   // drive numbers can have gaps (a removed USB disk)

void read_disks(std::vector<DiskCounters>& out) {
    int misses = 0;
    for (int i = 0; i < kMaxDrives && misses < kMaxMissesInARow; ++i) {
        const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(i);
        // No access rights: enough for the performance query, and allowed to any user.
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            ++misses;
            continue;
        }
        misses = 0;
        DISK_PERFORMANCE perf{};
        DWORD got = 0;
        const BOOL ok = DeviceIoControl(h, IOCTL_DISK_PERFORMANCE, nullptr, 0, &perf, sizeof(perf), &got, nullptr);
        CloseHandle(h);
        if (!ok || got < sizeof(perf)) continue;
        DiskCounters d;
        d.name = "PhysicalDrive" + std::to_string(i);
        d.read_bytes = static_cast<uint64_t>(perf.BytesRead.QuadPart);
        d.write_bytes = static_cast<uint64_t>(perf.BytesWritten.QuadPart);
        d.reads = perf.ReadCount;
        d.writes = perf.WriteCount;
        out.push_back(std::move(d));
    }
}

void read_interfaces(std::vector<NetCounters>& out) {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        // Filter drivers (WFP, QoS, ...) repeat the adapter beneath them.
        if (row.InterfaceAndOperStatusFlags.FilterInterface) continue;
        if (row.OperStatus != IfOperStatusUp) continue;
        NetCounters n;
        n.name = win::to_utf8(row.Alias);
        if (n.name.empty()) n.name = win::to_utf8(row.Description);
        n.loopback = row.Type == IF_TYPE_SOFTWARE_LOOPBACK;
        n.rx_bytes = row.InOctets;
        n.tx_bytes = row.OutOctets;
        n.rx_packets = row.InUcastPkts + row.InNUcastPkts;
        n.tx_packets = row.OutUcastPkts + row.OutNUcastPkts;
        out.push_back(std::move(n));
    }
    FreeMibTable(table);
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
