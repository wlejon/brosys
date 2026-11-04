// /proc/diskstats and /proc/net/dev parsers, and the counters' clock.
#include "brosys/io_counters.h"

#include <chrono>
#include <cstdlib>

namespace brosys {

namespace {

// Whitespace-separated fields of one line.
std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

// A decimal counter; anything else is 0.
uint64_t number(std::string_view s) {
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    return v;
}

template <class Fn>
void each_line(std::string_view text, Fn&& fn) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        fn(text.substr(start, end - start));
        start = end + 1;
    }
}

}  // namespace

double monotonic_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// major minor name reads merged sectors_read ms writes merged sectors_written ...
std::vector<DiskCounters> parse_diskstats(std::string_view text) {
    std::vector<DiskCounters> out;
    each_line(text, [&](std::string_view line) {
        const auto f = fields(line);
        if (f.size() < 10) return;
        DiskCounters d;
        d.name = std::string(f[2]);
        d.reads = number(f[3]);
        d.read_bytes = number(f[5]) * 512;
        d.writes = number(f[7]);
        d.write_bytes = number(f[9]) * 512;
        out.push_back(std::move(d));
    });
    return out;
}

// "  eth0: rx_bytes rx_packets errs drop fifo frame compressed multicast
//          tx_bytes tx_packets ..." after two header lines.
std::vector<NetCounters> parse_net_dev(std::string_view text) {
    std::vector<NetCounters> out;
    each_line(text, [&](std::string_view line) {
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) return;
        const auto name = fields(line.substr(0, colon));
        if (name.size() != 1) return;
        const auto f = fields(line.substr(colon + 1));
        if (f.size() < 10) return;
        NetCounters n;
        n.name = std::string(name[0]);
        n.loopback = n.name == "lo";
        n.rx_bytes = number(f[0]);
        n.rx_packets = number(f[1]);
        n.tx_bytes = number(f[8]);
        n.tx_packets = number(f[9]);
        out.push_back(std::move(n));
    });
    return out;
}

}  // namespace brosys
