// Windows audio: one worker thread (COM MTA) owns the device enumerator,
// the endpoint-volume objects and the model. Core Audio notification
// callbacks arrive on system threads and only enqueue work; the worker
// re-reads the OS state and pushes events for what actually changed.
// Host commands are executed on the worker too.
#include "brosys/audio.h"

#include "win/audio_endpoints.h"
#include "win/util.h"

#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace brosys {

namespace {

using win::ComPtr;

struct Work {
    enum Kind { Rescan, Default, Props, Volume, Command, Stop } kind = Rescan;
    std::string id;
    win::VolumeSample volume;
    std::function<void()> command;
};

class WinAudioService final : public AudioService {
public:
    explicit WinAudioService(const AudioConfig& cfg) : config_(cfg) {}

    ~WinAudioService() override {
        enqueue(Work{Work::Stop});
        if (thread_.joinable()) thread_.join();
    }

    bool start(std::string* error) {
        std::promise<std::string> ready;
        auto fut = ready.get_future();
        thread_ = std::thread([this, &ready] { thread_main(ready); });
        std::string err = fut.get();
        if (!err.empty()) {
            if (thread_.joinable()) thread_.join();
            if (error) *error = err;
            return false;
        }
        return true;
    }

    AudioEventQueue& events() override { return queue_; }

    AudioState state() const override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return published_;
    }

    Result set_volume(const std::string& id, float volume) override {
        float v = volume < 0 ? 0.0f : volume > 1.0f ? 1.0f : volume;
        return command([this, id, v]() -> Result {
            auto it = entries_.find(id);
            if (it == entries_.end() || !it->second.vol) return Result::failure("no such audio device: " + id);
            HRESULT hr = it->second.vol->SetMasterVolumeLevelScalar(v, &kContext);
            if (FAILED(hr)) return Result::failure(win::hresult_error("SetMasterVolumeLevelScalar", hr));
            return Result::success();
        });
    }

    Result set_mute(const std::string& id, bool muted) override {
        return command([this, id, muted]() -> Result {
            auto it = entries_.find(id);
            if (it == entries_.end() || !it->second.vol) return Result::failure("no such audio device: " + id);
            HRESULT hr = it->second.vol->SetMute(muted ? TRUE : FALSE, &kContext);
            if (FAILED(hr)) return Result::failure(win::hresult_error("SetMute", hr));
            return Result::success();
        });
    }

    Result set_default(const std::string& id) override {
        return command([this, id]() -> Result {
            if (!entries_.count(id)) return Result::failure("no such audio device: " + id);
            ComPtr<win::IPolicyConfig> policy;
            HRESULT hr = CoCreateInstance(win::kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(win::IPolicyConfig),
                                          reinterpret_cast<void**>(policy.GetAddressOf()));
            if (FAILED(hr)) return Result::failure(win::hresult_error("CoCreateInstance(PolicyConfig)", hr));
            std::wstring wid = win::to_wide(id);
            // What "Set as Default Device" in the Sound panel does: console +
            // multimedia (communications is a separate user choice).
            for (ERole role : {eConsole, eMultimedia}) {
                hr = policy->SetDefaultEndpoint(wid.c_str(), role);
                if (FAILED(hr)) return Result::failure(win::hresult_error("SetDefaultEndpoint", hr));
            }
            return Result::success();
        });
    }

private:
    struct Entry {
        AudioDevice device;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioEndpointVolume> vol;
        win::VolumeSink* sink = nullptr;
    };

    // Our own event context for volume changes we make (informational).
    static constexpr GUID kContext = {0x5a1b6f2e, 0x2c4d, 0x4b0e, {0x9c, 0x31, 0x62, 0x7e, 0x0a, 0x41, 0x8b, 0x77}};

    Result command(std::function<Result()> fn) {
        auto task = std::make_shared<std::packaged_task<Result()>>(std::move(fn));
        auto fut = task->get_future();
        Work w{Work::Command};
        w.command = [task] { (*task)(); };
        if (!enqueue(std::move(w))) return Result::failure("the audio service is shutting down");
        return fut.get();
    }

