# brosys

`brosys` is the high-performance, cross-platform Desktop System Services and Hardware Integration engine for the Bro ecosystem (`bro.sys`).

It connects Bro's desktop environment (panel, dock, control center, notifications, lock screen) to OS hardware and background system services:
1. **Power & Battery (`power`)**:
   - Query battery percentage, charging state, estimated battery life / time to empty/full.
   - Request system suspend/sleep, lock, reboot, and poweroff.
   - Windows: `GetSystemPowerStatus`, `SetSuspendState`, `ExitWindowsEx`, `LockWorkStation`.
   - Linux: `org.freedesktop.UPower` (battery devices, charge percentage, state) + `org.freedesktop.login1` (Suspend, Reboot, PowerOff).
2. **Audio Mixer & Endpoints (`audio`)**:
   - Query and set master system volume (0.0 to 1.0), mute toggle.
   - List available audio output sinks (speakers, headphones, HDMI) and input sources (microphones).
   - Real-time volume change notification callbacks.
   - Windows: Windows Core Audio (`IAudioEndpointVolume`, `IMMDeviceEnumerator`, `IMMNotificationClient`).
   - Linux: PipeWire / PulseAudio client or MPRIS (`org.mpris.MediaPlayer2` media playback controls).
3. **Network & Wi-Fi (`network`)**:
   - Query active connection status (Ethernet / Wi-Fi / disconnected).
   - Scan for available Wi-Fi access points (SSID, signal strength percentage/dBm, security type [Open, WPA2, WPA3]).
   - Connection event notifications.
   - Windows: Windows WLAN API (`wlanapi.h` via `wlanapi.lib`, `WlanOpenHandle`, `WlanScan`, `WlanGetAvailableNetworkList`).
   - Linux: `org.freedesktop.NetworkManager` over D-Bus (`GetAccessPoints`, active connections).
4. **Desktop Notifications (`notifications`)**:
   - Host the desktop notification server and dispatch notification popups.
   - Fields: `id`, `app_name`, `summary`, `body`, `icon_name` / `icon_data`, `actions` (button label + action key), `timeout_ms`, `urgency` (Low, Normal, Critical).
   - Emits events: `on_action_invoked(id, action_key)`, `on_notification_closed(id, reason)`.
   - Linux: Hosts the D-Bus service `org.freedesktop.Notifications`.
   - Windows: Win32 notification manager / toast window dispatcher.
5. **System Tray / Status Notifier (`tray`)**:
   - Host the desktop status notifier area for third-party apps (Discord, Steam, Slack, OBS).
   - Fields: `id`, `title`, `icon_name`, `icon_pixmap` (RGBA), `status` (Passive, Active, NeedsAttention), `tooltip`.
   - Emits events: `on_item_added`, `on_item_updated`, `on_item_removed`, `activate_item(id, x, y)`.
   - Linux: Hosts `StatusNotifierWatcher` on D-Bus and parses context menu items (`com.canonical.dbusmenu`).
   - Windows: Win32 `Shell_NotifyIconW` listener and taskbar icon integration.

Pure C++20 engine designed to be embedded directly into Bro and sibling projects with zero external heavy dependencies (no Qt, no GLib).

---

## Architecture & Module Layout

```
brosys/
├── CMakeLists.txt
├── README.md
├── include/brosys/
│   ├── version.h              # Version macros & functions
│   ├── export.h               # Shared / static export attributes
│   ├── power.h                # Battery & system power management
│   ├── audio.h                # Core Audio mixer, endpoints, and volume
│   ├── network.h              # Adapters, IP status, and Wi-Fi scanning
│   ├── notifications.h        # Desktop notification server
│   ├── tray.h                 # StatusNotifierItem & tray host
│   └── sys.h                  # Unified SystemServices context
├── src/
│   ├── version.cpp
│   ├── sys.cpp
│   ├── dbus_helper.h          # Modern C++20 sd-bus wrapper (Linux)
│   ├── dbus_helper.cpp
│   ├── power_internal.h
│   ├── power_common.cpp
│   ├── power_win.cpp          # Windows GetSystemPowerStatus / SetSuspendState
│   ├── power_linux.cpp        # Linux UPower / systemd-logind
│   ├── audio_internal.h
│   ├── audio_common.cpp
│   ├── audio_win.cpp          # Windows Core Audio / MMDeviceEnumerator
│   ├── audio_linux.cpp        # Linux MPRIS / PipeWire
│   ├── network_internal.h
│   ├── network_common.cpp
│   ├── network_win.cpp        # Windows WLAN API & IP Helper
│   ├── network_linux.cpp      # Linux NetworkManager / sysfs
│   ├── notifications_internal.h
│   ├── notifications_common.cpp
│   ├── notifications_win.cpp  # Windows toast / notification manager
│   ├── notifications_linux.cpp# Linux org.freedesktop.Notifications
│   ├── tray_internal.h
│   ├── tray_common.cpp
│   ├── tray_win.cpp           # Windows Shell_NotifyIcon integration
│   └── tray_linux.cpp         # Linux StatusNotifierWatcher / dbusmenu
└── tests/
    ├── CMakeLists.txt
    ├── test_common.h
    ├── test_smoke.cpp
    ├── test_power.cpp
    ├── test_audio.cpp
    ├── test_network.cpp
    ├── test_notifications.cpp
    └── test_tray.cpp
```

---

## Quick Example

### Unified Services Context
```cpp
#include <brosys/sys.h>
#include <iostream>

int main() {
    auto& sys = brosys::SystemServices::instance();
    sys.initialize();

    // Query battery
    auto battery = sys.power().get_battery_info();
    std::cout << "Battery: " << battery.percentage << "%\n";

    // Query audio
    float vol = sys.audio().get_master_volume();
    std::cout << "Master volume: " << (vol * 100.0f) << "%\n";

    // Query network
    auto net = sys.network().get_status();
    std::cout << "IP: " << net.ip_address << " (" << net.connection_name << ")\n";

    // Post notification
    brosys::NotificationItem item;
    item.app_name = "Bro Desktop";
    item.summary = "Welcome";
    item.body = "System services initialized successfully.";
    sys.notifications().post_notification(item);

    sys.shutdown();
}
```

---

## Building & Testing

```bash
cmake -B build -S .
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```
