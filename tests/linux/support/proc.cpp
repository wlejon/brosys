#include "linux/support/proc.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace bstest {

namespace {

std::vector<std::string> merged_env(const Env& env) {
    std::map<std::string, std::string> all;
    for (char** e = environ; e && *e; ++e) {
        const char* eq = std::strchr(*e, '=');
        if (eq) all[std::string(*e, static_cast<size_t>(eq - *e))] = eq + 1;
    }
    for (auto& [k, v] : env) {
        if (v.empty()) all.erase(k);
        else all[k] = v;
    }
    std::vector<std::string> out;
    for (auto& [k, v] : all) out.push_back(k + "=" + v);
    return out;
}

// fork + exec with stdout/stderr redirected to the given fds (-1: inherit).
pid_t spawn(const std::vector<std::string>& argv, const Env& env, int out_fd, int err_fd) {
    std::string prog = find_program(argv.at(0));
    if (prog.empty()) prog = argv[0];
    auto envs = merged_env(env);
    std::vector<char*> cargv, cenv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    for (auto& e : envs) cenv.push_back(const_cast<char*>(e.c_str()));
    cenv.push_back(nullptr);
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        // A test killed mid-run (timeout, sanitizer abort) must not leave its
        // private dbus-daemon / helpers behind holding ctest's output pipe.
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(127);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        if (out_fd >= 0) dup2(out_fd, 1);
        if (err_fd >= 0) dup2(err_fd, 2);
        execve(prog.c_str(), cargv.data(), cenv.data());
        std::fprintf(stderr, "exec %s: %s\n", prog.c_str(), std::strerror(errno));
        _exit(127);
    }
    return pid;
}

int decode_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

}  // namespace

std::string find_program(const std::string& program) {
    if (program.find('/') != std::string::npos) return access(program.c_str(), X_OK) == 0 ? program : std::string();
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/local/bin:/usr/bin:/bin";
    p += ":/usr/sbin:/sbin:/usr/libexec";
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        std::string cand = p.substr(start, end - start) + "/" + program;
        if (end > start && access(cand.c_str(), X_OK) == 0) return cand;
        start = end + 1;
    }
    return {};
}

bool have_program(const std::string& program) { return !find_program(program).empty(); }

RunResult run(const std::vector<std::string>& argv, const Env& env, std::chrono::milliseconds timeout) {
    RunResult res;
    int out_pipe[2], err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) != 0) return res;
    if (pipe2(err_pipe, O_CLOEXEC) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return res;
    }
    pid_t pid = spawn(argv, env, out_pipe[1], err_pipe[1]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    bool out_open = true, err_open = true;
    char buf[4096];
    while (out_open || err_open) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            res.timed_out = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            break;
        }
        pollfd fds[2] = {{out_open ? out_pipe[0] : -1, POLLIN, 0}, {err_open ? err_pipe[0] : -1, POLLIN, 0}};
        int n = poll(fds, 2, static_cast<int>(left.count()));
        if (n < 0 && errno != EINTR) break;
        for (int i = 0; i < 2; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t r = read(fds[i].fd, buf, sizeof buf);
            if (r <= 0) {
                (i == 0 ? out_open : err_open) = false;
            } else {
                (i == 0 ? res.out : res.err).append(buf, static_cast<size_t>(r));
            }
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    int status = 0;
    while (true) {
        pid_t w = waitpid(pid, &status, res.timed_out ? 0 : WNOHANG);
        if (w == pid) break;
        if (w < 0) break;
        if (std::chrono::steady_clock::now() > deadline) {
            res.timed_out = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            continue;
        }
        usleep(2000);
    }
    res.exit_code = res.timed_out ? -1 : decode_status(status);
    return res;
}

// ---------------------------------------------------------------- Daemon

Daemon::Daemon(const std::vector<std::string>& argv, const Env& env, bool capture_stdout) {
    int out_pipe[2] = {-1, -1};
    if (capture_stdout && pipe2(out_pipe, O_CLOEXEC) != 0) return;
    pid_ = spawn(argv, env, out_pipe[1], -1);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    out_fd_ = out_pipe[0];
}

Daemon::~Daemon() { stop(); }

Daemon::Daemon(Daemon&& o) noexcept { *this = std::move(o); }

Daemon& Daemon::operator=(Daemon&& o) noexcept {
    if (this != &o) {
        stop();
        pid_ = o.pid_;
        out_fd_ = o.out_fd_;
        buffer_ = std::move(o.buffer_);
        transcript_ = std::move(o.transcript_);
        o.pid_ = -1;
        o.out_fd_ = -1;
    }
    return *this;
}

bool Daemon::running() {
    if (pid_ <= 0) return false;
    int status = 0;
    pid_t w = waitpid(pid_, &status, WNOHANG);
    if (w == pid_) {
        pid_ = -1;
        return false;
    }
    return true;
}

bool Daemon::read_line(std::string& line, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        size_t nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            line = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            return true;
        }
        if (out_fd_ < 0) return false;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        pollfd fd{out_fd_, POLLIN, 0};
        int n = poll(&fd, 1, static_cast<int>(left.count()));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        char buf[4096];
        ssize_t r = read(out_fd_, buf, sizeof buf);
        if (r <= 0) {
            close(out_fd_);
            out_fd_ = -1;
            if (!buffer_.empty()) {
                line = buffer_;
                buffer_.clear();
                return true;
            }
            return false;
        }
        buffer_.append(buf, static_cast<size_t>(r));
        transcript_.append(buf, static_cast<size_t>(r));
    }
}

bool Daemon::wait_for_line(const std::string& needle, std::chrono::milliseconds timeout, std::string* out) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string line;
    while (true) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        if (!read_line(line, left)) return false;
        if (line.find(needle) != std::string::npos) {
            if (out) *out = line;
            return true;
        }
    }
}

int Daemon::wait_exit(std::chrono::milliseconds timeout) {
    if (pid_ <= 0) return -1;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        pid_t w = waitpid(pid_, &status, WNOHANG);
        if (w == pid_) {
            pid_ = -1;
            return decode_status(status);
        }
        usleep(5000);
    }
    return -1;
}

void Daemon::stop() {
    if (pid_ > 0) {
        kill(-pid_, SIGTERM);
        kill(pid_, SIGTERM);
        if (wait_exit(std::chrono::milliseconds(3000)) == -1 && pid_ > 0) {
            kill(-pid_, SIGKILL);
            kill(pid_, SIGKILL);
            int status = 0;
            waitpid(pid_, &status, 0);
        }
        pid_ = -1;
    }
    if (out_fd_ >= 0) {
        close(out_fd_);
        out_fd_ = -1;
    }
}

// ---------------------------------------------------------------- files

TempDir::TempDir(const std::string& prefix) {
    const char* tmp = std::getenv("TMPDIR");
    std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/" + prefix + "-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (mkdtemp(buf.data())) path_ = buf.data();
    if (!path_.empty()) chmod(path_.c_str(), 0700);
}

TempDir::~TempDir() {
    if (path_.empty()) return;
    nftw(
        path_.c_str(),
        [](const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }, 16,
        FTW_DEPTH | FTW_PHYS);
}

bool write_file(const std::string& path, const std::string& content) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace bstest
