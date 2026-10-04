#if defined(_WIN32)

#include "audio_internal.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <iostream>

#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace brosys {

namespace {

class ComScope {
public:
    ComScope() {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    ~ComScope() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
private:
    HRESULT hr_;
};

std::string get_device_property(IPropertyStore* props, const PROPERTYKEY& key) {
    if (!props) return "";
    PROPVARIANT var;
    PropVariantInit(&var);
    std::string res;
    if (SUCCEEDED(props->GetValue(key, &var))) {
        if (var.vt == VT_LPWSTR && var.pwszVal) {
            int len = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                res.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, res.data(), len, nullptr, nullptr);
            }
        }
    }
    PropVariantClear(&var);
    return res;
}

std::string get_device_id(IMMDevice* device) {
    if (!device) return "";
    LPWSTR str_id = nullptr;
    std::string res;
    if (SUCCEEDED(device->GetId(&str_id)) && str_id) {
        int len = WideCharToMultiByte(CP_UTF8, 0, str_id, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            res.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, str_id, -1, res.data(), len, nullptr, nullptr);
        }
        CoTaskMemFree(str_id);
    }
    return res;
}

class EndpointCallbackHandler : public IAudioEndpointVolumeCallback {
public:
    EndpointCallbackHandler(std::string id, EndpointDirection dir, AudioManager::VolumeCallback cb)
        : id_(std::move(id)), dir_(dir), cb_(std::move(cb)), ref_count_(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&ref_count_);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ul = InterlockedDecrement(&ref_count_);
        if (ul == 0) {
            delete this;
        }
        return ul;
    }

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify) override {
        if (cb_ && pNotify) {
            VolumeNotification n;
            n.endpoint_id = id_;
            n.direction = dir_;
            n.volume = pNotify->fMasterVolume;
            n.is_muted = (pNotify->bMuted != FALSE);
            cb_(n);
        }
        return S_OK;
    }

private:
    std::string id_;
    EndpointDirection dir_;
    AudioManager::VolumeCallback cb_;
    LONG ref_count_;
};

} // namespace

