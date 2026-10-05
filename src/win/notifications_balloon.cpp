#include "win/notifications_balloon.h"

#include "win/util.h"

#include <shellapi.h>

namespace brosys::win::notify {

namespace {

std::string escape_markup(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c; break;
        }
    }
    return out;
}

}  // namespace

Notification make_balloon_notification(const tray::BalloonData& b, bool markup) {
    Notification n;
    n.app_name = b.app_id;
    n.summary = to_utf8(b.title);
    std::string body = to_utf8(b.text);
    n.body = markup ? escape_markup(body) : body;
    n.actions.push_back({"default", ""});

    const char* niif = "none";
    switch (b.info_flags & NIIF_ICON_MASK) {
        case NIIF_INFO: niif = "info"; n.app_icon = "dialog-information"; break;
        case NIIF_WARNING: niif = "warning"; n.app_icon = "dialog-warning"; break;
        case NIIF_ERROR: niif = "error"; n.app_icon = "dialog-error"; break;
        case NIIF_USER: niif = "user"; break;
        default: break;
    }
    n.other_hints.emplace_back("x-windows-niif", niif);
    if (b.info_flags & NIIF_LARGE_ICON) n.other_hints.emplace_back("x-windows-large-icon", "true");
    if (b.info_flags & NIIF_RESPECT_QUIET_TIME) n.other_hints.emplace_back("x-windows-respect-quiet-time", "true");
    n.suppress_sound = (b.info_flags & NIIF_NOSOUND) != 0;
    n.image = b.image;
    // uTimeout is ignored since Vista (the shell's own setting decides).
    n.expire_timeout_ms = -1;
    n.sender = b.item_id;
    n.sender_pid = b.pid;
    return n;
}

UINT balloon_close_message(CloseReason reason) {
    switch (reason) {
        case CloseReason::Expired:
        case CloseReason::Dismissed: return NIN_BALLOONTIMEOUT;
        case CloseReason::Closed:
        case CloseReason::Undefined: break;
    }
    return NIN_BALLOONHIDE;
}

}  // namespace brosys::win::notify
