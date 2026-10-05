// initguid first: this translation unit defines the PROPERTYKEYs that
// mmdeviceapi.h only declares (PKEY_AudioEndpoint_FormFactor has no lib).
#include <windows.h>
#include <initguid.h>

#include "win/audio_endpoints.h"

#include "win/util.h"

#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

namespace brosys::win {

namespace {

std::string prop_string(IPropertyStore* props, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string out;
    if (SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = to_utf8(v.pwszVal);
    PropVariantClear(&v);
    return out;
}

const char* form_factor_name(UINT ff) {
    switch (ff) {
        case RemoteNetworkDevice: return "network";
        case Speakers: return "speakers";
        case LineLevel: return "line";
        case Headphones: return "headphones";
        case Microphone: return "microphone";
        case Headset: return "headset";
        case Handset: return "handset";
        case UnknownDigitalPassthrough: return "digital";
        case SPDIF: return "spdif";
        case DigitalAudioDisplayDevice: return "hdmi";
        default: return "";
    }
}

}  // namespace

std::string endpoint_id(IMMDevice* dev) {
    LPWSTR id = nullptr;
    std::string out;
    if (dev && SUCCEEDED(dev->GetId(&id)) && id) out = to_utf8(id);
    if (id) CoTaskMemFree(id);
    return out;
}

bool read_endpoint(IMMDevice* dev, AudioDirection dir, IAudioEndpointVolume* vol, AudioDevice& out) {
    out = AudioDevice{};
    out.id = endpoint_id(dev);
    if (out.id.empty()) return false;
    out.direction = dir;
    DWORD state = 0;
    if (SUCCEEDED(dev->GetState(&state))) {
        switch (state) {
            case DEVICE_STATE_ACTIVE: out.state = AudioDeviceState::Active; break;
            case DEVICE_STATE_UNPLUGGED: out.state = AudioDeviceState::Unplugged; break;
            case DEVICE_STATE_DISABLED: out.state = AudioDeviceState::Disabled; break;
            default: out.state = AudioDeviceState::NotPresent; break;
        }
    }
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
        out.description = prop_string(props.Get(), PKEY_Device_FriendlyName);
        out.device_name = prop_string(props.Get(), PKEY_DeviceInterface_FriendlyName);
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_AudioEndpoint_FormFactor, &v)) && v.vt == VT_UI4)
            out.form_factor = form_factor_name(v.ulVal);
        PropVariantClear(&v);
    }
    out.has_volume = vol != nullptr;
    if (vol) {
        float level = 0;
        if (SUCCEEDED(vol->GetMasterVolumeLevelScalar(&level))) out.volume = level;
        BOOL muted = FALSE;
        if (SUCCEEDED(vol->GetMute(&muted))) out.muted = muted != FALSE;
        UINT channels = 0;
        if (SUCCEEDED(vol->GetChannelCount(&channels)))
            for (UINT c = 0; c < channels; ++c) {
                float cv = 0;
                if (SUCCEEDED(vol->GetChannelVolumeLevelScalar(c, &cv))) out.channel_volumes.push_back(cv);
            }
    }
    return true;
}

// ---------------------------------------------------------------- VolumeSink

HRESULT VolumeSink::QueryInterface(REFIID riid, void** ppv) {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
        *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG VolumeSink::Release() {
    ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
}

HRESULT VolumeSink::OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) {
    if (!data) return S_OK;
    VolumeSample s;
    s.master = data->fMasterVolume;
    s.muted = data->bMuted != FALSE;
    for (UINT c = 0; c < data->nChannels; ++c) s.channels.push_back(data->afChannelVolumes[c]);
    if (fn_) fn_(s);
    return S_OK;
}

// ---------------------------------------------------------------- DeviceSink

HRESULT DeviceSink::QueryInterface(REFIID riid, void** ppv) {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *ppv = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG DeviceSink::Release() {
    ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
}

HRESULT DeviceSink::OnDeviceStateChanged(LPCWSTR id, DWORD) {
    fn_(0, eAll, id ? id : L"");
    return S_OK;
}
HRESULT DeviceSink::OnDeviceAdded(LPCWSTR id) {
    fn_(0, eAll, id ? id : L"");
    return S_OK;
}
HRESULT DeviceSink::OnDeviceRemoved(LPCWSTR id) {
    fn_(0, eAll, id ? id : L"");
    return S_OK;
}
HRESULT DeviceSink::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) {
    if (role == eConsole) fn_(1, flow, id ? id : L"");
    return S_OK;
}
HRESULT DeviceSink::OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) {
    if (IsEqualPropertyKey(key, PKEY_Device_FriendlyName) || IsEqualPropertyKey(key, PKEY_DeviceInterface_FriendlyName) ||
        IsEqualPropertyKey(key, PKEY_AudioEndpoint_FormFactor))
        fn_(2, eAll, id ? id : L"");
    return S_OK;
}

}  // namespace brosys::win
