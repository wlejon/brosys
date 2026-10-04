#if defined(_WIN32)

#include "tray_internal.h"
#include <windows.h>
#include <shellapi.h>
#include <iostream>

#pragma comment(lib, "shell32.lib")

namespace brosys {

class WindowsTrayBackend : public ITrayBackend {
public:
    WindowsTrayBackend() = default;

    bool start(TrayHost* host) override {
        host_ = host;
        return true;
    }

    void stop() override {
        host_ = nullptr;
    }

    void on_item_registered(const StatusNotifierItem& /*item*/) override {
        // Integrate with Shell_NotifyIconW or native notification area
    }

    void on_item_unregistered(const std::string& /*id*/) override {
        // Clean up taskbar icon
    }

    bool activate_item(const std::string& /*id*/, int /*x*/, int /*y*/) override {
        return true;
    }

    bool secondary_activate_item(const std::string& /*id*/, int /*x*/, int /*y*/) override {
        return true;
    }

    bool context_menu(const std::string& /*id*/, int /*x*/, int /*y*/) override {
        return true;
    }

private:
    TrayHost* host_ = nullptr;
};

std::unique_ptr<ITrayBackend> create_platform_tray_backend() {
    return std::make_unique<WindowsTrayBackend>();
}

} // namespace brosys

#endif // _WIN32