class WindowsAudioBackend : public IAudioBackend {
public:
    WindowsAudioBackend() {
        com_ = std::make_unique<ComScope>();
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                         CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator),
                         reinterpret_cast<void**>(enumerator_.GetAddressOf()));
    }

    ~WindowsAudioBackend() override {
        unregister_volume_callback();
    }

    float get_master_volume() override {
        auto vol = get_default_endpoint_volume(eRender);
        if (!vol) return 0.0f;
        float level = 0.0f;
        if (SUCCEEDED(vol->GetMasterVolumeLevelScalar(&level))) {
            return level;
        }
        return 0.0f;
    }

    bool set_master_volume(float volume) override {
        auto vol = get_default_endpoint_volume(eRender);
        if (!vol) return false;
        return SUCCEEDED(vol->SetMasterVolumeLevelScalar(volume, nullptr));
    }

    bool is_master_muted() override {
        auto vol = get_default_endpoint_volume(eRender);
        if (!vol) return false;
        BOOL muted = FALSE;
        if (SUCCEEDED(vol->GetMute(&muted))) {
            return muted != FALSE;
        }
        return false;
    }

    bool set_master_mute(bool mute) override {
        auto vol = get_default_endpoint_volume(eRender);
        if (!vol) return false;
        return SUCCEEDED(vol->SetMute(mute ? TRUE : FALSE, nullptr));
    }

    std::vector<AudioEndpoint> get_endpoints(EndpointDirection direction) override {
        std::vector<AudioEndpoint> result;
        if (!enumerator_) return result;

        EDataFlow flow = (direction == EndpointDirection::Output) ? eRender : eCapture;
        ComPtr<IMMDeviceCollection> coll;
        HRESULT hr = enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll);
        if (FAILED(hr) || !coll) return result;

        UINT count = 0;
        coll->GetCount(&count);

        std::string def_id;
        ComPtr<IMMDevice> def_dev;
        if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, &def_dev))) {
            def_id = get_device_id(def_dev.Get());
        }

        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            if (SUCCEEDED(coll->Item(i, &dev)) && dev) {
                AudioEndpoint ep;
                ep.id = get_device_id(dev.Get());
                ep.direction = direction;
                ep.is_default = (!def_id.empty() && ep.id == def_id);

                ComPtr<IPropertyStore> props;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                    ep.name = get_device_property(props.Get(), PKEY_Device_FriendlyName);
                    ep.description = get_device_property(props.Get(), PKEY_DeviceInterface_FriendlyName);
                }

                ComPtr<IAudioEndpointVolume> ep_vol;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER,
                                            nullptr, reinterpret_cast<void**>(ep_vol.GetAddressOf()))) && ep_vol) {
                    float v = 0.0f;
                    if (SUCCEEDED(ep_vol->GetMasterVolumeLevelScalar(&v))) {
                        ep.volume = v;
                    }
                    BOOL m = FALSE;
                    if (SUCCEEDED(ep_vol->GetMute(&m))) {
                        ep.is_muted = (m != FALSE);
                    }
                }
                result.push_back(ep);
            }
        }
        return result;
    }

    std::optional<AudioEndpoint> get_default_endpoint(EndpointDirection direction) override {
        if (!enumerator_) return std::nullopt;
        EDataFlow flow = (direction == EndpointDirection::Output) ? eRender : eCapture;
        ComPtr<IMMDevice> dev;
        if (FAILED(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, &dev)) || !dev) {
            return std::nullopt;
        }

        AudioEndpoint ep;
        ep.id = get_device_id(dev.Get());
        ep.direction = direction;
        ep.is_default = true;

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
            ep.name = get_device_property(props.Get(), PKEY_Device_FriendlyName);
            ep.description = get_device_property(props.Get(), PKEY_DeviceInterface_FriendlyName);
        }

        ComPtr<IAudioEndpointVolume> ep_vol;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER,
                                    nullptr, reinterpret_cast<void**>(ep_vol.GetAddressOf()))) && ep_vol) {
            float v = 0.0f;
            if (SUCCEEDED(ep_vol->GetMasterVolumeLevelScalar(&v))) {
                ep.volume = v;
            }
            BOOL m = FALSE;
            if (SUCCEEDED(ep_vol->GetMute(&m))) {
                ep.is_muted = (m != FALSE);
            }
        }
        return ep;
    }

    bool set_endpoint_volume(const std::string& endpoint_id, float volume) override {
        auto vol = get_endpoint_volume_by_id(endpoint_id);
        if (!vol) return false;
        return SUCCEEDED(vol->SetMasterVolumeLevelScalar(volume, nullptr));
    }

    bool set_endpoint_mute(const std::string& endpoint_id, bool mute) override {
        auto vol = get_endpoint_volume_by_id(endpoint_id);
        if (!vol) return false;
        return SUCCEEDED(vol->SetMute(mute ? TRUE : FALSE, nullptr));
    }

    void set_volume_callback(AudioManager::VolumeCallback cb) override {
        unregister_volume_callback();
        volume_cb_ = std::move(cb);
        if (!volume_cb_) return;

        auto vol = get_default_endpoint_volume(eRender);
        if (!vol) return;

        auto handler = new EndpointCallbackHandler("", EndpointDirection::Output, volume_cb_);
        if (SUCCEEDED(vol->RegisterControlChangeNotify(handler))) {
            active_cb_handler_ = handler;
            active_vol_ = vol;
        } else {
            delete handler;
        }
    }

    void set_endpoint_callback(AudioManager::EndpointListCallback cb) override {
        endpoint_cb_ = std::move(cb);
    }

private:
    std::unique_ptr<ComScope> com_;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    AudioManager::VolumeCallback volume_cb_;
    AudioManager::EndpointListCallback endpoint_cb_;

    IAudioEndpointVolumeCallback* active_cb_handler_ = nullptr;
    ComPtr<IAudioEndpointVolume> active_vol_;

    void unregister_volume_callback() {
        if (active_vol_ && active_cb_handler_) {
            active_vol_->UnregisterControlChangeNotify(active_cb_handler_);
            active_cb_handler_->Release();
            active_cb_handler_ = nullptr;
            active_vol_.Reset();
        }
    }

    ComPtr<IAudioEndpointVolume> get_default_endpoint_volume(EDataFlow flow) {
        if (!enumerator_) return nullptr;
        ComPtr<IMMDevice> dev;
        if (FAILED(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, &dev)) || !dev) {
            return nullptr;
        }
        ComPtr<IAudioEndpointVolume> vol;
        if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER,
                                 nullptr, reinterpret_cast<void**>(vol.GetAddressOf())))) {
            return nullptr;
        }
        return vol;
    }

    ComPtr<IAudioEndpointVolume> get_endpoint_volume_by_id(const std::string& id) {
        if (!enumerator_ || id.empty()) return nullptr;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return nullptr;
        std::wstring wid(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, wid.data(), wlen);

        ComPtr<IMMDevice> dev;
        if (FAILED(enumerator_->GetDevice(wid.c_str(), &dev)) || !dev) {
            return nullptr;
        }
        ComPtr<IAudioEndpointVolume> vol;
        if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER,
                                 nullptr, reinterpret_cast<void**>(vol.GetAddressOf())))) {
            return nullptr;
        }
        return vol;
    }
};

std::unique_ptr<IAudioBackend> create_platform_audio_backend() {
    return std::make_unique<WindowsAudioBackend>();
}

} // namespace brosys

#endif // _WIN32
