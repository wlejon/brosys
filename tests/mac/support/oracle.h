// macOS oracles: the OS's own command-line tools (pmset, ioreg, scutil,
// networksetup, osascript, system_profiler, SwitchAudioSource), run read-only
// and parsed just enough for the tests to compare against.
#pragma once

#include <unistd.h>

#include <array>
#include <cstdio>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace bstest::mac {

struct Output {
    int status = -1;  // exit status, -1 when the command could not run
    std::string out;  // stdout (stderr is discarded)
    bool ok() const { return status == 0; }
};

// Runs `cmd` through /bin/sh with Homebrew on PATH (ctest's environment may
// not have it). stderr goes to /dev/null.
inline Output run(const std::string& cmd) {
    std::string full = "PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin; (" + cmd + ") 2>/dev/null";
    Output o;
    FILE* p = popen(full.c_str(), "r");
    if (!p) return o;
    std::array<char, 4096> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), p)) > 0) o.out.append(buf.data(), n);
    int st = pclose(p);
    o.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return o;
}

inline bool have(const std::string& tool) { return run("command -v " + tool + " >/dev/null").ok(); }

inline std::string trim(std::string s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos) nl = s.size();
        if (nl > start) out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

inline bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
inline bool contains(const std::string& s, const std::string& p) { return s.find(p) != std::string::npos; }

// The text after `key` on the same line, trimmed ("" when absent).
inline std::optional<std::string> after(const std::string& text, const std::string& key) {
    size_t at = text.find(key);
    if (at == std::string::npos) return std::nullopt;
    at += key.size();
    size_t nl = text.find('\n', at);
    return trim(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
}

// A string field of a one-line JSON object ({"name": "x", ...}), as
// SwitchAudioSource -f json prints them; no escapes in its values.
inline std::string json_field(const std::string& line, const std::string& key) {
    std::string k = "\"" + key + "\": \"";
    size_t at = line.find(k);
    if (at == std::string::npos) return {};
    at += k.size();
    size_t end = line.find('"', at);
    return end == std::string::npos ? std::string() : line.substr(at, end - at);
}

inline std::string console_user() { return trim(run("stat -f %Su /dev/console").out); }
inline std::string current_user() { return trim(run("id -un").out); }

}  // namespace bstest::mac
