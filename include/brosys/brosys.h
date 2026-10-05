// brosys: system services for a desktop environment (power, audio, network,
// notifications, tray). Each service is created independently, runs its
// own backend thread(s), and reports facts as value snapshots through its
// own MessageQueue, which the host drains on its own thread.
#pragma once

#include "brosys/audio.h"
#include "brosys/common.h"
#include "brosys/event_queue.h"
#include "brosys/network.h"
#include "brosys/notifications.h"
#include "brosys/power.h"
#include "brosys/tray.h"

#define BROSYS_VERSION_MAJOR 0
#define BROSYS_VERSION_MINOR 2
#define BROSYS_VERSION_PATCH 0

namespace brosys {
const char* version_string();
}  // namespace brosys
