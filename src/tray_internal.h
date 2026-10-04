#pragma once

#include "brosys/tray.h"
#include <memory>

namespace brosys {

class ITrayBackend {
public:
    virtual ~ITrayBackend() = default;
    virtual bool start(TrayHost* host) = 0;
    virtual void stop() = 0;
    virtual void on_item_registered(const StatusNotifierItem& item) = 0;
    virtual void on_item_unregistered(const std::string& id) = 0;
    virtual bool activate_item(const std::string& id, int x, int y) = 0;
    virtual bool secondary_activate_item(const std::string& id, int x, int y) = 0;
    virtual bool context_menu(const std::string& id, int x, int y) = 0;
};

std::unique_ptr<ITrayBackend> create_platform_tray_backend();

} // namespace brosys
