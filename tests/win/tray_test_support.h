// Test support for the Windows shell tests: a private desktop, a client
// process on it speaking a line protocol, and a probe window on the user's
// desktop.
#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bstest::win {

inline std::wstring exe_dir() {
    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring p(path, n);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

// A desktop of our own on the interactive window station. Never switched to,
// so the user's desktop keeps input and nothing on it changes.
class PrivateDesktop {
public:
    PrivateDesktop() {
        name_ = L"brosys-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        handle_ = CreateDesktopW(name_.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        error_ = handle_ ? 0 : GetLastError();
    }
    ~PrivateDesktop() {
        if (handle_) CloseDesktop(handle_);
    }
    PrivateDesktop(const PrivateDesktop&) = delete;
    PrivateDesktop& operator=(const PrivateDesktop&) = delete;

    HDESK handle() const { return handle_; }
    DWORD error() const { return error_; }
    const std::wstring& name() const { return name_; }
    std::wstring station_path() const { return L"WinSta0\\" + name_; }

    // Runs fn on a fresh thread attached to this desktop (before it creates
    // any window) and waits for it.
    template <class F>
    bool run_on(F&& fn) const {
        bool attached = false;
        std::thread t([&] {
            attached = SetThreadDesktop(handle_) != FALSE;
            if (attached) fn();
        });
        t.join();
        return attached;
    }

private:
    std::wstring name_;
    HDESK handle_ = nullptr;
    DWORD error_ = 0;
};

// A child process whose stdout lines are collected and whose stdin takes
// commands.
class LineProcess {
public:
    ~LineProcess() { stop(); }

    bool start(const std::wstring& exe, const std::wstring& desktop, std::string* error) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE out_read = nullptr, out_write = nullptr, in_read = nullptr, in_write = nullptr;
        if (!CreatePipe(&out_read, &out_write, &sa, 0) || !CreatePipe(&in_read, &in_write, &sa, 0)) {
            if (error) *error = "CreatePipe failed";
            return false;
        }
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        std::wstring desk = desktop;
        si.lpDesktop = desk.empty() ? nullptr : desk.data();
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_read;
        si.hStdOutput = out_write;
        si.hStdError = out_write;
        std::wstring cmd = L"\"" + exe + L"\"";
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &si, &pi);
        DWORD err = GetLastError();
        CloseHandle(out_write);
        CloseHandle(in_read);
        if (!ok) {
            CloseHandle(out_read);
            CloseHandle(in_write);
            if (error) *error = "CreateProcess failed (" + std::to_string(err) + ")";
            return false;
        }
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        pid_ = pi.dwProcessId;
        stdin_ = in_write;
        stdout_ = out_read;
        reader_ = std::thread([this] { read_loop(); });
        return true;
    }

    DWORD pid() const { return pid_; }

    void send(const std::string& line) {
        std::string l = line + "\n";
        DWORD written = 0;
        WriteFile(stdin_, l.data(), static_cast<DWORD>(l.size()), &written, nullptr);
    }

    size_t mark() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_.size();
    }

    std::vector<std::string> since(size_t from) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (from >= lines_.size()) return {};
        return std::vector<std::string>(lines_.begin() + static_cast<ptrdiff_t>(from), lines_.end());
    }

    // First line at index >= from matching pred; *index gets its position.
    std::optional<std::string> wait(const std::function<bool(const std::string&)>& pred, size_t from,
                                    std::chrono::milliseconds timeout, size_t* index = nullptr) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto deadline = std::chrono::steady_clock::now() + timeout;
        size_t i = from;
        while (true) {
            for (; i < lines_.size(); ++i) {
                if (pred(lines_[i])) {
                    if (index) *index = i;
                    return lines_[i];
                }
            }
            if (eof_) return std::nullopt;
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout && i >= lines_.size())
                return std::nullopt;
        }
    }

    std::optional<std::string> wait_prefix(const std::string& prefix, size_t from,
                                           std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        return wait([&](const std::string& l) { return l.rfind(prefix, 0) == 0; }, from, timeout);
    }

    // Sends a command and returns its result ("1", "0", ...), "" on timeout.
    std::string command(const std::string& line) {
        size_t from = mark();
        send(line);
        std::string cmd = line.substr(0, line.find(' '));
        auto r = wait_prefix("ok " + cmd + " ", from);
        return r ? r->substr(4 + cmd.size()) : std::string();
    }

    void stop() {
        if (process_) {
            send("quit");
            if (WaitForSingleObject(process_, 5000) != WAIT_OBJECT_0) TerminateProcess(process_, 1);
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (stdin_) {
            CloseHandle(stdin_);
            stdin_ = nullptr;
        }
        if (reader_.joinable()) reader_.join();
        if (stdout_) {
            CloseHandle(stdout_);
            stdout_ = nullptr;
        }
    }

    // Kills the process without letting it clean up.
    void kill() {
        if (process_) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 5000);
        }
    }

private:
    void read_loop() {
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(stdout_, buf, sizeof buf, &n, nullptr) && n) {
            pending.append(buf, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pending.erase(0, nl + 1);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lines_.push_back(line);
                }
                if (std::getenv("BROSYS_TEST_VERBOSE")) std::printf("  client> %s\n", line.c_str());
                cv_.notify_all();
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            eof_ = true;
        }
        cv_.notify_all();
    }

    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    HANDLE stdin_ = nullptr;
    HANDLE stdout_ = nullptr;
    std::thread reader_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> lines_;
    bool eof_ = false;
};

// A hidden top-level window on the calling process's default desktop that
// counts "TaskbarCreated" messages: proof that an announcement stayed scoped.
class TaskbarCreatedProbe {
public:
    TaskbarCreatedProbe() {
        std::mutex m;
        std::condition_variable cv;
        bool ready = false;
        thread_ = std::thread([&] {
            msg_ = RegisterWindowMessageW(L"TaskbarCreated");
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                auto* self = reinterpret_cast<TaskbarCreatedProbe*>(GetWindowLongPtrW(h, GWLP_USERDATA));
                if (self && msg == self->msg_) {
                    ++self->count_;
                    return 0;
                }
                return DefWindowProcW(h, msg, wp, lp);
            };
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"brosys_taskbar_probe";
            RegisterClassExW(&wc);
            hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                    wc.hInstance, nullptr);
            if (hwnd_) SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            {
                std::lock_guard<std::mutex> lock(m);
                ready = true;
            }
            cv.notify_all();
            MSG mm;
            while (GetMessageW(&mm, nullptr, 0, 0) > 0) DispatchMessageW(&mm);
            if (hwnd_) DestroyWindow(hwnd_);
        });
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return ready; });
        thread_id_ = GetThreadId(thread_.native_handle());
    }
    ~TaskbarCreatedProbe() {
        PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
        thread_.join();
    }
    bool ok() const { return hwnd_ != nullptr; }
    int count() const { return count_.load(); }

private:
    std::thread thread_;
    DWORD thread_id_ = 0;
    HWND hwnd_ = nullptr;
    UINT msg_ = 0;
    std::atomic<int> count_{0};
};

}  // namespace bstest::win
