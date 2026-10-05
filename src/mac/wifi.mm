// CoreWLAN: Wi-Fi interfaces, scans, cached scan results and change events.
#import <CoreWLAN/CoreWLAN.h>

#include "mac/net_os.h"

#include <algorithm>
#include <cstdio>

@interface BrosysWifiObserver : NSObject <CWEventDelegate>
- (instancetype)initWithCallback:(std::function<void()>)fn;
- (void)stop;
@end

@implementation BrosysWifiObserver {
    std::function<void()> _fn;
    CWWiFiClient* _client;
}

- (instancetype)initWithCallback:(std::function<void()>)fn {
    if ((self = [super init])) {
        _fn = std::move(fn);
        _client = [[CWWiFiClient alloc] init];
        _client.delegate = self;
        for (CWEventType t : {CWEventTypePowerDidChange, CWEventTypeSSIDDidChange, CWEventTypeBSSIDDidChange,
                              CWEventTypeLinkDidChange, CWEventTypeModeDidChange})
            [_client startMonitoringEventWithType:t error:nil];
    }
    return self;
}

- (void)stop {
    [_client stopMonitoringAllEventsAndReturnError:nil];
    _client.delegate = nil;
}

- (void)fire {
    if (_fn) _fn();
}
- (void)powerStateDidChangeForWiFiInterfaceWithName:(NSString*)name { (void)name; [self fire]; }
- (void)ssidDidChangeForWiFiInterfaceWithName:(NSString*)name { (void)name; [self fire]; }
- (void)bssidDidChangeForWiFiInterfaceWithName:(NSString*)name { (void)name; [self fire]; }
- (void)linkDidChangeForWiFiInterfaceWithName:(NSString*)name { (void)name; [self fire]; }
- (void)modeDidChangeForWiFiInterfaceWithName:(NSString*)name { (void)name; [self fire]; }
@end

namespace brosys::mac {

namespace {

std::string utf8(NSString* s) { return s ? std::string(s.UTF8String) : std::string(); }

int band_ghz(CWChannel* ch) {
    switch (ch.channelBand) {
        case kCWChannelBand2GHz: return 2;
        case kCWChannelBand5GHz: return 5;
        case kCWChannelBand6GHz: return 6;
        default: return 0;
    }
}

WifiSecurity security_of(CWNetwork* n) {
    if ([n supportsSecurity:kCWSecurityWPA3Enterprise]) return WifiSecurity::Wpa3Enterprise;
    if ([n supportsSecurity:kCWSecurityWPA3Personal] || [n supportsSecurity:kCWSecurityWPA3Transition])
        return WifiSecurity::Wpa3Personal;
    if ([n supportsSecurity:kCWSecurityWPA2Enterprise]) return WifiSecurity::Wpa2Enterprise;
    if ([n supportsSecurity:kCWSecurityWPA2Personal]) return WifiSecurity::Wpa2Personal;
    if ([n supportsSecurity:kCWSecurityWPAEnterprise] || [n supportsSecurity:kCWSecurityWPAEnterpriseMixed])
        return WifiSecurity::WpaEnterprise;
    if ([n supportsSecurity:kCWSecurityWPAPersonal] || [n supportsSecurity:kCWSecurityWPAPersonalMixed])
        return WifiSecurity::WpaPersonal;
    if ([n supportsSecurity:kCWSecurityOWE] || [n supportsSecurity:kCWSecurityOWETransition]) return WifiSecurity::Owe;
    if ([n supportsSecurity:kCWSecurityWEP] || [n supportsSecurity:kCWSecurityDynamicWEP]) return WifiSecurity::Wep;
    if ([n supportsSecurity:kCWSecurityNone]) return WifiSecurity::Open;
    return WifiSecurity::Unknown;
}

std::string ssid_bytes(NSData* data, NSString* str) {
    if (data) return std::string(static_cast<const char*>(data.bytes), data.length);
    return utf8(str);
}

WifiAccessPoint access_point(CWNetwork* n, CWInterface* itf) {
    WifiAccessPoint ap;
    ap.device_id = utf8(itf.interfaceName);
    ap.ssid = ssid_bytes(n.ssidData, n.ssid);
    ap.bssid = normalize_mac(utf8(n.bssid));
    ap.rssi_dbm = static_cast<int32_t>(n.rssiValue);
    ap.strength_percent = static_cast<uint8_t>(std::clamp<long>((n.rssiValue + 100) * 2, 0, 100));
    if (n.wlanChannel) {
        ap.channel = static_cast<uint32_t>(n.wlanChannel.channelNumber);
        ap.frequency_mhz = wifi_frequency_mhz(ap.channel, band_ghz(n.wlanChannel));
    }
    ap.security = security_of(n);
    const std::string cur_bssid = normalize_mac(utf8(itf.bssid));
    ap.active = !ap.bssid.empty() ? ap.bssid == cur_bssid
                                  : (!ap.ssid.empty() && ap.ssid == ssid_bytes(itf.ssidData, itf.ssid) &&
                                     itf.wlanChannel && ap.channel == static_cast<uint32_t>(itf.wlanChannel.channelNumber));
    return ap;
}

CWInterface* find_interface(const std::string& name) {
    return [CWWiFiClient.sharedWiFiClient interfaceWithName:[NSString stringWithUTF8String:name.c_str()]];
}

std::vector<WifiAccessPoint> convert(NSSet<CWNetwork*>* nets, CWInterface* itf) {
    std::vector<WifiAccessPoint> out;
    for (CWNetwork* n in nets) out.push_back(access_point(n, itf));
    std::sort(out.begin(), out.end(), [](const WifiAccessPoint& a, const WifiAccessPoint& b) {
        return a.strength_percent != b.strength_percent ? a.strength_percent > b.strength_percent : a.bssid < b.bssid;
    });
    return out;
}

}  // namespace

uint32_t wifi_frequency_mhz(uint32_t channel, int band_ghz) {
    switch (band_ghz) {
        case 2: return channel == 14 ? 2484 : 2407 + 5 * channel;
        case 5: return 5000 + 5 * channel;
        case 6: return channel == 2 ? 5935 : 5950 + 5 * channel;
        default: return 0;
    }
}

std::string normalize_mac(const std::string& mac) {
    std::string out;
    unsigned parts[6];
    if (std::sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &parts[0], &parts[1], &parts[2], &parts[3], &parts[4],
                    &parts[5]) != 6)
        return {};
    char buf[18];
    std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", parts[0] & 0xff, parts[1] & 0xff, parts[2] & 0xff,
                  parts[3] & 0xff, parts[4] & 0xff, parts[5] & 0xff);
    return buf;
}