    // The work inbox is shared with the COM sinks, which may still be
    // mid-callback on a system thread after they are unregistered.
    struct Inbox {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Work> work;
        bool stopped = false;

        bool push(Work w) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopped) return false;
                work.push_back(std::move(w));
            }
            cv.notify_one();
            return true;
        }
    };

    bool enqueue(Work w) { return inbox_->push(std::move(w)); }

    void publish() {
        AudioState s;
        for (auto& [id, e] : entries_) s.devices.push_back(e.device);
        s.default_output = default_[0];
        s.default_input = default_[1];
        std::lock_guard<std::mutex> lock(state_mutex_);
        published_ = std::move(s);
    }

    static int slot(AudioDirection d) { return d == AudioDirection::Output ? 0 : 1; }

    std::string read_default(EDataFlow flow) {
        ComPtr<IMMDevice> dev;
        if (FAILED(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, &dev)) || !dev) return {};
        return win::endpoint_id(dev.Get());
    }

    void watch(Entry& e) {
        if (!e.vol) return;
        std::string id = e.device.id;
        auto* sink = new win::VolumeSink([inbox = inbox_, id](const win::VolumeSample& s) {
            Work w{Work::Volume};
            w.id = id;
            w.volume = s;
            inbox->push(std::move(w));
        });
        if (SUCCEEDED(e.vol->RegisterControlChangeNotify(sink))) e.sink = sink;
        else sink->Release();
    }

    void unwatch(Entry& e) {
        if (e.vol && e.sink) e.vol->UnregisterControlChangeNotify(e.sink);
        if (e.sink) e.sink->Release();
        e.sink = nullptr;
    }

    // Re-reads the active endpoint set; pushes Added / Removed / Changed.
    void rescan(bool initial) {
        std::map<std::string, Entry> seen;
        for (EDataFlow flow : {eRender, eCapture}) {
            AudioDirection dir = flow == eRender ? AudioDirection::Output : AudioDirection::Input;
            ComPtr<IMMDeviceCollection> coll;
            if (FAILED(enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll)) || !coll) continue;
            UINT n = 0;
            coll->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                Entry e;
                if (FAILED(coll->Item(i, &e.dev)) || !e.dev) continue;
                e.dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(e.vol.GetAddressOf()));
                if (!win::read_endpoint(e.dev.Get(), dir, e.vol.Get(), e.device)) continue;
                std::string id = e.device.id;
                seen.emplace(id, std::move(e));
            }
        }
        std::string defaults[2] = {read_default(eRender), read_default(eCapture)};
        for (auto& [id, e] : seen) e.device.is_default = defaults[slot(e.device.direction)] == id;

        // Removed.
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (!seen.count(it->first)) {
                unwatch(it->second);
                queue_.push(AudioDeviceRemoved{it->first, it->second.device.direction});
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        // Added / changed.
        for (auto& [id, e] : seen) {
            auto it = entries_.find(id);
            if (it == entries_.end()) {
                Entry& ne = entries_[id];
                ne = std::move(e);
                watch(ne);
                queue_.push(AudioDeviceAdded{ne.device});
                continue;
            }
            uint32_t changes = diff(it->second.device, e.device);
            it->second.device = e.device;
            if (changes) queue_.push(AudioDeviceChanged{e.device, changes});
        }
        for (int i = 0; i < 2; ++i) {
            if (initial || defaults[i] != default_[i]) {
                default_[i] = defaults[i];
                queue_.push(AudioDefaultChanged{i == 0 ? AudioDirection::Output : AudioDirection::Input, defaults[i]});
            }
        }
        publish();
    }

    static uint32_t diff(const AudioDevice& a, const AudioDevice& b) {
        uint32_t c = 0;
        if (a.volume != b.volume || a.channel_volumes != b.channel_volumes) c |= audio_change::Volume;
        if (a.muted != b.muted) c |= audio_change::Mute;
        if (a.description != b.description || a.device_name != b.device_name || a.form_factor != b.form_factor)
            c |= audio_change::Description;
        if (a.state != b.state) c |= audio_change::State;
        if (a.is_default != b.is_default) c |= audio_change::Default;
        return c;
    }

    void on_volume(const std::string& id, const win::VolumeSample& s) {
        auto it = entries_.find(id);
        if (it == entries_.end()) return;
        AudioDevice d = it->second.device;
        d.volume = s.master;
        d.muted = s.muted;
        if (!s.channels.empty()) d.channel_volumes = s.channels;
        uint32_t changes = diff(it->second.device, d);
        if (!changes) return;
        it->second.device = d;
        publish();
        queue_.push(AudioDeviceChanged{d, changes});
    }

    void on_props(const std::string& id) {
        auto it = entries_.find(id);
        if (it == entries_.end()) return;
        AudioDevice d;
        if (!win::read_endpoint(it->second.dev.Get(), it->second.device.direction, it->second.vol.Get(), d)) return;
        d.is_default = it->second.device.is_default;
        uint32_t changes = diff(it->second.device, d);
        if (!changes) return;
        it->second.device = d;
        publish();
        queue_.push(AudioDeviceChanged{d, changes});
    }

    void thread_main(std::promise<std::string>& ready) {
        win::ComInit com;
        if (!com.ok()) {
            ready.set_value("COM initialization failed");
            return;
        }
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(enumerator_.GetAddressOf()));
        if (FAILED(hr)) {
            ready.set_value(win::hresult_error("CoCreateInstance(MMDeviceEnumerator)", hr));
            return;
        }
        auto* sink = new win::DeviceSink([inbox = inbox_](int kind, EDataFlow, const std::wstring& id) {
            Work w{kind == 0 ? Work::Rescan : kind == 1 ? Work::Default : Work::Props};
            w.id = win::to_utf8(id);
            inbox->push(std::move(w));
        });
        if (FAILED(enumerator_->RegisterEndpointNotificationCallback(sink))) {
            sink->Release();
            sink = nullptr;
        }
        rescan(true);
        ready.set_value(std::string());

        while (true) {
            std::deque<Work> batch;
            {
                std::unique_lock<std::mutex> lock(inbox_->mutex);
                inbox_->cv.wait(lock, [&] { return !inbox_->work.empty(); });
                batch.swap(inbox_->work);
            }
            bool stop = false, need_rescan = false;
            for (auto& w : batch) {
                switch (w.kind) {
                    case Work::Stop: stop = true; break;
                    case Work::Rescan:
                    case Work::Default: need_rescan = true; break;
                    case Work::Props: on_props(w.id); break;
                    case Work::Volume: on_volume(w.id, w.volume); break;
                    case Work::Command: w.command(); break;
                }
            }
            if (need_rescan && !stop) rescan(false);
            if (stop) break;
        }
        std::deque<Work> rest;
        {
            std::lock_guard<std::mutex> lock(inbox_->mutex);
            inbox_->stopped = true;
            rest.swap(inbox_->work);
        }
        for (auto& w : rest)
            if (w.kind == Work::Command) w.command();  // complete waiting callers
        if (sink) {
            enumerator_->UnregisterEndpointNotificationCallback(sink);
            sink->Release();
        }
        for (auto& [id, e] : entries_) unwatch(e);
        entries_.clear();
        enumerator_.Reset();
    }

    AudioConfig config_;
    AudioEventQueue queue_;
    mutable std::mutex state_mutex_;
    AudioState published_;

    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();

    // Worker-thread state.
    ComPtr<IMMDeviceEnumerator> enumerator_;
    std::map<std::string, Entry> entries_;
    std::string default_[2];
    std::thread thread_;
};

}  // namespace

std::unique_ptr<AudioService> AudioService::create(const AudioConfig& config, std::string* error) {
    auto s = std::make_unique<WinAudioService>(config);
    if (!s->start(error)) return nullptr;
    return s;
}

}  // namespace brosys
