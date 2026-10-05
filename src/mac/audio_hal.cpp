// CoreAudio HAL reads and writes. Volume is the virtual main volume
// (kAudioHardwareServiceDeviceProperty_VirtualMainVolume): the value the menu
// bar slider, System Settings and `osascript -e "output volume of (get volume
// settings)"` show, whether the device has a main volume control or only
// per-channel ones.
#include "mac/audio_hal.h"
#include "mac/cf.h"

#include <AudioToolbox/AudioServices.h>

#include <algorithm>

namespace brosys::mac {

namespace {

AudioObjectPropertyScope scope_of(AudioDirection d) {
    return d == AudioDirection::Output ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput;
}

AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel,
                                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                                AudioObjectPropertyElement element = kAudioObjectPropertyElementMain) {
    return AudioObjectPropertyAddress{sel, scope, element};
}

template <class T>
bool get(AudioObjectID o, const AudioObjectPropertyAddress& a, T& out) {
    if (!AudioObjectHasProperty(o, &a)) return false;
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(o, &a, 0, nullptr, &size, &out) == noErr && size == sizeof(T);
}

std::string get_string(AudioObjectID o, AudioObjectPropertySelector sel) {
    CFStringRef s = nullptr;
    auto a = addr(sel);
    if (!get(o, a, s) || !s) return {};
    std::string out = to_utf8(s);
    CFRelease(s);
    return out;
}

bool settable(AudioObjectID o, const AudioObjectPropertyAddress& a) {
    Boolean b = false;
    return AudioObjectHasProperty(o, &a) && AudioObjectIsPropertySettable(o, &a, &b) == noErr && b;
}

UInt32 stream_count(AudioObjectID o, AudioObjectPropertyScope scope) {
    auto a = addr(kAudioDevicePropertyStreams, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(o, &a, 0, nullptr, &size) != noErr) return 0;
    return size / sizeof(AudioStreamID);
}

UInt32 channel_count(AudioObjectID o, AudioObjectPropertyScope scope) {
    auto a = addr(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(o, &a, 0, nullptr, &size) != noErr || size < sizeof(AudioBufferList)) return 0;
    std::vector<uint8_t> buf(size);
    if (AudioObjectGetPropertyData(o, &a, 0, nullptr, &size, buf.data()) != noErr) return 0;
    auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
    UInt32 n = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) n += list->mBuffers[i].mNumberChannels;
    return n;
}

std::string form_factor(AudioObjectID o, AudioDirection dir, bool both_directions) {
    UInt32 source = 0;
    if (get(o, addr(kAudioDevicePropertyDataSource, scope_of(dir)), source)) {
        switch (source) {
            case 'ispk': return "speakers";
            case 'hdpn': return "headphones";
            case 'imic':
            case 'emic': return "microphone";
            case 'lnot':
            case 'line': return "line";
            case 'spdf': return "spdif";
            default: break;
        }
    }
    UInt32 transport = 0;
    get(o, addr(kAudioDevicePropertyTransportType), transport);
    switch (transport) {
        case kAudioDeviceTransportTypeBuiltIn: return dir == AudioDirection::Output ? "speakers" : "microphone";
        case kAudioDeviceTransportTypeHDMI: return "hdmi";
        case kAudioDeviceTransportTypeDisplayPort: return "displayport";
        case kAudioDeviceTransportTypeAirPlay: return "airplay";
        case kAudioDeviceTransportTypeBluetooth:
        case kAudioDeviceTransportTypeBluetoothLE:
            if (both_directions) return "headset";
            return dir == AudioDirection::Output ? "headphones" : "microphone";
        default: return {};
    }
}

bool read_device(AudioObjectID o, AudioDirection dir, bool both, AudioDevice& d) {
    const std::string uid = device_uid(o);
    if (uid.empty()) return false;
    const AudioObjectPropertyScope scope = scope_of(dir);
    d.id = audio_id(uid, dir);
    d.description = get_string(o, kAudioObjectPropertyName);
    d.device_name = get_string(o, kAudioObjectPropertyManufacturer);
    d.form_factor = form_factor(o, dir, both);
    d.direction = dir;
    d.state = AudioDeviceState::Active;
    d.native_id = o;

    auto vm = addr(kAudioHardwareServiceDeviceProperty_VirtualMainVolume, scope);
    Float32 v = 0;
    d.has_volume = settable(o, vm) && get(o, vm, v);
    d.volume = d.has_volume ? std::clamp(static_cast<float>(v), 0.0f, 1.0f) : 1.0f;

    // Per-channel volumes only when every channel has its own control.
    const UInt32 channels = channel_count(o, scope);
    for (UInt32 ch = 1; ch <= channels; ++ch) {
        Float32 cv = 0;
        if (!get(o, addr(kAudioDevicePropertyVolumeScalar, scope, ch), cv)) {
            d.channel_volumes.clear();
            break;
        }
        d.channel_volumes.push_back(static_cast<float>(cv));
    }

    UInt32 mute = 0;
    if (get(o, addr(kAudioDevicePropertyMute, scope), mute)) {
        d.muted = mute != 0;
    } else if (channels > 0) {
        bool all = true;
        for (UInt32 ch = 1; ch <= channels && all; ++ch) all = get(o, addr(kAudioDevicePropertyMute, scope, ch), mute) && mute;
        d.muted = all;
    }
    return true;
}

}  // namespace

