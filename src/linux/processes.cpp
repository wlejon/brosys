// Linux process table (/proc/[pid]/stat, status, cmdline) and signal control (::kill).
#include "brosys/processes.h"

#include <dirent.h>
#include <pwd.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace brosys {

namespace {

bool read_file_bytes(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string lookup_username(uint32_t uid, std::unordered_map<uint32_t, std::string>& cache) {
    auto it = cache.find(uid);
    if (it != cache.end()) return it->second;

    std::string name;
    if (uid == 0) {
        name = "root";
    } else {
        struct passwd pwd_buf{};
        struct passwd* result = nullptr;
        char buf[4096];
        if (::getpwuid_r(static_cast<uid_t>(uid), &pwd_buf, buf, sizeof(buf), &result) == 0 &&
            result && result->pw_name && result->pw_name[0] != '\0') {
            name = result->pw_name;
        } else {
            name = std::to_string(uid);
        }
    }
    cache.emplace(uid, name);
    return name;
}

bool parse_proc_stat(const std::string& text, uint64_t page_size, uint64_t clk_tck, ProcessInfo& info) {
    const size_t open_paren = text.find('(');
    const size_t close_paren = text.rfind(')');
    if (open_paren == std::string::npos || close_paren == std::string::npos || close_paren <= open_paren) {
        return false;
    }

    const std::string pid_str = text.substr(0, open_paren);
    char* end_ptr = nullptr;
    const long parsed_pid = std::strtol(pid_str.c_str(), &end_ptr, 10);
    if (end_ptr == pid_str.c_str() || parsed_pid <= 0) {
        return false;
    }

    info.pid = static_cast<int32_t>(parsed_pid);
    info.name = text.substr(open_paren + 1, close_paren - open_paren - 1);

    if (close_paren + 2 >= text.size()) return false;
    std::istringstream tail(text.substr(close_paren + 2));

    std::vector<std::string> fields;
    fields.reserve(32);
    std::string tok;
    while (tail >> tok && fields.size() < 25) {
        fields.push_back(std::move(tok));
    }
    if (fields.size() < 22) return false;

    // Indices relative to field 3 (state) in /proc/[pid]/stat:
    //  0: state (field 3)
    //  1: ppid (field 4)
    // 11: utime (field 14)
    // 12: stime (field 15)
    // 17: num_threads (field 20)
    // 20: vsize (field 23)
    // 21: rss (field 24)
    info.state = fields[0].empty() ? "S" : fields[0].substr(0, 1);
    info.ppid = static_cast<int32_t>(std::strtol(fields[1].c_str(), nullptr, 10));

    const uint64_t utime = std::strtoull(fields[11].c_str(), nullptr, 10);
    const uint64_t stime = std::strtoull(fields[12].c_str(), nullptr, 10);
    if (clk_tck > 0) {
        info.cpu_time_ms = ((utime + stime) * 1000ULL) / clk_tck;
    }

    const int32_t threads = static_cast<int32_t>(std::strtol(fields[17].c_str(), nullptr, 10));
    info.threads = threads > 0 ? threads : 1;

    info.vsz_bytes = std::strtoull(fields[20].c_str(), nullptr, 10);
    const int64_t rss_pages = std::strtoll(fields[21].c_str(), nullptr, 10);
    info.rss_bytes = rss_pages > 0 ? static_cast<uint64_t>(rss_pages) * page_size : 0;

    return true;
}

void parse_proc_status(const std::string& text, ProcessInfo& info) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        const char* val = line.c_str() + colon + 1;
        while (*val == ' ' || *val == '\t') ++val;

        if (key == "Uid") {
            info.uid = static_cast<uint32_t>(std::strtoul(val, nullptr, 10));
        } else if (key == "VmRSS" && info.rss_bytes == 0) {
            info.rss_bytes = std::strtoull(val, nullptr, 10) * 1024ULL;
        } else if (key == "VmSize" && info.vsz_bytes == 0) {
            info.vsz_bytes = std::strtoull(val, nullptr, 10) * 1024ULL;
        } else if (key == "Threads" && info.threads <= 0) {
            const int32_t t = static_cast<int32_t>(std::strtol(val, nullptr, 10));
            if (t > 0) info.threads = t;
        }
    }
}

std::string parse_proc_cmdline(const std::string& raw, const std::string& fallback_name) {
    if (raw.empty()) return fallback_name;
    std::string out;
    out.reserve(raw.size());
    bool prev_space = true;
    for (char c : raw) {
        if (c == '\0') {
            if (!prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
        } else {
            out.push_back(c);
            prev_space = false;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out.empty() ? fallback_name : out;
}

int to_native_signal(int32_t signal) {
    switch (signal) {
        case signal_num::SIGHUP_NUM:  return SIGHUP;
        case signal_num::SIGINT_NUM:  return SIGINT;
        case signal_num::SIGKILL_NUM: return SIGKILL;
        case signal_num::SIGTERM_NUM: return SIGTERM;
        case signal_num::SIGCONT_NUM: return SIGCONT;
        case signal_num::SIGSTOP_NUM: return SIGSTOP;
        default: return signal;
    }
}

}  // namespace

std::vector<ProcessInfo> list_processes() {
    std::vector<ProcessInfo> out;
    DIR* dir = ::opendir("/proc");
    if (!dir) return out;

    const long ps = ::sysconf(_SC_PAGESIZE);
    const uint64_t page_size = ps > 0 ? static_cast<uint64_t>(ps) : 4096ULL;
    const long tck = ::sysconf(_SC_CLK_TCK);
    const uint64_t clk_tck = tck > 0 ? static_cast<uint64_t>(tck) : 100ULL;

    std::unordered_map<uint32_t, std::string> uid_cache;
    uid_cache.emplace(0, "root");

    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        const char* dname = entry->d_name;
        if (dname[0] < '1' || dname[0] > '9') continue;
        bool numeric = true;
        for (const char* p = dname; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                numeric = false;
                break;
            }
        }
        if (!numeric) continue;

        const std::string pid_dir = std::string("/proc/") + dname;
        std::string stat_text;
        if (!read_file_bytes(pid_dir + "/stat", stat_text)) continue;

        ProcessInfo info;
        if (!parse_proc_stat(stat_text, page_size, clk_tck, info)) continue;

        std::string status_text;
        if (read_file_bytes(pid_dir + "/status", status_text)) {
            parse_proc_status(status_text, info);
        }
        info.user = lookup_username(info.uid, uid_cache);

        std::string cmd_raw;
        read_file_bytes(pid_dir + "/cmdline", cmd_raw);
        info.cmdline = parse_proc_cmdline(cmd_raw, info.name);

        out.push_back(std::move(info));
    }
    ::closedir(dir);

    std::sort(out.begin(), out.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        return a.pid < b.pid;
    });
    return out;
}

Result kill_process(int32_t pid, int32_t signal) {
    if (pid <= 1) {
        return Result::failure("refusing to signal pid " + std::to_string(pid));
    }
    if (signal <= 0 || signal > 64) {
        return Result::failure("invalid signal " + std::to_string(signal));
    }
    const int native_sig = to_native_signal(signal);
    if (::kill(static_cast<pid_t>(pid), native_sig) != 0) {
        return Result::failure(std::strerror(errno));
    }
    return Result::success();
}

}  // namespace brosys
