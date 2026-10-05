// Child processes for the Linux tests: run a real client to completion and
// capture its output, or keep a daemon running for the test's lifetime.
#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include <sys/types.h>

namespace bstest {

struct RunResult {
    int exit_code = -1;  // -1: killed by timeout or signal
    bool timed_out = false;
    std::string out;
    std::string err;
};

// Extra environment for children: name -> value ("" value unsets).
using Env = std::map<std::string, std::string>;

// Runs argv[0] (searched in PATH) and waits; SIGKILL after `timeout`.
RunResult run(const std::vector<std::string>& argv, const Env& env = Env(),
              std::chrono::milliseconds timeout = std::chrono::milliseconds(20000));

// Whether `program` is found in PATH (or is an executable path).
bool have_program(const std::string& program);
std::string find_program(const std::string& program);

// A background process, SIGTERM then SIGKILL on destruction. Its stdout
// is a pipe readable with read_line(); stderr is inherited.
class Daemon {
public:
    Daemon() = default;
    Daemon(const std::vector<std::string>& argv, const Env& env = Env(), bool capture_stdout = true);
    ~Daemon();
    Daemon(Daemon&& o) noexcept;
    Daemon& operator=(Daemon&& o) noexcept;
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    bool running();
    pid_t pid() const { return pid_; }
    // Next stdout line ("" + false on EOF / timeout).
    bool read_line(std::string& line, std::chrono::milliseconds timeout);
    // Reads lines until one contains `needle`; returns that line.
    bool wait_for_line(const std::string& needle, std::chrono::milliseconds timeout, std::string* line = nullptr);
    // All stdout seen so far (lines read plus anything buffered).
    const std::string& transcript() const { return transcript_; }
    void stop();
    int wait_exit(std::chrono::milliseconds timeout);  // exit code, -1 on timeout/signal

private:
    pid_t pid_ = -1;
    int out_fd_ = -1;
    std::string buffer_;
    std::string transcript_;
};

// A fresh private directory under $TMPDIR, removed recursively on destruction.
class TempDir {
public:
    explicit TempDir(const std::string& prefix = "brosys-test");
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

bool write_file(const std::string& path, const std::string& content);

}  // namespace bstest
