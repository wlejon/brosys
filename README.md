# brosys

System-services substrate for a desktop environment built on the bro
runtime: power, audio, network, notifications and the tray. A standalone
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
  power.h           PowerService: devices, source, lid, capabilities, actions, inhibitors
  audio.h           AudioService: sinks/sources, defaults, volume/mute, change events
  network.h         NetworkService: connectivity, devices + IP config, primary, active connections, Wi-Fi scans
  notifications.h   NotificationServer: the desktop notification server
  tray.h            TrayHost: status-notifier items, menus, interaction
```

## Backends

| Service | Linux | Windows |
|---------|-------|---------|
| Power | UPower (real devices only: no DisplayDevice, no line power, `IsPresent`), logind (CanX, actions, Inhibit fds, PrepareForSleep/Shutdown) | GetSystemPowerStatus + battery device class IOCTLs, powrprof capabilities, privilege and policy checks, power requests, shutdown block reasons, power broadcasts |
| Audio | PipeWire client (nodes, device routes, `default` metadata, the way wpctl sets them) | Core Audio (IMMNotificationClient, per-endpoint volume callbacks; IPolicyConfig for set_default) |
| Network | NetworkManager (devices, IP configs, active connections, primary, LastScan-tracked scans) | IP Helper (adapters, primary from default routes + interface metrics, change notifications), connectivity hint / NLM, WLAN API (BSS list, IE-parsed security, scan completion) |
| Notifications | `org.freedesktop.Notifications`, spec 1.2 | shell mode: tray balloons (NIF_INFO) as notifications; alongside Explorer: local only |
| Tray | StatusNotifierWatcher + host, dbusmenu as data; WatcherClient role beside another watcher | shell mode: owns `Shell_TrayWnd` on its desktop (WM_COPYDATA NIM_*); alongside Explorer: role None |

Linux D-Bus goes through `src/linux/dbus/` (sd-bus): one `Connection` per
role with its own thread, `Value` for any D-Bus value, blocking, async and
deferred calls, matches, timers, name ownership, and object export with
generated introspection.

Windows tray hosting and notification interception need the process to be
the shell. `TrayMode::Auto` takes the shell role only when no
`Shell_TrayWnd` exists on the calling thread's desktop. `capabilities()` and
`status()` report which role applies. Toasts are not interceptable without
package identity.

## Building

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
| test_network_fake / _system | scripted NM on a private bus; read-only vs nmcli and `ip route` |
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

Isolating the shell-mode tray: `Shell_NotifyIcon` finds the tray per
desktop. The tests therefore create a private desktop, run the host's thread
and the client process on it, and announce TaskbarCreated only to that
desktop's windows. The user's Explorer tray never sees the icons.
