// Waiting for specific events in a brosys MessageQueue: events that do not
// match stay in a backlog for later waits, in order.
#pragma once

#include "brosys/event_queue.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <variant>

namespace bstest {

template <class Event>
class EventLog {
public:
    explicit EventLog(brosys::MessageQueue<Event>& q) : q_(q) {}

    // The first event of type T satisfying `pred` (removed from the log).
    template <class T>
    std::optional<T> wait(const std::function<bool(const T&)>& pred,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            pull();
            for (auto it = backlog_.begin(); it != backlog_.end(); ++it) {
                if (auto* t = std::get_if<T>(&*it); t && pred(*t)) {
                    T out = std::move(*t);
                    backlog_.erase(it);
                    return out;
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;
            q_.wait_for(std::min<std::chrono::milliseconds>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), std::chrono::milliseconds(50)));
        }
    }

    template <class T>
    std::optional<T> wait_any(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        return wait<T>([](const T&) { return true; }, timeout);
    }

    // Whether an event of type T satisfying `pred` arrives within `window` (it stays logged).
    template <class T>
    bool seen(const std::function<bool(const T&)>& pred, std::chrono::milliseconds window) {
        auto deadline = std::chrono::steady_clock::now() + window;
        while (true) {
            pull();
            for (auto& e : backlog_)
                if (auto* t = std::get_if<T>(&e); t && pred(*t)) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            q_.wait_for(std::chrono::milliseconds(20));
        }
    }

    void clear() {
        pull();
        backlog_.clear();
    }

private:
    void pull() {
        for (auto& e : q_.drain()) backlog_.push_back(std::move(e));
    }
    brosys::MessageQueue<Event>& q_;
    std::deque<Event> backlog_;
};

}  // namespace bstest
