#pragma once

#include "brosys/brosys.h"
#include "embed/embed.h"
#include "object_builder.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace brosys::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

/// Mount the `bro.sys` APIs into the current Bronze realm:
/// `bro.sys.power`, `bro.sys.audio`, `bro.sys.network`,
/// `bro.sys.bluetooth`, `bro.sys.notifications`, `bro.sys.tray`.
void installSys();

/// Drain pending events from all active services and dispatch callbacks / resolve promises.
/// Safe and recommended to invoke once per frame / tick from the main JS thread.
void tickSysAsync();

/// Shut down and reset all active services, event subscriptions, and pending tasks.
void shutdownSysAsync();

// Service accessors and injection seams
PowerService* getPowerService();
void setPowerService(PowerService* service, bool own = false);

AudioService* getAudioService();
void setAudioService(AudioService* service, bool own = false);

NetworkService* getNetworkService();
void setNetworkService(NetworkService* service, bool own = false);

BluetoothService* getBluetoothService();
void setBluetoothService(BluetoothService* service, bool own = false);

NotificationServer* getNotificationServer();
void setNotificationServer(NotificationServer* server, bool own = false);

TrayHost* getTrayHost();
void setTrayHost(TrayHost* host, bool own = false);

// Event registration & dispatch
void addEventListener(std::string_view target, std::string_view event, Value callback);
void removeEventListener(std::string_view target, std::string_view event, Value callback);
void dispatchEvent(std::string_view target, std::string_view event, Value payload);

// Subsystem installers
void installPower(ObjectBuilder& sys);
void installAudio(ObjectBuilder& sys);
void installNetwork(ObjectBuilder& sys);
void installBluetooth(ObjectBuilder& sys);
void installNotifications(ObjectBuilder& sys);
void installTray(ObjectBuilder& sys);

// Subsystem tick & shutdown
void tickPower();
void tickAudio();
void tickNetwork();
void tickBluetooth();
void tickNotifications();
void tickTray();

void shutdownPower();
void shutdownAudio();
void shutdownNetwork();
void shutdownBluetooth();
void shutdownNotifications();
void shutdownTray();

} // namespace brosys::api

// Global namespace aliases
using brosys::api::installSys;
using brosys::api::tickSysAsync;
using brosys::api::shutdownSysAsync;
