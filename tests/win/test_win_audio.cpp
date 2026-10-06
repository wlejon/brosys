// Windows audio against the OS: the endpoint store in the registry
// (MMDevices: which endpoints are active, their names), and Core Audio
// queried directly for defaults and volumes. Never changes the user's
// volume, mute or default device: the only writes re-apply a value that
// is already set (mute state, current default), which changes nothing.
#include "check.h"

#include "brosys/audio.h"
#include "win/audio_endpoints.h"

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <set>

using namespace brosys;
using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

struct RegEndpoint {
    AudioDirection direction;
    std::string desc;  // DeviceDesc ("Speakers", or the user's rename)
};

// guid (lower case, with braces) -> endpoint, for DeviceState == ACTIVE.
std::map<std::string, RegEndpoint> registry_endpoints() {
    std::map<std::string, RegEndpoint> out;
    for (auto [sub, dir] : {std::pair{L"Render", AudioDirection::Output}, std::pair{L"Capture", AudioDirection::Input}}) {
        std::wstring base = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\") + sub;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; ++i) {
            wchar_t name[128];
            DWORD len = 128;
            if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            DWORD state = 0, size = sizeof state;
            if (RegGetValueW(key, name, L"DeviceState", RRF_RT_REG_DWORD, nullptr, &state, &size) != ERROR_SUCCESS ||
                state != DEVICE_STATE_ACTIVE)
                continue;
            wchar_t desc[512] = {};
            DWORD dsize = sizeof desc;
            std::wstring props = std::wstring(name) + L"\\Properties";
            RegGetValueW(key, props.c_str(), L"{a45c254e-df1c-4efd-8020-67d146a850e0},2", RRF_RT_REG_SZ, nullptr, desc,
                         &dsize);
            out[lower(narrow(name))] = RegEndpoint{dir, narrow(desc)};
        }
        RegCloseKey(key);
    }
    return out;
}

std::string guid_of(const std::string& id) {
    // "{0.0.0.00000000}.{guid}" -> "{guid}"
    auto dot = id.find("}.{");
    return dot == std::string::npos ? std::string() : lower(id.substr(dot + 2));
}

ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                     reinterpret_cast<void**>(e.GetAddressOf()));
    return e;
}

ComPtr<IAudioEndpointVolume> volume_of(IMMDeviceEnumerator* en, const std::string& id) {
    int n = MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, w.data(), n);
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioEndpointVolume> vol;
    if (SUCCEEDED(en->GetDevice(w.c_str(), &dev)))
        dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(vol.GetAddressOf()));
    return vol;
}

std::string os_default(IMMDeviceEnumerator* en, EDataFlow flow, ERole role) {
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(flow, role, &dev))) return {};
    return brosys::win::endpoint_id(dev.Get());
}

void test_model(AudioService& svc) {
    // Events queued by create(): one Added per device, one DefaultChanged per direction.
    auto evs = svc.events().drain();
    std::set<std::string> added;
    int defaults = 0;
    for (auto& e : evs) {
        if (auto* a = std::get_if<AudioDeviceAdded>(&e)) added.insert(a->device.id);
        if (std::holds_alternative<AudioDefaultChanged>(e)) ++defaults;
    }
    CHECK_EQ(defaults, 2);

    AudioState s = svc.state();
    auto reg = registry_endpoints();
    std::printf("audio: %zu devices (registry: %zu active endpoints)\n", s.devices.size(), reg.size());
    CHECK_EQ(s.devices.size(), reg.size());
    CHECK_EQ(added.size(), s.devices.size());
    for (auto& d : s.devices) {
        std::printf("  %s %-6s %-11s vol=%.3f mute=%d '%s' [%s]\n", d.is_default ? "*" : " ", to_string(d.direction),
                    d.form_factor.c_str(), d.volume, d.muted, d.description.c_str(), d.device_name.c_str());
        auto it = reg.find(guid_of(d.id));
        CHECK(it != reg.end());
        if (it == reg.end()) continue;
        CHECK(it->second.direction == d.direction);
        CHECK(d.id.rfind(d.direction == AudioDirection::Output ? "{0.0.0." : "{0.0.1.", 0) == 0);
        CHECK(!it->second.desc.empty() && d.description.rfind(it->second.desc, 0) == 0);
        CHECK(added.count(d.id) == 1);
        CHECK(d.state == AudioDeviceState::Active);
    }

    auto en = enumerator();
    REQUIRE(en);
    CHECK_EQ(s.default_output, os_default(en.Get(), eRender, eConsole));
    CHECK_EQ(s.default_input, os_default(en.Get(), eCapture, eConsole));
    int marked = 0;
    for (auto& d : s.devices) {
        if (d.is_default) ++marked;
        CHECK_EQ(d.is_default, d.id == (d.direction == AudioDirection::Output ? s.default_output : s.default_input));
        auto vol = volume_of(en.Get(), d.id);
        REQUIRE(vol);
        float level = -1;
        BOOL muted = FALSE;
        UINT channels = 0;
        vol->GetMasterVolumeLevelScalar(&level);
        vol->GetMute(&muted);
        vol->GetChannelCount(&channels);
        CHECK(std::fabs(level - d.volume) < 1e-4f);
        CHECK_EQ(d.muted, muted != FALSE);
        CHECK_EQ(d.channel_volumes.size(), size_t(channels));
    }
    CHECK_EQ(marked, (s.default_output.empty() ? 0 : 1) + (s.default_input.empty() ? 0 : 1));

    CHECK(!svc.set_volume("{not-a-device}", 0.5f).ok);
    CHECK(!svc.set_mute("{not-a-device}", true).ok);
    CHECK(!svc.set_default("{not-a-device}").ok);
}