std::vector<WifiInterfaceInfo> wifi_interfaces() {
    std::vector<WifiInterfaceInfo> out;
    @autoreleasepool {
        for (CWInterface* itf in CWWiFiClient.sharedWiFiClient.interfaces) {
            WifiInterfaceInfo w;
            w.name = utf8(itf.interfaceName);
            w.power_on = itf.powerOn;
            w.ssid = ssid_bytes(itf.ssidData, itf.ssid);
            w.bssid = normalize_mac(utf8(itf.bssid));
            if (w.power_on && itf.wlanChannel) {
                w.rssi_dbm = static_cast<int32_t>(itf.rssiValue);
                w.tx_rate_mbps = itf.transmitRate;
                w.channel = static_cast<uint32_t>(itf.wlanChannel.channelNumber);
                w.frequency_mhz = wifi_frequency_mhz(w.channel, band_ghz(itf.wlanChannel));
            }
            out.push_back(std::move(w));
        }
    }
    return out;
}

WifiScanResult wifi_scan(const std::string& interface) {
    WifiScanResult r;
    @autoreleasepool {
        CWInterface* itf = find_interface(interface);
        if (!itf) {
            r.error = "no Wi-Fi interface " + interface;
            return r;
        }
        if (!itf.powerOn) {
            r.error = interface + ": Wi-Fi is powered off";
            return r;
        }
        NSError* err = nil;
        NSSet<CWNetwork*>* nets = [itf scanForNetworksWithName:nil error:&err];
        if (!nets) {
            r.error = interface + ": scan failed (" + utf8(err.domain) + " " + std::to_string(err.code) + ")";
            return r;
        }
        r.ok = true;
        r.access_points = convert(nets, itf);
    }
    return r;
}

std::vector<WifiAccessPoint> wifi_cached(const std::string& interface) {
    @autoreleasepool {
        CWInterface* itf = find_interface(interface);
        if (!itf) return {};
        return convert(itf.cachedScanResults, itf);
    }
}

std::shared_ptr<void> watch_wifi(std::function<void()> fn) {
    BrosysWifiObserver* obs = [[BrosysWifiObserver alloc] initWithCallback:std::move(fn)];
    void* retained = (__bridge_retained void*)obs;
    return std::shared_ptr<void>(retained, [](void* p) {
        BrosysWifiObserver* o = (__bridge_transfer BrosysWifiObserver*)p;
        [o stop];
    });
}

}  // namespace brosys::mac
