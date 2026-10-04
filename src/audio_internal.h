#pragma once

#include "brosys/audio.h"
#include <memory>

namespace brosys {

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    virtual float get_master_volume() = 0;
    virtual bool set_master_volume(float volume) = 0;
    virtual bool is_master_muted() = 0;
    virtual bool set_master_mute(bool mute) = 0;

    virtual std::vector<AudioEndpoint> get_endpoints(EndpointDirection direction) = 0;
    virtual std::optional<AudioEndpoint> get_default_endpoint(EndpointDirection direction) = 0;
    virtual bool set_endpoint_volume(const std::string& endpoint_id, float volume) = 0;
    virtual bool set_endpoint_mute(const std::string& endpoint_id, bool mute) = 0;

    virtual void set_volume_callback(AudioManager::VolumeCallback cb) = 0;
    virtual void set_endpoint_callback(AudioManager::EndpointListCallback cb) = 0;
};

std::unique_ptr<IAudioBackend> create_platform_audio_backend();

} // namespace brosys
