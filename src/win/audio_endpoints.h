// Core Audio helpers for the Windows audio service: reading one endpoint
// into an AudioDevice, and the endpoint-volume / device-notification COM
// sinks (which only forward to a callback; they never block).
#pragma once

#include "brosys/audio.h"

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace brosys::win {

using Microsoft::WRL::ComPtr;

std::string endpoint_id(IMMDevice* dev);
// Fills everything except is_default from the endpoint and its volume.
bool read_endpoint(IMMDevice* dev, AudioDirection dir, IAudioEndpointVolume* vol, AudioDevice& out);

struct VolumeSample {
    float master = 0;
    bool muted = false;
    std::vector<float> channels;
};

class VolumeSink final : public IAudioEndpointVolumeCallback {
public:
    explicit VolumeSink(std::function<void(const VolumeSample&)> fn) : fn_(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override;

private:
    std::function<void(const VolumeSample&)> fn_;
    std::atomic<ULONG> refs_{1};
};

class DeviceSink final : public IMMNotificationClient {
public:
    // kind: 0 = device set changed, 1 = default changed (flow), 2 = property changed (id)
    using Fn = std::function<void(int kind, EDataFlow flow, const std::wstring& id)>;
    explicit DeviceSink(Fn fn) : fn_(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override;

private:
    Fn fn_;
    std::atomic<ULONG> refs_{1};
};

// The undocumented interface the Sound control panel uses to set default
// endpoints (stable since Windows 7). Only SetDefaultEndpoint is called;
// the other slots exist to give it the right vtable offset.
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR device_id, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
inline constexpr CLSID kPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

}  // namespace brosys::win