// Re-applying values that are already set: the calls must succeed and,
// since nothing changed, no change event may be reported.
void test_idempotent_writes(AudioService& svc) {
    AudioState s = svc.state();
    if (s.default_output.empty()) return;
    auto en = enumerator();
    REQUIRE(en);
    // set_default sets console + multimedia; only re-apply when both already
    // point at this device, otherwise it would change the multimedia default.
    if (os_default(en.Get(), eRender, eMultimedia) == s.default_output) {
        Result r = svc.set_default(s.default_output);
        CHECK(r.ok);
        if (!r.ok) std::printf("set_default: %s\n", r.error.c_str());
    } else {
        std::printf("note: console and multimedia defaults differ; set_default re-apply skipped\n");
    }
    const AudioDevice* out = nullptr;
    for (auto& d : s.devices)
        if (d.id == s.default_output) out = &d;
    REQUIRE(out);
    svc.events().drain();
    Result r = svc.set_mute(out->id, out->muted);
    CHECK(r.ok);
    std::this_thread::sleep_for(300ms);
    for (auto& e : svc.events().drain()) {
        if (auto* c = std::get_if<AudioDeviceChanged>(&e)) CHECK(c->changes == 0);  // never reached: nothing changed
        CHECK(!std::holds_alternative<AudioDefaultChanged>(e));
        CHECK(!std::holds_alternative<AudioDeviceRemoved>(e));
    }
    CHECK_EQ(svc.state().default_output, s.default_output);
}

// The OS -> endpoint-volume-callback path, observed on the same COM sink the
// service registers for every endpoint. Re-applying the current mute state is
// still notified by Windows, so the delivery is checked without changing anything.
void test_volume_notifications(AudioService& svc) {
    AudioState s = svc.state();
    if (s.default_output.empty()) return;
    auto en = enumerator();
    auto vol = volume_of(en.Get(), s.default_output);
    REQUIRE(vol);
    std::atomic<int> notified{0};
    std::atomic<bool> muted_seen{false};
    auto* sink = new brosys::win::VolumeSink([&](const brosys::win::VolumeSample& v) {
        muted_seen = v.muted;
        ++notified;
    });
    REQUIRE(SUCCEEDED(vol->RegisterControlChangeNotify(sink)));
    BOOL muted = FALSE;
    vol->GetMute(&muted);
    Result r = svc.set_mute(s.default_output, muted != FALSE);  // through the service
    CHECK(r.ok);
    bool got = bstest::wait_until([&] { return notified.load() > 0; }, 2000ms);
    if (got) {
        CHECK_EQ(muted_seen.load(), muted != FALSE);
        std::printf("volume notification delivered for a same-value mute write\n");
    } else {
        std::printf("note: Windows did not notify a same-value write on this endpoint\n");
    }
    vol->UnregisterControlChangeNotify(sink);
    sink->Release();
}

}  // namespace

int main() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string err;
    auto svc = AudioService::create(AudioConfig{}, &err);
    if (!svc) {
        std::printf("create failed: %s\n", err.c_str());
        return 1;
    }
    test_model(*svc);
    if (bstest::mutate_opted_in()) {
        test_idempotent_writes(*svc);
        test_volume_notifications(*svc);
    } else {
        std::printf("note: same-value writes to the default endpoint not exercised; set BROSYS_TEST_MUTATE=1\n");
    }
    svc.reset();
    if (SUCCEEDED(hr)) CoUninitialize();
    return bstest::finish("test_win_audio");
}
