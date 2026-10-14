// Enum names and small shared helpers of the public API.
#include "brosys/brosys.h"

namespace brosys {

const char* version_string() { return "0.2.0"; }

const char* to_string(Availability a) {
    switch (a) {
        case Availability::No: return "no";
        case Availability::Yes: return "yes";
        case Availability::NeedsAuth: return "needs-auth";
        case Availability::Unknown: break;
    }
    return "unknown";
}

Availability PowerCapabilities::of(PowerAction a) const {
    switch (a) {
        case PowerAction::Suspend: return suspend;
        case PowerAction::Hibernate: return hibernate;
        case PowerAction::HybridSleep: return hybrid_sleep;
        case PowerAction::Reboot: return reboot;
        case PowerAction::PowerOff: return power_off;
        case PowerAction::Lock: return lock;
    }
    return Availability::Unknown;
}

const char* to_string(PowerSource s) {
    switch (s) {
        case PowerSource::AC: return "ac";
        case PowerSource::Battery: return "battery";
        case PowerSource::Unknown: break;
    }
    return "unknown";
}

const char* to_string(PowerDeviceKind k) {
    switch (k) {
        case PowerDeviceKind::Battery: return "battery";
        case PowerDeviceKind::Ups: return "ups";
        case PowerDeviceKind::Mouse: return "mouse";
        case PowerDeviceKind::Keyboard: return "keyboard";
        case PowerDeviceKind::Headset: return "headset";
        case PowerDeviceKind::Phone: return "phone";
        case PowerDeviceKind::Tablet: return "tablet";
        case PowerDeviceKind::Gamepad: return "gamepad";
        case PowerDeviceKind::Other: break;
    }
    return "other";
}

const char* to_string(BatteryState s) {
    switch (s) {
        case BatteryState::Charging: return "charging";
        case BatteryState::Discharging: return "discharging";
        case BatteryState::Empty: return "empty";
        case BatteryState::FullyCharged: return "fully-charged";
        case BatteryState::PendingCharge: return "pending-charge";
        case BatteryState::PendingDischarge: return "pending-discharge";
        case BatteryState::Unknown: break;
    }
    return "unknown";
}

const char* to_string(BatteryTechnology t) {
    switch (t) {
        case BatteryTechnology::LithiumIon: return "lithium-ion";
        case BatteryTechnology::LithiumPolymer: return "lithium-polymer";
        case BatteryTechnology::LithiumIronPhosphate: return "lithium-iron-phosphate";
        case BatteryTechnology::LeadAcid: return "lead-acid";
        case BatteryTechnology::NickelCadmium: return "nickel-cadmium";
        case BatteryTechnology::NickelMetalHydride: return "nickel-metal-hydride";
        case BatteryTechnology::Unknown: break;
    }
    return "unknown";
}

const char* to_string(PowerAction a) {
    switch (a) {
        case PowerAction::Suspend: return "suspend";
        case PowerAction::Hibernate: return "hibernate";
        case PowerAction::HybridSleep: return "hybrid-sleep";
        case PowerAction::Reboot: return "reboot";
        case PowerAction::PowerOff: return "power-off";
        case PowerAction::Lock: return "lock";
    }
    return "unknown";
}

const char* to_string(AudioDirection d) { return d == AudioDirection::Output ? "output" : "input"; }

const char* to_string(AudioDeviceState s) {
    switch (s) {
        case AudioDeviceState::Active: return "active";
        case AudioDeviceState::Unplugged: return "unplugged";
        case AudioDeviceState::Disabled: return "disabled";
        case AudioDeviceState::NotPresent: return "not-present";
    }
    return "unknown";
}

const char* to_string(Connectivity c) {
    switch (c) {
        case Connectivity::None: return "none";
        case Connectivity::Portal: return "portal";
        case Connectivity::Limited: return "limited";
        case Connectivity::Full: return "full";
        case Connectivity::Unknown: break;
    }
    return "unknown";
}

const char* to_string(LinkType t) {
    switch (t) {
        case LinkType::Ethernet: return "ethernet";
        case LinkType::WiFi: return "wifi";
        case LinkType::Cellular: return "cellular";
        case LinkType::Loopback: return "loopback";
        case LinkType::Bridge: return "bridge";
        case LinkType::Vpn: return "vpn";
        case LinkType::Tunnel: return "tunnel";
        case LinkType::Virtual: return "virtual";
        case LinkType::Other: return "other";
        case LinkType::Unknown: break;
    }
    return "unknown";
}

const char* to_string(LinkState s) {
    switch (s) {
        case LinkState::Unavailable: return "unavailable";
        case LinkState::Disconnected: return "disconnected";
        case LinkState::Connecting: return "connecting";
        case LinkState::Connected: return "connected";
        case LinkState::Disconnecting: return "disconnecting";
        case LinkState::Unknown: break;
    }
    return "unknown";
}

const char* to_string(WifiSecurity s) {
    switch (s) {
        case WifiSecurity::Open: return "open";
        case WifiSecurity::Wep: return "wep";
        case WifiSecurity::WpaPersonal: return "wpa-personal";
        case WifiSecurity::Wpa2Personal: return "wpa2-personal";
        case WifiSecurity::Wpa3Personal: return "wpa3-personal";
        case WifiSecurity::WpaEnterprise: return "wpa-enterprise";
        case WifiSecurity::Wpa2Enterprise: return "wpa2-enterprise";
        case WifiSecurity::Wpa3Enterprise: return "wpa3-enterprise";
        case WifiSecurity::Owe: return "owe";
        case WifiSecurity::Unknown: break;
    }
    return "unknown";
}

const char* to_string(CloseReason r) {
    switch (r) {
        case CloseReason::Expired: return "expired";
        case CloseReason::Dismissed: return "dismissed";
        case CloseReason::Closed: return "closed";
        case CloseReason::Undefined: break;
    }
    return "undefined";
}

const char* to_string(Urgency u) {
    switch (u) {
        case Urgency::Low: return "low";
        case Urgency::Critical: return "critical";
        case Urgency::Normal: break;
    }
    return "normal";
}

const char* to_string(TrayRole r) {
    switch (r) {
        case TrayRole::Watcher: return "watcher";
        case TrayRole::WatcherClient: return "watcher-client";
        case TrayRole::Shell: return "shell";
        case TrayRole::None: break;
    }
    return "none";
}

const char* to_string(TrayItemStatus s) {
    switch (s) {
        case TrayItemStatus::Passive: return "passive";
        case TrayItemStatus::NeedsAttention: return "needs-attention";
        case TrayItemStatus::Active: break;
    }
    return "active";
}

int32_t parse_signal(std::string_view name) {
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
    if (name.empty()) return 0;

    std::string up;
    up.reserve(name.size());
    bool all_digits = true;
    for (char c : name) {
        if (c < '0' || c > '9') all_digits = false;
        if (c >= 'a' && c <= 'z') up.push_back(static_cast<char>(c - 'a' + 'A'));
        else up.push_back(c);
    }

    if (all_digits) {
        int32_t v = 0;
        for (char c : up) {
            v = v * 10 + (c - '0');
            if (v > 64) return 0;
        }
        return v > 0 ? v : 0;
    }

    if (up.rfind("SIG", 0) == 0) {
        up.erase(0, 3);
    }
    if (up == "TERM") return signal_num::SIGTERM_NUM;
    if (up == "KILL") return signal_num::SIGKILL_NUM;
    if (up == "INT")  return signal_num::SIGINT_NUM;
    if (up == "HUP")  return signal_num::SIGHUP_NUM;
    if (up == "STOP") return signal_num::SIGSTOP_NUM;
    if (up == "CONT") return signal_num::SIGCONT_NUM;
    return 0;
}

std::unique_ptr<ProcessService> ProcessService::create(std::string*) {
    return std::make_unique<ProcessService>();
}

std::vector<ProcessInfo> ProcessService::list() const {
    return list_processes();
}

Result ProcessService::kill(int32_t pid, int32_t signal) {
    return kill_process(pid, signal);
}

}  // namespace brosys
