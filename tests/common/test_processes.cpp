// Process snapshot, signal parsing, and kill_process tests.
#include "check.h"

#include "brosys/processes.h"

#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace brosys;

static void test_parse_signal() {
    CHECK_EQ(parse_signal("SIGTERM"), 15);
    CHECK_EQ(parse_signal("TERM"), 15);
    CHECK_EQ(parse_signal("sigterm"), 15);
    CHECK_EQ(parse_signal("15"), 15);

    CHECK_EQ(parse_signal("SIGKILL"), 9);
    CHECK_EQ(parse_signal("KILL"), 9);
    CHECK_EQ(parse_signal("9"), 9);

    CHECK_EQ(parse_signal("SIGINT"), 2);
    CHECK_EQ(parse_signal("INT"), 2);
    CHECK_EQ(parse_signal("SIGHUP"), 1);
    CHECK_EQ(parse_signal("HUP"), 1);
    CHECK_EQ(parse_signal("SIGSTOP"), 19);
    CHECK_EQ(parse_signal("STOP"), 19);
    CHECK_EQ(parse_signal("SIGCONT"), 18);
    CHECK_EQ(parse_signal("CONT"), 18);

    CHECK_EQ(parse_signal(""), 0);
    CHECK_EQ(parse_signal("SIGFOO"), 0);
    CHECK_EQ(parse_signal("-5"), 0);
    CHECK_EQ(parse_signal("999"), 0);
}

static void test_list_processes() {
    auto procs = list_processes();
    REQUIRE(!procs.empty());

#ifdef _WIN32
    const int32_t self_pid = static_cast<int32_t>(GetCurrentProcessId());
#else
    const int32_t self_pid = static_cast<int32_t>(::getpid());
#endif

    auto it = std::find_if(procs.begin(), procs.end(), [&](const ProcessInfo& p) {
        return p.pid == self_pid;
    });
    REQUIRE(it != procs.end());
    CHECK_EQ(it->pid, self_pid);
    CHECK(!it->name.empty());
    CHECK(!it->cmdline.empty());
    CHECK(!it->state.empty());
    CHECK(it->rss_bytes > 0);
    CHECK(it->threads >= 1);

    auto svc = ProcessService::create();
    REQUIRE(svc != nullptr);
    auto svc_procs = svc->list();
    CHECK(!svc_procs.empty());
}

static void test_kill_validation_and_child() {
    CHECK(!kill_process(0, 15).ok);
    CHECK(!kill_process(1, 15).ok);
    CHECK(!kill_process(-10, 9).ok);
    CHECK(!kill_process(99999, 0).ok);
    CHECK(!kill_process(99999, 999).ok);

#ifndef _WIN32
    pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        for (;;) {
            ::pause();
        }
        _exit(0);
    }

    const int32_t cpid = static_cast<int32_t>(child);
    auto procs = list_processes();
    const bool found = std::any_of(procs.begin(), procs.end(), [&](const ProcessInfo& p) {
        return p.pid == cpid;
    });
    CHECK(found);

    CHECK(kill_process(cpid, signal_num::SIGSTOP_NUM).ok);
    CHECK(kill_process(cpid, signal_num::SIGCONT_NUM).ok);
    CHECK(kill_process(cpid, signal_num::SIGKILL_NUM).ok);

    int status = 0;
    const pid_t waited = ::waitpid(child, &status, 0);
    CHECK_EQ(waited, child);
    CHECK(WIFSIGNALED(status));
#endif
}

int main() {
    test_parse_signal();
    test_list_processes();
    test_kill_validation_and_child();
    return bstest::finish("test_processes");
}
