// Windows tray hosts: the shell-mode host (owns Shell_TrayWnd on its
// desktop) and the alongside host (Explorer or another shell owns it).
#pragma once

#include "brosys/tray.h"
#include "win/tray_balloon.h"

#include <memory>
#include <string>

namespace brosys::win::tray {

// Implemented by both Windows hosts so the notification server can find the
// balloon hook behind a TrayHost* (nullptr when the host is not the shell).
class WinTrayHost : public TrayHost {
public:
    virtual std::shared_ptr<BalloonHub> balloon_hub() = 0;
};

// Starts the shell thread on `desktop` and creates Shell_TrayWnd there.
std::unique_ptr<TrayHost> create_shell_host(const TrayConfig& config, HDESK desktop, std::string* error);
// Hosts nothing; `detail` says why.
std::unique_ptr<TrayHost> create_alongside_host(std::string detail);

// Name of a desktop ("Default", ...).
std::string desktop_name(HDESK desktop);

}  // namespace brosys::win::tray
