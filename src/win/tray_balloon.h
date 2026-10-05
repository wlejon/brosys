// The internal hook between a shell-mode tray host and the notification
// server that renders its balloons (NotificationServerConfig::balloon_source).
//
// Balloons that arrive while no server is attached are held (the latest
// per icon, until the sender clears it or the icon goes) and delivered when
// a server attaches, so a server created after the tray host still sees
// what is showing.
//
// Lock order: BalloonHub::mutex_ is taken before the server's or the tray's
// own mutex, never after; neither side calls the hub while holding its own
// mutex.
#pragma once

#include "brosys/common.h"
#include "win/tray_wire.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace brosys::win::tray {

// One balloon request (Shell_NotifyIcon NIF_INFO with a non-empty szInfo).
struct BalloonData {
    std::string item_id;
    std::string app_id;  // executable base name
    uint32_t pid = 0;
    std::wstring title;  // szInfoTitle
    std::wstring text;   // szInfo
    DWORD info_flags = 0;
    std::optional<Image> image;  // NIIF_USER: hBalloonIcon, else the icon's own image
    CallbackTarget target;
};

// Why a balloon disappeared on the icon's side.
enum class BalloonGone {
    SenderCleared,  // NIF_INFO with an empty szInfo
    IconDeleted,    // NIM_DELETE
    OwnerDied,      // the icon's window no longer exists
};

class BalloonSink {
public:
    virtual ~BalloonSink() = default;
    // Called on the tray thread while the sender is blocked in Shell_NotifyIcon.
    virtual void balloon_shown(const BalloonData& data) = 0;
    virtual void balloon_gone(const std::string& item_id, BalloonGone why) = 0;
    // The tray host is being destroyed; no further calls follow.
    virtual void source_gone() = 0;
};

class BalloonHub {
public:
    using Lookup = std::function<std::optional<CallbackTarget>(const std::string& item_id)>;

    explicit BalloonHub(Lookup lookup) : lookup_(std::move(lookup)) {}

    // Server side. attach() connects the sink; the held balloons follow with
    // deliver_held() once the server has reported its status.
    bool attach(BalloonSink* sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sink_ || !lookup_) return false;
        sink_ = sink;
        return true;
    }
    void deliver_held(BalloonSink* sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sink_ != sink) return;
        for (auto& b : held_) {
            // The icon may have changed since (version, window): use its current target.
            std::optional<CallbackTarget> t = lookup_ ? lookup_(b.item_id) : std::nullopt;
            if (!t) continue;
            b.target = *t;
            sink_->balloon_shown(b);
        }
        held_.clear();
    }
    void detach(BalloonSink* sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sink_ == sink) sink_ = nullptr;
    }
    // The icon's current callback target (its version may have changed since
    // the balloon was posted); nullopt once the icon or the tray is gone.
    std::optional<CallbackTarget> target(const std::string& item_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return lookup_ ? lookup_(item_id) : std::nullopt;
    }
    bool source_alive() {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<bool>(lookup_);
    }

    // Tray side. Returns whether a server received it (else it is held).
    bool shown(const BalloonData& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        drop_held(data.item_id);  // a newer balloon replaces a held one
        if (!sink_) {
            held_.push_back(data);
            return false;
        }
        sink_->balloon_shown(data);
        return true;
    }
    void gone(const std::string& item_id, BalloonGone why) {
        std::lock_guard<std::mutex> lock(mutex_);
        drop_held(item_id);
        if (sink_) sink_->balloon_gone(item_id, why);
    }
    void tray_destroyed() {
        std::lock_guard<std::mutex> lock(mutex_);
        lookup_ = nullptr;
        held_.clear();
        if (sink_) sink_->source_gone();
        sink_ = nullptr;
    }
    size_t held() {
        std::lock_guard<std::mutex> lock(mutex_);
        return held_.size();
    }

private:
    void drop_held(const std::string& item_id) {
        for (auto it = held_.begin(); it != held_.end(); ++it)
            if (it->item_id == item_id) {
                held_.erase(it);
                return;
            }
    }

    std::mutex mutex_;
    Lookup lookup_;
    BalloonSink* sink_ = nullptr;
    std::vector<BalloonData> held_;  // arrival order, at most one per icon
};

}  // namespace brosys::win::tray
