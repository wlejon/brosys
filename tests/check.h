// Minimal test harness: checks are real code paths in every configuration
// (no assert()), failures are counted and reported, and main() returns
// nonzero when anything failed. skip() exits 77 (ctest SKIP_RETURN_CODE).
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

namespace bstest {

inline int& failures() {
    static int n = 0;
    return n;
}

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what.c_str());
    std::fflush(stderr);
}

template <class A, class B>
std::string describe(const char* ea, const char* eb, const A& a, const B& b) {
    std::ostringstream s;
    s << ea << " == " << eb << " (got " << a << " vs " << b << ")";
    return s.str();
}

inline int finish(const char* name) {
    if (failures() == 0) {
        std::printf("[%s] PASSED\n", name);
        return 0;
    }
    std::printf("[%s] FAILED (%d check%s)\n", name, failures(), failures() == 1 ? "" : "s");
    return 1;
}

[[noreturn]] inline void skip(const char* name, const std::string& why) {
    std::printf("[%s] SKIPPED: %s\n", name, why.c_str());
    std::fflush(stdout);
    std::exit(77);
}

// Writes to the machine's real devices and services (even same-value ones,
// and devices the test adds and removes itself) run only with
// BROSYS_TEST_MUTATE=1. CI runners are disposable and set it; a desktop
// user's machine is left alone.
inline bool mutate_opted_in() {
    const char* v = std::getenv("BROSYS_TEST_MUTATE");
    return v && std::string(v) == "1";
}

// Polls `pred` until it holds or `timeout` passes.
inline bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout,
                       std::chrono::milliseconds step = std::chrono::milliseconds(10)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(step);
    }
}

}  // namespace bstest

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) ::bstest::fail(__FILE__, __LINE__, #cond);        \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        auto check_a_ = (a); /* copies: operands may reference temporaries */           \
        auto check_b_ = (b);                                                           \
        if (!(check_a_ == check_b_))                                                   \
            ::bstest::fail(__FILE__, __LINE__,                                         \
                           ::bstest::describe(#a, #b, check_a_, check_b_));            \
    } while (0)

// Fatal variant: stops the current test function.
#define REQUIRE(cond)                                                  \
    do {                                                               \
        if (!(cond)) {                                                 \
            ::bstest::fail(__FILE__, __LINE__, "required: " #cond);    \
            return;                                                    \
        }                                                              \
    } while (0)
