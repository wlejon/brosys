#pragma once

#include "brosys/export.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace brosys {

enum class NotificationUrgency {
    Low = 0,
    Normal = 1,
    Critical = 2
};

enum class CloseReason {
    Expired = 1,
    DismissedByUser = 2,
    ClosedByCall = 3,
    Undefined = 4
};

struct NotificationAction {
    std::string key;
    std::string label;

    bool operator==(const NotificationAction& other) const = default;
};

struct NotificationImage {
    int width = 0;
    int height = 0;
    int rowstride = 0;
    bool has_alpha = true;
    int bits_per_sample = 8;
    int channels = 4;
    std::vector<uint8_t> data;

    bool operator==(const NotificationImage& other) const = default;
};

struct NotificationItem {
    uint32_t id = 0;
    std::string app_name;
    std::string summary;
    std::string body;
    std::string icon_name;
    NotificationImage icon_image;
    std::vector<NotificationAction> actions;
    std::unordered_map<std::string, std::string> hints;
    int timeout_ms = -1; // -1 = default, 0 = never expire
    NotificationUrgency urgency = NotificationUrgency::Normal;
    uint64_t timestamp_ms = 0;

    bool operator==(const NotificationItem& other) const = default;
};

struct ServerInfo {
    std::string name;
    std::string vendor;
    std::string version;
    std::string spec_version;

    bool operator==(const ServerInfo& other) const = default;
};

class BROSYS_API NotificationServer {
public:
    using NotificationReceivedCallback = std::function<void(const NotificationItem&)>;
    using ActionInvokedCallback = std::function<void(uint32_t id, const std::string& action_key)>;
    using NotificationClosedCallback = std::function<void(uint32_t id, CloseReason reason)>;

    NotificationServer();
    ~NotificationServer();

    NotificationServer(const NotificationServer&) = delete;
    NotificationServer& operator=(const NotificationServer&) = delete;
    NotificationServer(NotificationServer&&) noexcept;
    NotificationServer& operator=(NotificationServer&&) noexcept;

    bool start();
    void stop();
    [[nodiscard]] bool is_running() const;

    uint32_t post_notification(const NotificationItem& item);
    bool close_notification(uint32_t id, CloseReason reason = CloseReason::ClosedByCall);
    bool invoke_action(uint32_t id, const std::string& action_key);

    [[nodiscard]] std::vector<NotificationItem> get_active_notifications() const;
    [[nodiscard]] std::optional<NotificationItem> get_notification(uint32_t id) const;
    [[nodiscard]] size_t active_count() const;
    void clear_all();

    [[nodiscard]] std::vector<std::string> get_capabilities() const;
    [[nodiscard]] ServerInfo get_server_info() const;

    void set_notification_received_callback(NotificationReceivedCallback cb);
    void set_action_invoked_callback(ActionInvokedCallback cb);
    void set_notification_closed_callback(NotificationClosedCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace brosys
