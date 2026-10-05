// Accumulates a MessageQueue's events so tests can wait for specific ones.
#pragma once

#include "brosys/event_queue.h"

#include <chrono>
#include <functional>
#include <optional>
#include <variant>
#include <vector>

namespace bstest {

template <class Event>
class EventLog {
public:
    explicit EventLog(brosys::MessageQueue<Event>& q) : q_(q) {}

    size_t mark() {
        pump();
        return all_.size();
    }

    // First event of type T at index >= from matching pred.
    template <class T>
    std::optional<T> wait(const std::function<bool(const T&)>& pred, size_t from,
                          std::chrono::milliseconds timeout = std::chrono::seconds(10), size_t* index = nullptr) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        size_t i = from;
        while (true) {
            pump();
            for (; i < all_.size(); ++i) {
                if (auto* v = std::get_if<T>(&all_[i]); v && pred(*v)) {
                    if (index) *index = i;
                    return *v;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            q_.wait_for(std::chrono::milliseconds(50));
        }
    }

    template <class T>
    size_t count(const std::function<bool(const T&)>& pred, size_t from = 0) {
        pump();
        size_t n = 0;
        for (size_t i = from; i < all_.size(); ++i)
            if (auto* v = std::get_if<T>(&all_[i]); v && pred(*v)) ++n;
        return n;
    }

private:
    void pump() {
        for (auto& e : q_.drain()) all_.push_back(std::move(e));
    }

    brosys::MessageQueue<Event>& q_;
    std::vector<Event> all_;
};

}  // namespace bstest
