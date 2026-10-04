#include "brosys/audio.h"
#include "audio_internal.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace brosys {

struct AudioManager::Impl {
    std::unique_ptr<IAudioBackend> backend;
    bool mocked = false;
    std::vector<AudioEndpoint> mock_outputs;
    std::vector<AudioEndpoint> mock_inputs;
    float mock_master_vol = 0.75f;
    bool mock_master_muted = false;

    VolumeCallback volume_cb;
    EndpointListCallback endpoint_cb;
    mutable std::mutex mutex;

    Impl() : backend(create_platform_audio_backend()) {}
};

AudioManager::AudioManager() : impl_(std::make_unique<Impl>()) {}
AudioManager::~AudioManager() = default;

AudioManager::AudioManager(AudioManager&&) noexcept = default;
AudioManager& AudioManager::operator=(AudioManager&&) noexcept = default;

float AudioManager::get_master_volume() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        return impl_->mock_master_vol;
    }
    return impl_->backend ? impl_->backend->get_master_volume() : 0.0f;
}

bool AudioManager::set_master_volume(float volume) {
    volume = std::clamp(volume, 0.0f, 1.0f);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        impl_->mock_master_vol = volume;
        if (impl_->volume_cb) {
            impl_->volume_cb(VolumeNotification{"", EndpointDirection::Output, volume, impl_->mock_master_muted});
        }
        return true;
    }
    return impl_->backend ? impl_->backend->set_master_volume(volume) : false;
}

bool AudioManager::is_master_muted() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        return impl_->mock_master_muted;
    }
    return impl_->backend ? impl_->backend->is_master_muted() : false;
}

bool AudioManager::set_master_mute(bool mute) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        impl_->mock_master_muted = mute;
        if (impl_->volume_cb) {
            impl_->volume_cb(VolumeNotification{"", EndpointDirection::Output, impl_->mock_master_vol, mute});
        }
        return true;
    }
    return impl_->backend ? impl_->backend->set_master_mute(mute) : false;
}

bool AudioManager::toggle_master_mute() {
    return set_master_mute(!is_master_muted());
}

std::vector<AudioEndpoint> AudioManager::get_output_endpoints() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        return impl_->mock_outputs;
    }
    return impl_->backend ? impl_->backend->get_endpoints(EndpointDirection::Output) : std::vector<AudioEndpoint>{};
}

std::vector<AudioEndpoint> AudioManager::get_input_endpoints() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        return impl_->mock_inputs;
    }
    return impl_->backend ? impl_->backend->get_endpoints(EndpointDirection::Input) : std::vector<AudioEndpoint>{};
}

std::optional<AudioEndpoint> AudioManager::get_default_output() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        for (const auto& ep : impl_->mock_outputs) {
            if (ep.is_default) return ep;
        }
        if (!impl_->mock_outputs.empty()) return impl_->mock_outputs.front();
        return std::nullopt;
    }
    return impl_->backend ? impl_->backend->get_default_endpoint(EndpointDirection::Output) : std::nullopt;
}

std::optional<AudioEndpoint> AudioManager::get_default_input() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        for (const auto& ep : impl_->mock_inputs) {
            if (ep.is_default) return ep;
        }
        if (!impl_->mock_inputs.empty()) return impl_->mock_inputs.front();
        return std::nullopt;
    }
    return impl_->backend ? impl_->backend->get_default_endpoint(EndpointDirection::Input) : std::nullopt;
}

bool AudioManager::set_endpoint_volume(const std::string& endpoint_id, float volume) {
    volume = std::clamp(volume, 0.0f, 1.0f);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        for (auto& ep : impl_->mock_outputs) {
            if (ep.id == endpoint_id) {
                ep.volume = volume;
                return true;
            }
        }
        for (auto& ep : impl_->mock_inputs) {
            if (ep.id == endpoint_id) {
                ep.volume = volume;
                return true;
            }
        }
        return false;
    }
    return impl_->backend ? impl_->backend->set_endpoint_volume(endpoint_id, volume) : false;
}

bool AudioManager::set_endpoint_mute(const std::string& endpoint_id, bool mute) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->mocked) {
        for (auto& ep : impl_->mock_outputs) {
            if (ep.id == endpoint_id) {
                ep.is_muted = mute;
                return true;
            }
        }
        for (auto& ep : impl_->mock_inputs) {
            if (ep.id == endpoint_id) {
                ep.is_muted = mute;
                return true;
            }
        }
        return false;
    }
    return impl_->backend ? impl_->backend->set_endpoint_mute(endpoint_id, mute) : false;
}

void AudioManager::register_volume_callback(VolumeCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->volume_cb = cb;
    if (impl_->backend) {
        impl_->backend->set_volume_callback(std::move(cb));
    }
}

void AudioManager::register_endpoint_callback(EndpointListCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->endpoint_cb = cb;
    if (impl_->backend) {
        impl_->backend->set_endpoint_callback(std::move(cb));
    }
}

void AudioManager::set_mock_endpoints(const std::vector<AudioEndpoint>& outputs,
                                      const std::vector<AudioEndpoint>& inputs) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->mocked = true;
    impl_->mock_outputs = outputs;
    impl_->mock_inputs = inputs;
}

void AudioManager::clear_mock_endpoints() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->mocked = false;
    impl_->mock_outputs.clear();
    impl_->mock_inputs.clear();
}

bool AudioManager::is_mocked() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->mocked;
}

namespace audio {

static AudioManager& get_instance() {
    static AudioManager inst;
    return inst;
}

float get_master_volume() {
    return get_instance().get_master_volume();
}

bool set_master_volume(float volume) {
    return get_instance().set_master_volume(volume);
}

bool is_master_muted() {
    return get_instance().is_master_muted();
}

bool set_master_mute(bool mute) {
    return get_instance().set_master_mute(mute);
}

bool toggle_master_mute() {
    return get_instance().toggle_master_mute();
}

std::vector<AudioEndpoint> get_output_endpoints() {
    return get_instance().get_output_endpoints();
}

std::vector<AudioEndpoint> get_input_endpoints() {
    return get_instance().get_input_endpoints();
}

} // namespace audio

} // namespace brosys
