// Notifications: the desktop notification server. Other processes post
// notifications; the host renders them and reports user interaction back.
//
// Linux: owns org.freedesktop.Notifications on the session bus and
// implements the Desktop Notifications Specification 1.2 completely: Notify
// (replaces_id, actions, hints, expire_timeout), CloseNotification,
// GetCapabilities, GetServerInformation, the NotificationClosed /
// ActionInvoked / ActivationToken signals.
// Windows: notifications reach a process only when it is the shell. In shell
// mode the tray host's balloon notifications (Shell_NotifyIcon NIF_INFO)
// become notifications here and interaction is reported back to the icon
// (NIN_BALLOONUSERCLICK / NIN_BALLOONTIMEOUT / NIN_BALLOONHIDE). Alongside
// Explorer only notifications the host posts itself are seen;
// capabilities() says which.
// macOS: local only. Other applications' notifications go to Notification
// Center, which has no server role and no API to receive them.
#pragma once

#include "brosys/common.h"
#include "brosys/event_queue.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace brosys {

class TrayHost;

enum class Urgency : uint8_t { Low = 0, Normal = 1, Critical = 2 };

// Values are the specification's NotificationClosed reason codes.
enum class CloseReason : uint32_t {
    Expired = 1,    // the timeout passed
    Dismissed = 2,  // the user dismissed it
    Closed = 3,     // CloseNotification (the sender) or the host closed it
    Undefined = 4,
};

struct NotificationAction {
    std::string key;    // "default" is the action for clicking the notification itself
    std::string label;  // with action-icons: an icon name
    bool operator==(const NotificationAction&) const = default;
};

struct Notification {
    uint32_t id = 0;                // never 0 for a posted notification
    std::string app_name;
    std::string app_icon;           // icon name or file:// URI
    std::string summary;
    std::string body;               // may contain the spec's markup subset when "body-markup" is advertised
    std::vector<NotificationAction> actions;

    // Standard hints.
    Urgency urgency = Urgency::Normal;
    std::string category;           // "email.arrived", ...
    std::string desktop_entry;      // "org.gnome.Evolution"
    std::optional<Image> image;     // image-data / image_data / icon_data (in that precedence)
    std::string image_path;         // image-path / image_path
    std::string sound_file;
    std::string sound_name;
    bool suppress_sound = false;
    bool transient = false;
    bool resident = false;
    bool action_icons = false;
    std::optional<int32_t> x;
    std::optional<int32_t> y;
    // Every other hint, rendered as text (GVariant-like) for the host to inspect.
    std::vector<std::pair<std::string, std::string>> other_hints;

    int32_t expire_timeout_ms = -1;  // as requested: -1 server default, 0 never
    // When the server will close it with CloseReason::Expired (nullopt = never).
    std::optional<std::chrono::steady_clock::time_point> expires_at;

    std::string sender;             // unique bus name (Linux), "hwnd:uid" of the icon (Windows), "local"
    uint32_t sender_pid = 0;

    bool operator==(const Notification&) const = default;
};

// ---------------------------------------------------------------- events

// A new notification, or `replaced` = an existing id was updated in place
// (replaces_id / NIM_MODIFY); the host should update the existing popup.
struct NotificationPosted {
    Notification notification;
    bool replaced = false;
};

// The notification is gone, whoever closed it (sender, host, expiry).
struct NotificationClosed {
    uint32_t id = 0;
    CloseReason reason = CloseReason::Undefined;
};

// Lost (or regained) ownership of the service name; while lost the server
// receives nothing.
struct NotificationServerStatus {
    bool active = false;
    std::string detail;
};

using NotificationEvent = std::variant<NotificationPosted, NotificationClosed, NotificationServerStatus>;
using NotificationEventQueue = MessageQueue<NotificationEvent>;

struct NotificationServerConfig {
    std::string name = "brosys";    // GetServerInformation
    std::string vendor = "bro";
    std::string version = "0.2";
    // GetCapabilities. Advertise only what the host actually renders.
    std::vector<std::string> capabilities{"actions", "body", "body-markup", "icon-static", "persistence"};
    int32_t default_timeout_ms = 5000;  // used for expire_timeout == -1 (critical never expires)
    // Linux: replace an existing owner of org.freedesktop.Notifications if it
    // allows replacement; otherwise queue for the name.
    bool replace_existing = false;
    std::string session_bus_address;   // Linux: override (tests use a private bus); empty = default
    TrayHost* balloon_source = nullptr; // Windows: balloons of this (shell-mode) tray host become notifications
};

struct NotificationServerCapabilities {
    bool receives_foreign = false;  // other processes' notifications reach this server
    std::string source;             // "org.freedesktop.Notifications", "Shell_NotifyIcon balloons", "local only"
    std::string detail;             // why, when receives_foreign is false
};

class NotificationServer {
public:
    static std::unique_ptr<NotificationServer> create(const NotificationServerConfig& config, std::string* error);
    virtual ~NotificationServer() = default;

    virtual NotificationEventQueue& events() = 0;
    virtual NotificationServerCapabilities capabilities() const = 0;
    virtual std::vector<Notification> active() const = 0;

    // The user clicked an action (or "default"). Emits ActionInvoked (+
    // ActivationToken when given, before it); a non-resident notification is
    // then closed with Dismissed.
    virtual Result invoke_action(uint32_t id, const std::string& action_key, const std::string& activation_token) = 0;
    // The host closed it: Dismissed (user), Expired (host-managed timeout), ...
    virtual Result close(uint32_t id, CloseReason reason) = 0;
    // Shows the host's own notification through the same path; returns its id (0 on failure).
    virtual uint32_t post(const Notification& notification) = 0;
};

const char* to_string(CloseReason r);
const char* to_string(Urgency u);

}  // namespace brosys
