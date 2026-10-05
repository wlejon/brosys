// Collects a service's events and waits for particular ones.
#pragma once

#include "brosys/event_queue.h"

#include <chrono>
#include <optional>
#include <variant>
#include <vector>

namespace bstest {

template <class Event>
class EventLog {
public:
    explicit EventLog(brosys::MessageQueue<Event>& q) : q_(q) {}

    // Pulls whatever is queued into the log.
    void pump() {
        for (auto& e : q_.drain()) all_.push_back(std::move(e));
    }

    // Waits for the next not-yet-matched event of type T satisfying `pred`
    // (events before it are skipped over, not consumed for later waits).
    template <class T, class Pred>
    std::optional<T> wait(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(10000)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            pump();
            while (cursor_ < all_.size()) {
                auto* t = std::get_if<T>(&all_[cursor_++]);
                if (t && pred(*t)) return *t;
            }
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;
            q_.wait_for(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        }
    }
    template <class T>
    std::optional<T> wait(std::chrono::milliseconds timeout = std::chrono::milliseconds(10000)) {
        return wait<T>([](const T&) { return true; }, timeout);
    }

    // Events pushed after the cursor (not yet looked at by wait()).
    std::vector<Event> unseen() {
        pump();
        return std::vector<Event>(all_.begin() + static_cast<std::ptrdiff_t>(cursor_), all_.end());
    }
    void skip_all() {
        pump();
        cursor_ = all_.size();
    }
    const std::vector<Event>& all() const { return all_; }

private:
    brosys::MessageQueue<Event>& q_;
    std::vector<Event> all_;
    size_t cursor_ = 0;
};

}  // namespace bstest
