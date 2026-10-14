// Process snapshot and signal/termination control.
//
// Linux: /proc/[pid]/stat, /proc/[pid]/status, /proc/[pid]/cmdline, and ::kill.
// Windows: CreateToolhelp32Snapshot + OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)
// + K32GetProcessMemoryInfo + GetProcessTimes; WM_CLOSE on top-level windows for
// graceful termination (SIGTERM=15) and TerminateProcess for forceful (SIGKILL=9).
// macOS: proc_listallpids + proc_pidinfo (PROC_PIDTBSDINFO / PROC_PIDTASKINFO)
// and ::kill.
#pragma once

#include "brosys/common.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace brosys {

struct ProcessInfo {
    int32_t pid = 0;
    int32_t ppid = 0;
    std::string name;
    std::string cmdline;
    std::string state;       // "R", "S", "D", "Z", "T", "I", etc.
    std::string user;
    uint32_t uid = 0;
    uint64_t rss_bytes = 0;
    uint64_t vsz_bytes = 0;
    uint64_t cpu_time_ms = 0;
    int32_t threads = 1;

    bool operator==(const ProcessInfo&) const = default;
};

namespace signal_num {
inline constexpr int32_t SIGHUP_NUM  = 1;
inline constexpr int32_t SIGINT_NUM  = 2;
inline constexpr int32_t SIGKILL_NUM = 9;
inline constexpr int32_t SIGTERM_NUM = 15;
inline constexpr int32_t SIGCONT_NUM = 18;
inline constexpr int32_t SIGSTOP_NUM = 19;
}  // namespace signal_num

// Parse a signal name ("SIGTERM", "TERM", "SIGKILL", "KILL", "SIGINT", "INT",
// "SIGHUP", "HUP", "SIGSTOP", "STOP", "SIGCONT", "CONT", or a positive integer
// string) into a standard signal number. Returns 0 if unrecognized.
int32_t parse_signal(std::string_view name);

// Enumerate running processes on the system.
std::vector<ProcessInfo> list_processes();

// Send a signal to or terminate a process.
// Validates pid (> 1 on POSIX, > 4 on Windows). Default signal is 15 (SIGTERM).
Result kill_process(int32_t pid, int32_t signal = signal_num::SIGTERM_NUM);

class ProcessService {
public:
    static std::unique_ptr<ProcessService> create(std::string* error = nullptr);
    virtual ~ProcessService() = default;

    virtual std::vector<ProcessInfo> list() const;
    virtual Result kill(int32_t pid, int32_t signal = signal_num::SIGTERM_NUM);
};

}  // namespace brosys
