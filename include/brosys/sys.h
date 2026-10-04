#pragma once

#include "brosys/export.h"
#include "brosys/version.h"
#include "brosys/power.h"
#include "brosys/audio.h"
#include "brosys/network.h"
#include "brosys/notifications.h"
#include "brosys/tray.h"

#include <memory>

namespace brosys {

class BROSYS_API SystemServices {
public:
    SystemServices();
    ~SystemServices();

    SystemServices(const SystemServices&) = delete;
    SystemServices& operator=(const SystemServices&) = delete;
    SystemServices(SystemServices&&) noexcept;
    SystemServices& operator=(SystemServices&&) noexcept;

    static SystemServices& instance();

    bool initialize();
    void shutdown();
    [[nodiscard]] bool is_initialized() const;

    [[nodiscard]] PowerManager& power();
    [[nodiscard]] AudioManager& audio();
    [[nodiscard]] NetworkManager& network();
    [[nodiscard]] NotificationServer& notifications();
    [[nodiscard]] TrayHost& tray();

    [[nodiscard]] const PowerManager& power() const;
    [[nodiscard]] const AudioManager& audio() const;
    [[nodiscard]] const NetworkManager& network() const;
    [[nodiscard]] const NotificationServer& notifications() const;
    [[nodiscard]] const TrayHost& tray() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace brosys