std::string audio_id(const std::string& uid, AudioDirection direction) {
    return (direction == AudioDirection::Output ? "out:" : "in:") + uid;
}

std::string device_uid(AudioObjectID object) { return get_string(object, kAudioDevicePropertyDeviceUID); }

std::vector<AudioObjectID> hal_device_objects() {
    auto a = addr(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return {};
    ids.resize(size / sizeof(AudioObjectID));
    return ids;
}

std::vector<HalDevice> read_hal_devices() {
    std::vector<HalDevice> out;
    for (AudioObjectID o : hal_device_objects()) {
        UInt32 hidden = 0;
        if (get(o, addr(kAudioDevicePropertyIsHidden), hidden) && hidden) continue;
        const bool has_out = stream_count(o, kAudioObjectPropertyScopeOutput) > 0;
        const bool has_in = stream_count(o, kAudioObjectPropertyScopeInput) > 0;
        for (AudioDirection dir : {AudioDirection::Output, AudioDirection::Input}) {
            if (!(dir == AudioDirection::Output ? has_out : has_in)) continue;
            HalDevice h;
            h.object = o;
            if (read_device(o, dir, has_out && has_in, h.device)) out.push_back(std::move(h));
        }
    }
    return out;
}

AudioObjectID default_device(AudioDirection direction) {
    AudioObjectID o = kAudioObjectUnknown;
    get(kAudioObjectSystemObject,
        addr(direction == AudioDirection::Output ? kAudioHardwarePropertyDefaultOutputDevice
                                                 : kAudioHardwarePropertyDefaultInputDevice),
        o);
    return o;
}

Result hal_set_volume(AudioObjectID o, AudioDirection direction, float volume) {
    auto a = addr(kAudioHardwareServiceDeviceProperty_VirtualMainVolume, scope_of(direction));
    if (!settable(o, a)) return Result::failure("the device has no settable volume");
    Float32 v = std::clamp(volume, 0.0f, 1.0f);
    OSStatus st = AudioObjectSetPropertyData(o, &a, 0, nullptr, sizeof v, &v);
    if (st != noErr) return Result::failure(os_status("AudioObjectSetPropertyData(VirtualMainVolume)", st));
    return Result::success();
}

Result hal_set_mute(AudioObjectID o, AudioDirection direction, bool muted) {
    const auto scope = scope_of(direction);
    UInt32 m = muted ? 1 : 0;
    auto main = addr(kAudioDevicePropertyMute, scope);
    if (settable(o, main)) {
        OSStatus st = AudioObjectSetPropertyData(o, &main, 0, nullptr, sizeof m, &m);
        if (st != noErr) return Result::failure(os_status("AudioObjectSetPropertyData(Mute)", st));
        return Result::success();
    }
    const UInt32 channels = channel_count(o, scope);
    bool any = false;
    for (UInt32 ch = 1; ch <= channels; ++ch) {
        auto a = addr(kAudioDevicePropertyMute, scope, ch);
        if (!settable(o, a)) continue;
        OSStatus st = AudioObjectSetPropertyData(o, &a, 0, nullptr, sizeof m, &m);
        if (st != noErr) return Result::failure(os_status("AudioObjectSetPropertyData(Mute, channel)", st));
        any = true;
    }
    if (!any) return Result::failure("the device has no mute control");
    return Result::success();
}

Result hal_set_default(AudioObjectID o, AudioDirection direction) {
    auto a = addr(direction == AudioDirection::Output ? kAudioHardwarePropertyDefaultOutputDevice
                                                      : kAudioHardwarePropertyDefaultInputDevice);
    OSStatus st = AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, sizeof o, &o);
    if (st != noErr) return Result::failure(os_status("AudioObjectSetPropertyData(DefaultDevice)", st));
    return Result::success();
}

std::vector<AudioObjectPropertyAddress> watched_device_properties() {
    std::vector<AudioObjectPropertyAddress> out = {
        addr(kAudioObjectPropertyName),
        addr(kAudioObjectPropertyManufacturer),
        addr(kAudioDevicePropertyDeviceIsAlive),
        addr(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeWildcard),
    };
    for (auto scope : {kAudioObjectPropertyScopeOutput, kAudioObjectPropertyScopeInput}) {
        out.push_back(addr(kAudioHardwareServiceDeviceProperty_VirtualMainVolume, scope));
        out.push_back(addr(kAudioDevicePropertyVolumeScalar, scope, kAudioObjectPropertyElementWildcard));
        out.push_back(addr(kAudioDevicePropertyMute, scope, kAudioObjectPropertyElementWildcard));
        out.push_back(addr(kAudioDevicePropertyDataSource, scope));
    }
    return out;
}

}  // namespace brosys::mac
