// Windows process table (Toolhelp32 + GetProcessMemoryInfo + GetProcessTimes)
// and process termination (WM_CLOSE for graceful SIGTERM=15, TerminateProcess for SIGKILL=9).
#include "brosys/processes.h"
#include "win/util.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace brosys {

namespace {

uint64_t filetime_to_u64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime);
}

std::string query_process_user(HANDLE proc) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(proc, TOKEN_QUERY, &token)) return {};

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        return {};
    }

    std::vector<uint8_t> buf(needed);
    std::string out;
    if (GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
        const auto* tu = reinterpret_cast<const TOKEN_USER*>(buf.data());
        wchar_t name[256] = {};
        wchar_t domain[256] = {};
        DWORD name_len = 256;
        DWORD domain_len = 256;
        SID_NAME_USE use = SidTypeUnknown;
        if (LookupAccountSidW(nullptr, tu->User.Sid, name, &name_len, domain, &domain_len, &use)) {
            out = win::to_utf8(name);
        }
    }
    CloseHandle(token);
    return out;
}

struct CloseWindowsCtx {
    DWORD pid = 0;
    int posted = 0;
};

BOOL CALLBACK close_windows_proc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<CloseWindowsCtx*>(lparam);
    DWORD win_pid = 0;
    GetWindowThreadProcessId(hwnd, &win_pid);
    if (win_pid == ctx->pid && GetWindow(hwnd, GW_OWNER) == nullptr) {
        if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
            ctx->posted++;
        }
    }
    return TRUE;
}

}  // namespace

std::vector<ProcessInfo> list_processes() {
    std::vector<ProcessInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0) continue;

            ProcessInfo info;
            info.pid = static_cast<int32_t>(pe.th32ProcessID);
            info.ppid = static_cast<int32_t>(pe.th32ParentProcessID);
            info.threads = pe.cntThreads > 0 ? static_cast<int32_t>(pe.cntThreads) : 1;
            info.name = win::to_utf8(pe.szExeFile);
            info.cmdline = info.name;
            info.state = "R";

            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (h) {
                PROCESS_MEMORY_COUNTERS pmc{};
                pmc.cb = sizeof(pmc);
                if (K32GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
                    info.rss_bytes = static_cast<uint64_t>(pmc.WorkingSetSize);
                    info.vsz_bytes = static_cast<uint64_t>(pmc.PagefileUsage);
                }

                FILETIME create_time{}, exit_time{}, kernel_time{}, user_time{};
                if (GetProcessTimes(h, &create_time, &exit_time, &kernel_time, &user_time)) {
                    const uint64_t total_100ns = filetime_to_u64(kernel_time) + filetime_to_u64(user_time);
                    info.cpu_time_ms = total_100ns / 10000ULL;
                }

                wchar_t path_buf[MAX_PATH * 2] = {};
                DWORD path_len = static_cast<DWORD>(sizeof(path_buf) / sizeof(wchar_t));
                if (QueryFullProcessImageNameW(h, 0, path_buf, &path_len) && path_len > 0) {
                    info.cmdline = win::to_utf8(std::wstring_view(path_buf, path_len));
                }

                info.user = query_process_user(h);
                CloseHandle(h);
            }

            out.push_back(std::move(info));
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    std::sort(out.begin(), out.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        return a.pid < b.pid;
    });
    return out;
}

Result kill_process(int32_t pid, int32_t signal) {
    if (pid <= 4) {
        return Result::failure("refusing to terminate system pid " + std::to_string(pid));
    }
    if (signal <= 0 || signal > 64) {
        return Result::failure("invalid signal " + std::to_string(signal));
    }

    const DWORD target_pid = static_cast<DWORD>(pid);

    if (signal == signal_num::SIGTERM_NUM) {
        CloseWindowsCtx ctx{target_pid, 0};
        EnumWindows(&close_windows_proc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.posted > 0) {
            return Result::success();
        }
    }

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, target_pid);
    if (!h) {
        return Result::failure(win::win32_error("OpenProcess", GetLastError()));
    }
    const BOOL ok = TerminateProcess(h, 1);
    const DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) {
        return Result::failure(win::win32_error("TerminateProcess", err));
    }
    return Result::success();
}

}  // namespace brosys
