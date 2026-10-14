// macOS process table (libproc proc_listallpids / proc_pidinfo) and signal control (::kill).
#include "brosys/processes.h"

#include <libproc.h>
#include <pwd.h>
#include <signal.h>
#include <sys/proc_info.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace brosys {

namespace {

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

const char* map_bsd_status(uint32_t status) {
    switch (status) {
        case 1: return "I";  // SIDL
        case 2: return "R";  // SRUN
        case 3: return "S";  // SSLEEP
        case 4: return "T";  // SSTOP
        case 5: return "Z";  // SZOMB
        default: return "S";
    }
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
    const int est = ::proc_listallpids(nullptr, 0);
    if (est <= 0) return out;

    std::vector<pid_t> pids(static_cast<size_t>(est) * 2);
    const int bytes = ::proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (bytes <= 0) return out;
    const size_t count = static_cast<size_t>(bytes) / sizeof(pid_t);

    std::unordered_map<uint32_t, std::string> uid_cache;
    uid_cache.emplace(0, "root");

    for (size_t i = 0; i < count; ++i) {
        const pid_t pid = pids[i];
        if (pid <= 0) continue;

        struct proc_bsdinfo bsd{};
        if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof(bsd)) != sizeof(bsd)) {
            continue;
        }

        ProcessInfo info;
        info.pid = static_cast<int32_t>(pid);
        info.ppid = static_cast<int32_t>(bsd.pbi_ppid);
        info.uid = bsd.pbi_uid;
        info.user = lookup_username(info.uid, uid_cache);
        info.name = bsd.pbi_name[0] != '\0' ? bsd.pbi_name : bsd.pbi_comm;
        info.state = map_bsd_status(bsd.pbi_status);

        char path_buf[PROC_PIDPATHINFO_MAXSIZE] = {};
        if (::proc_pidpath(pid, path_buf, sizeof(path_buf)) > 0 && path_buf[0] != '\0') {
            info.cmdline = path_buf;
        } else {
            info.cmdline = info.name;
        }

        struct proc_taskinfo ti{};
        if (::proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) == sizeof(ti)) {
            info.rss_bytes = ti.pti_resident_size;
            info.vsz_bytes = ti.pti_virtual_size;
            info.threads = ti.pti_threadnum > 0 ? ti.pti_threadnum : 1;
            info.cpu_time_ms = (ti.pti_total_user + ti.pti_total_system) / 1000000ULL;
        }

        out.push_back(std::move(info));
    }

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
