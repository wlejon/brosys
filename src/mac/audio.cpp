// macOS audio service. CoreAudio property listeners (the device list, the
// two default devices, and per device: name, streams, liveness, virtual main
// volume, channel volumes, mute, data source) run on one serial dispatch
// queue; any of them schedules a single coalesced rescan there, which
// re-reads the HAL and pushes events for what actually changed. Commands
// resolve their id on the queue and call the HAL from the caller's thread.
#include "brosys/audio.h"

#include "mac/audio_hal.h"

#include <Block.h>
#include <dispatch/dispatch.h>

#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace brosys {

namespace {

using mac::HalDevice;

class MacAudioService final : public AudioService {
public:
    explicit MacAudioService(const AudioConfig& cfg)
        : config_(cfg), queue_(dispatch_queue_create("brosys.audio", DISPATCH_QUEUE_SERIAL)) {
        listener_ = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) { schedule_rescan(); });
    }

    ~MacAudioService() override {
        for (const auto& a : system_props())
            AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &a, queue_, listener_);
        dispatch_sync(queue_, ^{
          for (AudioObjectID o : watched_) unwatch(o);
          watched_.clear();
          stopped_ = true;
        });
        dispatch_sync(queue_, ^{});  // a rescan scheduled before `stopped_` runs (and does nothing) first
        Block_release(listener_);
        dispatch_release(queue_);
    }

    bool start(std::string* error) {
        for (const auto& a : system_props()) {
            OSStatus st = AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &a, queue_, listener_);
            if (st != noErr) {
                if (error) *error = "CoreAudio: cannot listen to the system object (OSStatus " + std::to_string(st) + ")";
                return false;
            }
        }
        dispatch_sync(queue_, ^{ rescan(true); });  // Added / DefaultChanged queued before create() returns
        return true;
    }

    AudioEventQueue& events() override { return events_; }

    AudioState state() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return published_;
    }

    Result set_volume(const std::string& id, float volume) override {
        float v = volume < 0 ? 0.0f : volume > 1.0f ? 1.0f : volume;
        auto target = resolve(id);
        if (!target) return Result::failure("no such audio device: " + id);
        return mac::hal_set_volume(target->object, target->device.direction, v);
    }

    Result set_mute(const std::string& id, bool muted) override {
        auto target = resolve(id);
        if (!target) return Result::failure("no such audio device: " + id);
        return mac::hal_set_mute(target->object, target->device.direction, muted);
    }

    Result set_default(const std::string& id) override {
        auto target = resolve(id);
        if (!target) return Result::failure("no such audio device: " + id);
        return mac::hal_set_default(target->object, target->device.direction);
    }

private:
    static std::vector<AudioObjectPropertyAddress> system_props() {
        return {
            {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
            {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
            {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        };
    }

    std::optional<HalDevice> resolve(const std::string& id) {
        __block std::optional<HalDevice> out;
        dispatch_sync(queue_, ^{
          auto it = entries_.find(id);
          if (it != entries_.end()) out = it->second;
        });
        return out;
    }

    // Queue only from here on.
    void schedule_rescan() {
        if (rescan_pending_ || stopped_) return;
        rescan_pending_ = true;
        dispatch_async(queue_, ^{
          rescan_pending_ = false;
          if (!stopped_) rescan(false);
        });
    }

    void watch(AudioObjectID o) {
        for (const auto& a : mac::watched_device_properties())
            AudioObjectAddPropertyListenerBlock(o, &a, queue_, listener_);  // absent properties fail harmlessly
    }

    void unwatch(AudioObjectID o) {
        for (const auto& a : mac::watched_device_properties())
            AudioObjectRemovePropertyListenerBlock(o, &a, queue_, listener_);
    }

    static uint32_t diff(const AudioDevice& a, const AudioDevice& b) {
        uint32_t c = 0;
        if (a.volume != b.volume || a.channel_volumes != b.channel_volumes || a.has_volume != b.has_volume)
            c |= audio_change::Volume;
        if (a.muted != b.muted) c |= audio_change::Mute;
        if (a.description != b.description || a.device_name != b.device_name || a.form_factor != b.form_factor)
            c |= audio_change::Description;
        if (a.state != b.state) c |= audio_change::State;
        if (a.is_default != b.is_default) c |= audio_change::Default;
        return c;
    }

    void rescan(bool initial) {
        std::string defaults[2];
        for (int i = 0; i < 2; ++i) {
            AudioObjectID o = mac::default_device(i == 0 ? AudioDirection::Output : AudioDirection::Input);
            std::string uid = o == kAudioObjectUnknown ? std::string() : mac::device_uid(o);
            if (!uid.empty()) defaults[i] = mac::audio_id(uid, i == 0 ? AudioDirection::Output : AudioDirection::Input);
        }
        std::map<std::string, HalDevice> seen;
        std::set<AudioObjectID> objects;
        for (auto& h : mac::read_hal_devices()) {
            h.device.is_default = defaults[h.device.direction == AudioDirection::Output ? 0 : 1] == h.device.id;
            objects.insert(h.object);
            seen.emplace(h.device.id, std::move(h));
        }
        // A default that is not a visible device of that direction is reported as none.
        for (auto& d : defaults)
            if (!d.empty() && !seen.count(d)) d.clear();

        for (AudioObjectID o : objects)
            if (!watched_.count(o)) watch(o);
        for (auto it = watched_.begin(); it != watched_.end();) {
            if (!objects.count(*it)) {
                unwatch(*it);
                it = watched_.erase(it);
            } else {
                ++it;
            }
        }
        watched_.insert(objects.begin(), objects.end());

        for (auto it = entries_.begin(); it != entries_.end();) {
            if (!seen.count(it->first)) {
                events_.push(AudioDeviceRemoved{it->first, it->second.device.direction});
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& [id, h] : seen) {
            auto it = entries_.find(id);
            if (it == entries_.end()) {
                entries_.emplace(id, h);
                events_.push(AudioDeviceAdded{h.device});
                continue;
            }
            uint32_t changes = diff(it->second.device, h.device);
            it->second = h;
            if (changes) events_.push(AudioDeviceChanged{h.device, changes});
        }
        for (int i = 0; i < 2; ++i) {
            if (initial || defaults[i] != default_[i]) {
                default_[i] = defaults[i];
                events_.push(AudioDefaultChanged{i == 0 ? AudioDirection::Output : AudioDirection::Input, defaults[i]});
            }
        }
        AudioState s;
        for (auto& [id, h] : entries_) s.devices.push_back(h.device);
        s.default_output = default_[0];
        s.default_input = default_[1];
        std::lock_guard<std::mutex> lock(mutex_);
        published_ = std::move(s);
    }

    AudioConfig config_;
    AudioEventQueue events_;
    mutable std::mutex mutex_;
    AudioState published_;

    dispatch_queue_t queue_;
    AudioObjectPropertyListenerBlock listener_ = nullptr;
    // Queue-confined.
    std::map<std::string, HalDevice> entries_;
    std::set<AudioObjectID> watched_;
    std::string default_[2];
    bool rescan_pending_ = false;
    bool stopped_ = false;
};

}  // namespace

std::unique_ptr<AudioService> AudioService::create(const AudioConfig& config, std::string* error) {
    auto s = std::make_unique<MacAudioService>(config);
    if (!s->start(error)) return nullptr;
    return s;
}

}  // namespace brosys
