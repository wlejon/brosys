// Disk and network counters: the /proc parsers over fixed text, and a live
// read on this machine (two snapshots: counters never go back, time goes on).
#include "check.h"

#include "brosys/io_counters.h"

#include <algorithm>
#include <cstdio>
#include <thread>

using namespace brosys;

static void test_parse_diskstats() {
    const char* text =
        "   8       0 sda 1000 10 20480 300 500 5 8192 200 0 400 500 0 0 0 0\n"
        "   8       1 sda1 900 0 18000 280 400 0 6000 150 0 300 430\n"
        " 259       0 nvme0n1 7 0 16 1 3 0 24 1 0 2 2\n"
        "short line\n"
        "   7       0 loop0 x 0 y 0 0 0 0 0 0 0 0\n";
    const auto d = parse_diskstats(text);
    REQUIRE(d.size() == 4);
    CHECK_EQ(d[0].name, std::string("sda"));
    CHECK_EQ(d[0].reads, 1000u);
    CHECK_EQ(d[0].read_bytes, 20480u * 512u);
    CHECK_EQ(d[0].writes, 500u);
    CHECK_EQ(d[0].write_bytes, 8192u * 512u);
    CHECK_EQ(d[1].name, std::string("sda1"));      // the backend filters partitions, not the parser
    CHECK_EQ(d[2].name, std::string("nvme0n1"));
    CHECK_EQ(d[2].write_bytes, 24u * 512u);
    CHECK_EQ(d[3].reads, 0u);                      // a field that is not a number reads 0
    CHECK(parse_diskstats("").empty());
}

static void test_parse_net_dev() {
    const char* text =
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo:  123456     789    0    0    0     0          0         0   123456     789    0    0    0     0       0          0\n"
        "  eth0:9876543210 6543210 0 0 0 0 0 0 1234567890 1234567 0 0 0 0 0 0\n"
        "wlan0: 1 2 3\n";
    const auto n = parse_net_dev(text);
    REQUIRE(n.size() == 2);
    CHECK_EQ(n[0].name, std::string("lo"));
    CHECK(n[0].loopback);
    CHECK_EQ(n[0].rx_bytes, 123456u);
    CHECK_EQ(n[0].tx_packets, 789u);
    CHECK_EQ(n[1].name, std::string("eth0"));       // no space after the colon, past 32 bits
    CHECK(!n[1].loopback);
    CHECK_EQ(n[1].rx_bytes, 9876543210ull);
    CHECK_EQ(n[1].rx_packets, 6543210u);
    CHECK_EQ(n[1].tx_bytes, 1234567890u);
    CHECK_EQ(n[1].tx_packets, 1234567u);
}

static void test_live() {
    const IoCounters a = read_io_counters();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const IoCounters b = read_io_counters();
    CHECK(a.timestamp_ms > 0);
    CHECK(b.timestamp_ms > a.timestamp_ms);
    std::printf("  %zu disk(s), %zu interface(s)\n", b.disks.size(), b.interfaces.size());
    for (const auto& d : b.disks)
        std::printf("    disk %s: read %llu B (%llu ops), written %llu B (%llu ops)\n", d.name.c_str(),
                    static_cast<unsigned long long>(d.read_bytes), static_cast<unsigned long long>(d.reads),
                    static_cast<unsigned long long>(d.write_bytes), static_cast<unsigned long long>(d.writes));
    for (const auto& n : b.interfaces)
        std::printf("    net %s%s: rx %llu B, tx %llu B\n", n.name.c_str(), n.loopback ? " (loopback)" : "",
                    static_cast<unsigned long long>(n.rx_bytes), static_cast<unsigned long long>(n.tx_bytes));
#if defined(_WIN32) || defined(__APPLE__)
    // Every Windows or macOS machine boots from a disk and has an interface up.
    CHECK(!b.disks.empty());
    CHECK(!b.interfaces.empty());
#else
    CHECK(!b.interfaces.empty());   // lo, at least (a container may show no physical disk)
#endif
    for (const auto& d : b.disks) {
        CHECK(!d.name.empty());
        auto prev = std::find_if(a.disks.begin(), a.disks.end(), [&](const DiskCounters& x) { return x.name == d.name; });
        if (prev == a.disks.end()) continue;
        CHECK(d.read_bytes >= prev->read_bytes);
        CHECK(d.write_bytes >= prev->write_bytes);
        CHECK(d.reads >= prev->reads);
    }
    bool anyDiskRead = false;
    for (const auto& d : b.disks) anyDiskRead = anyDiskRead || d.read_bytes > 0;
    if (!b.disks.empty()) CHECK(anyDiskRead);   // the system booted from one of them
    for (const auto& n : b.interfaces) {
        CHECK(!n.name.empty());
        auto prev = std::find_if(a.interfaces.begin(), a.interfaces.end(), [&](const NetCounters& x) { return x.name == n.name; });
        if (prev == a.interfaces.end()) continue;
        CHECK(n.rx_bytes >= prev->rx_bytes);
        CHECK(n.tx_bytes >= prev->tx_bytes);
    }
}

int main() {
    test_parse_diskstats();
    test_parse_net_dev();
    test_live();
    return bstest::finish("test_io_counters");
}
