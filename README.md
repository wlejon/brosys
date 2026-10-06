# brosys

[![CI](https://github.com/wlejon/brosys/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/brosys/actions/workflows/ci.yml)

System-services substrate for a desktop environment built on the
[bro](https://github.com/wlejon/bro) runtime: power, audio, network, Bluetooth, notifications and the tray. A standalone
C++20 library: no dependency on bro or bronze, no JS binding, its own CMake
and ctest.

## Model

Each service is created on its own and owns its backend thread(s). Backends
push value snapshots into the service's `MessageQueue` (`event_queue.h`);
the host drains it on its own thread. No callback runs host code except the
queue's optional wake hook. Queries return the latest snapshot; commands
return a `Result`, and their effect shows up as events. There are no mocks or
test setters in the public API.

```cpp
std::string err;
auto power = brosys::PowerService::create({}, &err);   // first PowerChanged already queued
power->events().set_wake([] { /* post to the host loop */ });
// on the host thread:
for (auto& e : power->events().drain())
    if (auto* c = std::get_if<brosys::PowerChanged>(&e)) render_battery(c->state);
```

```
include/brosys/
  common.h          Result, Image (straight RGBA8), Availability
  event_queue.h     MessageQueue<T>
  power.h           PowerService: devices, source, lid, capabilities, actions, inhibitors;
                    ScreenSaverServer: org.freedesktop.ScreenSaver mapped onto logind (Linux)
  audio.h           AudioService: sinks/sources, defaults, volume/mute, change events
  network.h         NetworkService: connectivity, devices + IP config, primary, active connections,
                    Wi-Fi scans; Wi-Fi and VPN connect/disconnect (Linux)
  bluetooth.h       BluetoothService: adapters, devices, power, discovery, pair/connect/remove (Linux)
  notifications.h   NotificationServer: the desktop notification server, history, Do-Not-Disturb
  tray.h            TrayHost: status-notifier items, menus, interaction
```

## Backends

| Service | Linux | Windows | macOS |
|---------|-------|---------|-------|
| Power | UPower (real devices only: no DisplayDevice, no line power, `IsPresent`), logind (CanX, actions, Inhibit fds, PrepareForSleep/Shutdown) | GetSystemPowerStatus + battery device class IOCTLs, powrprof capabilities, privilege and policy checks, power requests, shutdown block reasons, power broadcasts | IOKit power sources + AppleSmartBattery, clamshell, IORegisterForSystemPower (Delay inhibitors hold the will-sleep ack), IOPM assertions; reboot/power off via loginwindow AppleEvents (NeedsAuth until Automation consent); no hibernate, no shutdown inhibitors |
| Audio | PipeWire client (nodes, device routes, `default` metadata, the way wpctl sets them) | Core Audio (IMMNotificationClient, per-endpoint volume callbacks; IPolicyConfig for set_default) | CoreAudio HAL (property listener blocks, virtual main volume, `out:`/`in:` + device UID ids) |
| Network | NetworkManager (devices, IP configs, active connections, primary, LastScan-tracked scans) | IP Helper (adapters, primary from default routes + interface metrics, change notifications), connectivity hint / NLM, WLAN API (BSS list, IE-parsed security, scan completion) | SystemConfiguration (current set's services, dynamic store, primary) + getifaddrs, Network.framework path monitor, CoreWLAN (SSID/BSSID need Location permission) |
| Network control | NetworkManager: `connect_wifi` (AddAndActivateConnection with the SSID and passphrase), `disconnect`, `connect_vpn` / `disconnect_vpn` by profile name or UUID | fails with the reason (not implemented) | fails with the reason (not implemented) |
| Bluetooth | BlueZ 5 on the system bus (ObjectManager, Adapter1, Device1) | `create()` fails with the reason (no backend) | `create()` fails with the reason (no backend) |
| Screen saver | exports `org.freedesktop.ScreenSaver` (Inhibit/UnInhibit/GetActive/SimulateUserActivity) on the session bus, holding a logind idle inhibitor while any inhibition is held (`PowerConfig::export_screensaver`) | `create()` fails with the reason | `create()` fails with the reason |
| Notifications | `org.freedesktop.Notifications`, spec 1.2 | shell mode: tray balloons (NIF_INFO) as notifications; alongside Explorer: local only | local only (Notification Center has no server role) |
| History, Do-Not-Disturb | every platform: posted notifications are kept in `history()` after they close; with DND on they still post, marked `popup_suppressed` | same | same |
| Tray | StatusNotifierWatcher + host, dbusmenu as data; WatcherClient role beside another watcher | shell mode: owns `Shell_TrayWnd` on its desktop (WM_COPYDATA NIM_*); alongside Explorer: role None | role None (menu-bar extras cannot be hosted by another process) |

Linux D-Bus goes through `src/linux/dbus/` (sd-bus): one `Connection` per
role with its own thread, `Value` for any D-Bus value, blocking, async and
deferred calls, matches, timers, name ownership, and object export with
generated introspection.

Windows tray hosting and notification interception need the process to be
the shell. `TrayMode::Auto` takes the shell role only when no
`Shell_TrayWnd` exists on the calling thread's desktop. `capabilities()` and
`status()` report which role applies. Toasts are not interceptable without
package identity.

macOS has neither role to take, so it reports both honestly instead of
faking them. Nor does brosys post to Notification Center or create an
NSStatusItem of its own. The host renders the notifications it posts, so
forwarding them would show each one twice, and UNUserNotificationCenter
needs an app bundle plus user authorization. A status item of the host's
own is a tray *client*, which bro gets from SDL3 (`SDL_CreateTray`) on the
main thread AppKit requires. Objective-C++ is confined to `src/mac/*.mm`
(CoreWLAN, Network.framework, NSWorkspace) behind C++ headers.

## Building

There are no sibling repos to fetch: brosys needs CMake 3.24+, a C++20
compiler and the OS (on Linux, sd-bus from libsystemd and optionally PipeWire).

Windows (Visual Studio generator, one build dir):

```bash
cmake -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Linux (GCC 12+; Debian package names):

```bash
sudo apt install libsystemd-dev libpipewire-0.3-dev    # PipeWire optional: BROSYS_WITH_PIPEWIRE
# test oracles (missing ones skip with a reason):
sudo apt install dbus-daemon libnotify-bin libglib2.0-bin umockdev libumockdev-dev upower \
    pipewire wireplumber pipewire-pulse network-manager libayatana-appindicator3-dev xvfb
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

macOS (Apple clang, macOS 13+; Ninja from Homebrew):

```bash
brew install ninja switchaudio-osx    # SwitchAudioSource: test_mac_audio's oracle (skips without it)
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

## Tests

Real ctests: no `assert()`, and failures count in every configuration. Exit
77 is a skip, used only when a service or tool is absent, and the test prints
the reason. The tests are read-only toward the machine. Mutations run only
in isolation.

Linux. Everything that writes runs on a private dbus-daemon (`tests/linux/support`):

| Test | Oracle |
|------|--------|
| test_dbus_layer | busctl, gdbus against exported objects; names, signals, fds, disconnect |
| test_notify_server | notify-send (actions, --wait, hints, replace, expiry), gdbus calls, gdbus monitor |
| test_tray_sni | a real SNI item process (`brosys_sni_item`), a second watcher process, busctl / gdbus |
| test_tray_ayatana | a libayatana-appindicator3 GTK app under Xvfb |
| test_power_mock | real upowerd under umockdev (battery, AC, add/remove) on a private bus, plus `upower -d` |
| test_power_system | read-only: `upower -d`, busctl CanX, loginctl, systemd-inhibit --list |
| test_network_fake / _system | scripted NM on a private bus (including Wi-Fi and VPN activation); read-only vs nmcli and `ip route` |
| test_bluetooth | scripted BlueZ on a private bus: adapters, devices, power, discovery, connect/pair/remove and their events |
| test_screensaver | the exported `org.freedesktop.ScreenSaver` driven with gdbus, against a scripted logind on a private system bus |
| test_network_wifi | opt-in (env, sudo): mac80211_hwsim + hostapd AP vs nmcli; see the file header |
| test_audio_private | private PipeWire + WirePlumber + null sinks: set through the library, check with wpctl/pactl, and the reverse |
| test_audio_session | read-only vs wpctl / pactl on the user's session |
| test_desktop_parse, test_system_models | pure translation layers |

Windows:

| Test | Oracle |
|------|--------|
| test_win_power | GetSystemPowerStatus, WMI Win32_Battery, `powercfg /a`, `whoami /priv`, ShutdownBlockReasonQuery |
| test_win_audio | the MMDevices endpoint store in the registry, Core Audio queried directly |
| test_win_network | WMI MSFT_NetIPAddress / NetRoute / NetIPInterface / NetAdapter, GetBestInterfaceEx, NLM, `netsh wlan` |
| test_win_tray_shell | a real `Shell_NotifyIconW` client process (`brosys_tray_client`) on a private desktop |
| test_win_notify_balloons | balloons from that client, with the NIN_BALLOON* replies it receives |
| test_win_shell_alongside | the real Explorer, read-only (Auto resolves to None, nothing created) |
| test_win_tray_wire | wire layouts, icon conversion, balloon mapping |

macOS. Everything here is read-only; the only writes are same-value ones:

| Test | Oracle |
|------|--------|
| test_mac_power | `pmset -g batt`, ioreg AppleClamshellState, `pmset -g` + the console user, `pmset -g assertions` for inhibitors; the will-sleep / Delay handshake through a test seam with synthetic IOKit messages |
| test_mac_audio | `SwitchAudioSource -a/-c -f json`, osascript `get volume settings`, `system_profiler SPAudioDataType`; Added/Removed via a private aggregate device only this process sees |
| test_mac_network | `scutil --nwi`, scutil Global IPv4 / DNS, `networksetup -listallhardwareports` / `-listnetworkserviceorder` / `-getairportpower`, ifconfig, `scutil -r` |
| test_mac_shell | tray role None in every mode, local-only notifications fully working |

Isolating the shell-mode tray: `Shell_NotifyIcon` finds the tray per
desktop. The tests therefore create a private desktop, run the host's thread
and the client process on it, and announce TaskbarCreated only to that
desktop's windows. The user's Explorer tray never sees the icons.
