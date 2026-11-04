// Linux disk and network counters: /proc/diskstats (whole physical disks
// only) and /proc/net/dev.
#include "brosys/io_counters.h"

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace brosys {

namespace {

bool read_text(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

// A whole disk backed by hardware: /sys/block/<name> is a whole block device
// (a partition is not listed there), and `device` is missing for loop, dm-*,
// zram, md and other virtual devices. A slash in the name is '!' in sysfs.
bool physical_disk(std::string name) {
    for (auto& c : name)
        if (c == '/') c = '!';
    return exists("/sys/block/" + name + "/device");
}

bool loopback(const std::string& name) {
    if (name == "lo") return true;
    std::string type;
    if (!read_text("/sys/class/net/" + name + "/type", type)) return false;
    return std::atoi(type.c_str()) == 772;   // ARPHRD_LOOPBACK
}

}  // namespace

IoCounters read_io_counters() {
    IoCounters c;
    std::string text;
    if (read_text("/proc/diskstats", text)) {
        for (auto& d : parse_diskstats(text))
            if (physical_disk(d.name)) c.disks.push_back(std::move(d));
    }
    if (read_text("/proc/net/dev", text)) {
        c.interfaces = parse_net_dev(text);
        for (auto& n : c.interfaces) n.loopback = loopback(n.name);
    }
    c.timestamp_ms = monotonic_ms();
    return c;
}

}  // namespace brosys
