// Balloon <-> notification mapping (pure; no window or thread state).
#pragma once

#include "brosys/notifications.h"
#include "win/tray_balloon.h"

namespace brosys::win::notify {

// A Shell_NotifyIcon balloon as a Notification (id and expiry unset).
//   summary / body   szInfoTitle / szInfo (body escaped when `markup`)
//   app_name         the icon's executable; sender = the tray item id
//   app_icon         dialog-information / -warning / -error for NIIF_INFO/WARNING/ERROR
//   image            NIIF_USER: hBalloonIcon (or the tray icon's own image)
//   suppress_sound   NIIF_NOSOUND
//   other_hints      x-windows-niif, x-windows-large-icon, x-windows-respect-quiet-time
//   actions          {"default"}: clicking the balloon
Notification make_balloon_notification(const tray::BalloonData& b, bool markup);

// What the icon hears when the host closes its balloon:
//   Expired   -> NIN_BALLOONTIMEOUT (the timeout passed)
//   Dismissed -> NIN_BALLOONTIMEOUT (what Windows sends for the close button)
//   Closed / Undefined -> NIN_BALLOONHIDE (the balloon disappeared)
UINT balloon_close_message(CloseReason reason);

}  // namespace brosys::win::notify
