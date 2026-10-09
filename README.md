# brosys

[![CI](https://github.com/wlejon/brosys/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/brosys/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

System-services substrate for a desktop environment: battery and power state, audio sinks and
sources, network connectivity and Wi-Fi management, Bluetooth, desktop notifications, and the
system tray. A standalone C++20 library with native platform backends for Linux, Windows,
and macOS.

brosys sits in the desktop-environment layer of the
[bro ecosystem](https://github.com/wlejon/bro/blob/main/docs/ecosystem.md). It is consumed by
the [bro runtime](https://github.com/wlejon/bro) under the `BRO_WITH_SYS` feature gate. The
engine mounts its JavaScript binding (`brosys_api` in `src/api/`) onto `bro.sys`, providing
desktop applications with unified reactive access to underlying hardware and OS services.

## Architecture & Model

Each service is created independently and manages its own background worker thread(s).
Backends push immutable value snapshots into a thread-safe `MessageQueue` (`event_queue.h`),
which the host drains on its own thread:

- **No callback re-entrancy:** Backend threads never execute host application code; an
  optional wake hook posts an event to the host loop.
- **Snapshot queries:** Synchronous queries return the latest known immutable state.
- **Commands & Results:** Mutating commands return a `Result` status, and real hardware or OS
  changes arrive asynchronously as events in the queue.
- **No mocks in production:** Public APIs report genuine hardware state and capabilities; mocks
  and emulators are restricted strictly to test fixtures.

```cpp
std::string err;
auto power = brosys::PowerService::create({}, &err);
power->events().set_wake([] { /* post wake to host event loop */ });

// On the host thread:
for (auto& ev : power->events().drain()) {
    if (auto* c = std::get_if<brosys::PowerChanged>(&ev)) {
        render_battery(c->state);
    }
}
```

```
include/brosys/
  brosys.h          Umbrella header
  common.h          Result, Image (RGBA8 buffer), Availability
  event_queue.h     MessageQueue<T> (thread-safe MPSC queue with wake hook)
  power.h           PowerService: battery levels, AC state, lid switch, inhibitors;
                    ScreenSaverServer: org.freedesktop.ScreenSaver export
  audio.h           AudioService: sinks/sources, default device routing, volume, mute
  network.h         NetworkService: connectivity, interfaces, IP configs, Wi-Fi scans/connect, VPN
  bluetooth.h       BluetoothService: adapters, devices, power, discovery, pairing/connect
  notifications.h   NotificationServer: notification daemon, actions, history, Do-Not-Disturb
  tray.h            TrayHost: status-notifier items, context menus, interaction
  api.h             Bronze JavaScript binding entry point (brosys_api)
```

## Platform Backends

Operating system integrations are implemented directly against native platform facilities:

| Service | Linux | Windows | macOS |
|---|---|---|---|
| **Power & Battery** | UPower (`IsPresent`, battery levels, AC status) & logind (idle/shutdown inhibitors, `PrepareForSleep`) via `brodbus` | `GetSystemPowerStatus`, battery IOCTLs, powrprof capabilities, power requests, shutdown block reasons | IOKit power sources (`AppleSmartBattery`), `AppleClamshellState`, `IORegisterForSystemPower` sleep/wake callbacks, IOPM assertions |
| **Audio** | PipeWire client (`libpipewire-0.3`: nodes, routes, default metadata via `BROSYS_WITH_PIPEWIRE`) | Core Audio (`IMMDeviceEnumerator`, `IMMNotificationClient`, `IAudioEndpointVolume`, `IPolicyConfig`) | CoreAudio HAL (property listener blocks, virtual main volume, `out:`/`in:` device UIDs) |
| **Network & Wi-Fi** | NetworkManager over D-Bus (`brodbus`): IP config, active connections, Wi-Fi scans, `connect_wifi`, `connect_vpn` | IP Helper API (`GetAdaptersAddresses`, routing metrics), Network List Manager (NLM), WLAN API (`WlanGetNetworkBssList`) | SystemConfiguration dynamic store + `getifaddrs`, `Network.framework` path monitor, `CoreWLAN` |
| **Network Control** | Full Wi-Fi and VPN connect/disconnect via NetworkManager | Unsupported (reports explanatory error) | Unsupported (reports explanatory error) |
| **Bluetooth** | BlueZ 5 via `brodbus` (ObjectManager, Adapter1, Device1: pairing, connect, discovery) | Unsupported (reports explanatory error) | Unsupported (reports explanatory error) |
| **Screen Saver** | Exports `org.freedesktop.ScreenSaver` mapped to logind idle inhibitors | Unsupported (reports explanatory error) | Unsupported (reports explanatory error) |
| **Notifications** | FreeDesktop `org.freedesktop.Notifications` v1.2 server with actions, hints, and replace | Shell mode: tray balloons (`NIF_INFO`); alongside Explorer: local notifications | Local notification delivery (does not duplicate UNUserNotificationCenter toasts) |
| **Notification History & DND** | Persistent history log with Do-Not-Disturb suppression across all platforms | Supported | Supported |
| **System Tray** | StatusNotifierItem (SNI) host + `StatusNotifierWatcher`, `dbusmenu` data model | Shell mode: owns `Shell_TrayWnd` (`WM_COPYDATA NIM_*`); alongside Explorer: role `None` | Role `None` (menu extras cannot be hosted by third-party processes; clients use SDL3 tray) |

## Building & Dependencies

brosys requires CMake 3.24+ and a C++20 compiler.

### Dependencies

- **Linux:**
  - Requires **[brodbus](https://github.com/wlejon/brodbus)** for D-Bus connection management and private bus isolation.
  - Requires `libsystemd-dev` (sd-bus) >= 246 and `pkg-config`.
  - Optionally requires `libpipewire-0.3-dev` for the PipeWire audio backend (`-DBROSYS_WITH_PIPEWIRE=ON`, default if found).
- **Windows:** MSVC 2022+; links `iphlpapi`, `wlanapi`, `powrprof`, `ole32`, `user32`.
- **macOS:** Apple Clang (macOS 13+); links `IOKit`, `CoreAudio`, `SystemConfiguration`, `CoreWLAN`, `Network`.

### Dependency Resolution (brodbus)

On Linux, `brosys` needs the `brodbus` library. There are no submodules: brodbus (and
bronze, for the JavaScript binding) is a `bro_dependency()` pin in `CMakeLists.txt`,
resolved through `cmake/bro_deps.cmake` in this order:
1. **Existing CMake target:** An existing `brodbus` target (e.g. added by a superbuild).
2. **Working tree:** `../brodbus` beside the top-level project (or `-DFETCHCONTENT_SOURCE_DIR_BRODBUS=<path>`).
3. **Pinned commit:** fetched from GitHub at configure, so a plain `git clone` builds.

### Standalone Build

```bash
# Linux (GCC / Clang + Ninja)
sudo apt install libsystemd-dev libpipewire-0.3-dev pkg-config ninja-build
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure

# Windows (Visual Studio 2022)
cmake -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# macOS (Apple Clang + Ninja)
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

### Consuming brosys

Downstream projects link against `brosys::brosys`:

```cmake
add_subdirectory(path/to/brosys)
target_link_libraries(your_target PRIVATE brosys::brosys)
```

The standalone Bronze JavaScript binding (`BROSYS_ENABLE_API`, on when brosys is the
top-level project) builds `brosys_api` for the [bronze](https://github.com/wlejon/bronze)
runtime. bronze (with brass) resolves like brodbus: `../bronze` beside the top-level project,
else the head of its main branch. Set `-DBROSYS_ENABLE_API=OFF` to disable the JavaScript binding.

## Tests & Test Oracles

All tests use standard ctest without mock assertions in library code. Absent services or
missing optional tools exit with status `77` (ctest skip) and output the skip reason.

### Linux Isolation & Oracles

Linux tests run against isolated private bus and daemon environments without touching the
active user session:
- **Private `dbus-daemon`:** All mutating tests run on private session/system bus instances
  managed by `tests/linux/support/private_bus.cpp` via `brodbus`.
- **UPower & umockdev:** `test_power_mock` drives real `upowerd` under `umockdev` on a private
  bus, verified against `upower -d`.
- **PipeWire & WirePlumber:** `test_audio_private` spins up an isolated PipeWire daemon with
  WirePlumber and null sinks, verified using `wpctl` and `pactl`.
- **Ayatana Indicators & Notifications:** `test_tray_ayatana` runs a real GTK application under
  `Xvfb`; `test_notify_server` validates against `notify-send` and `gdbus`.
- **Opt-in Wi-Fi Testing:** `test_network_wifi` is opt-in (`BRO_TEST_WIFI=1`, requires sudo)
  and runs against `mac80211_hwsim` with a virtual `hostapd` access point.

### Windows Private Desktop Isolation

Windows tray testing requires the host to act as the shell. Calling `Shell_NotifyIconW`
locates the tray per Windows desktop. `test_win_tray_shell` and `test_win_notify_balloons`
create an isolated private desktop (`CreateDesktopW`) and run the host and test client
(`brosys_tray_client.exe`) inside it. Notifications and `TaskbarCreated` messages never leak
to the developer's interactive Explorer taskbar.

### macOS Test Oracles

macOS tests run read-only against native system frameworks:
- **SwitchAudioSource Oracle:** `test_mac_audio` verifies CoreAudio HAL device listings and
  volume changes against the `SwitchAudioSource` CLI tool (skips cleanly if not installed).
- **IOKit Assertions:** Power management tests verify sleep/wake handshakes, clamshell state,
  and power assertions against `pmset -g` and `ioreg`.
