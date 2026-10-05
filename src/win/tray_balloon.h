// The internal hook between a shell-mode tray host and the notification
// server that renders its balloons (NotificationServerConfig::balloon_source).
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

    // Server side.
    bool attach(BalloonSink* sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sink_ || !lookup_) return false;
        sink_ = sink;
        return true;
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

    // Tray side.
    bool shown(const BalloonData& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sink_) return false;
        sink_->balloon_shown(data);
        return true;
    }
    void gone(const std::string& item_id, BalloonGone why) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sink_) sink_->balloon_gone(item_id, why);
    }
    void tray_destroyed() {
        std::lock_guard<std::mutex> lock(mutex_);
        lookup_ = nullptr;
        if (sink_) sink_->source_gone();
        sink_ = nullptr;
    }

private:
    std::mutex mutex_;
    Lookup lookup_;
    BalloonSink* sink_ = nullptr;
};

}  // namespace brosys::win::tray
