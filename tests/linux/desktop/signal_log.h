// An independent client connection that records the signals matching a
// rule, as any other process on the bus would receive them.
#pragma once

#include "check.h"
#include "linux/dbus/connection.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bstest {

class SignalLog {
public:
    SignalLog(const std::string& address, const std::string& rule) {
        std::string err;
        conn_ = brosys::dbus::Connection::open(brosys::dbus::BusKind::Session, address, "signal-log", &err);
        if (conn_)
            conn_->add_match(rule, [this](const brosys::dbus::Message& m) {
                std::lock_guard<std::mutex> lock(mu_);
                messages_.push_back(m);
            });
    }
    bool ok() const { return conn_ != nullptr; }
    brosys::dbus::Connection* connection() { return conn_.get(); }

    std::vector<brosys::dbus::Message> snapshot() {
        std::lock_guard<std::mutex> lock(mu_);
        return messages_;
    }
    // Index of the first message (at or after `from`) satisfying pred, -1 after timeout.
    int wait(const std::function<bool(const brosys::dbus::Message&)>& pred,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(5000), size_t from = 0) {
        int found = -1;
        wait_until([&] {
            std::lock_guard<std::mutex> lock(mu_);
            for (size_t i = from; i < messages_.size(); ++i)
                if (pred(messages_[i])) {
                    found = static_cast<int>(i);
                    return true;
                }
            return false;
        }, timeout);
        return found;
    }

private:
    std::mutex mu_;
    std::vector<brosys::dbus::Message> messages_;
    std::unique_ptr<brosys::dbus::Connection> conn_;  // last: joined first
};

}  // namespace bstest
