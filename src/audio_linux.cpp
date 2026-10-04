#if !defined(_WIN32)

#include "audio_internal.h"
#include "dbus_helper.h"
#include <algorithm>
#include <iostream>

namespace brosys {

class LinuxAudioBackend : public IAudioBackend {
public:
    LinuxAudioBackend() {
        master_volume_ = 0.8f;
        master_muted_ = false;

        AudioEndpoint out_ep;
        out_ep.id = "default_output";
        out_ep.name = "Default Output Sink";
        out_ep.description = "PipeWire / PulseAudio System Output";
        out_ep.direction = EndpointDirection::Output;
        out_ep.is_default = true;
        out_ep.volume = master_volume_;
        out_ep.is_muted = master_muted_;
        endpoints_.push_back(out_ep);

        AudioEndpoint in_ep;
        in_ep.id = "default_input";
        in_ep.name = "Default Input Source";
        in_ep.description = "PipeWire / PulseAudio System Input";
        in_ep.direction = EndpointDirection::Input;
        in_ep.is_default = true;
        in_ep.volume = 0.7f;
        in_ep.is_muted = false;
        endpoints_.push_back(in_ep);
    }

    float get_master_volume() override {
        // Check MPRIS first if active player exists
        auto bus = dbus::DBusConnection::open(dbus::BusType::Session);
        if (bus) {
            dbus::DBusVariant val;
            if (bus->get_property("org.mpris.MediaPlayer2.player",
                                  "/org/mpris/MediaPlayer2",
                                  "org.mpris.MediaPlayer2.Player",
                                  "Volume", val)) {
                if (std::holds_alternative<double>(val)) {
                    return static_cast<float>(std::get<double>(val));
                }
            }
        }
        return master_volume_;
    }

    bool set_master_volume(float volume) override {
        master_volume_ = std::clamp(volume, 0.0f, 1.0f);
        // Also update MPRIS if available
        auto bus = dbus::DBusConnection::open(dbus::BusType::Session);
        if (bus) {
            bus->set_property("org.mpris.MediaPlayer2.player",
                              "/org/mpris/MediaPlayer2",
                              "org.mpris.MediaPlayer2.Player",
                              "Volume", static_cast<double>(master_volume_));
        }

        for (auto& ep : endpoints_) {
            if (ep.direction == EndpointDirection::Output && ep.is_default) {
                ep.volume = master_volume_;
            }
        }

        if (vol_cb_) {
            VolumeNotification n;
            n.endpoint_id = "default_output";
            n.direction = EndpointDirection::Output;
            n.volume = master_volume_;
            n.is_muted = master_muted_;
            vol_cb_(n);
        }
        return true;
    }

    bool is_master_muted() override {
        return master_muted_;
    }

    bool set_master_mute(bool mute) override {
        master_muted_ = mute;
        for (auto& ep : endpoints_) {
            if (ep.direction == EndpointDirection::Output && ep.is_default) {
                ep.is_muted = master_muted_;
            }
        }
        if (vol_cb_) {
            VolumeNotification n;
            n.endpoint_id = "default_output";
            n.direction = EndpointDirection::Output;
            n.volume = master_volume_;
            n.is_muted = master_muted_;
            vol_cb_(n);
        }
        return true;
    }

    std::vector<AudioEndpoint> get_endpoints(EndpointDirection direction) override {
        std::vector<AudioEndpoint> result;
        for (const auto& ep : endpoints_) {
            if (ep.direction == direction) {
                result.push_back(ep);
            }
        }
        return result;
    }

    std::optional<AudioEndpoint> get_default_endpoint(EndpointDirection direction) override {
        for (const auto& ep : endpoints_) {
            if (ep.direction == direction && ep.is_default) {
                return ep;
            }
        }
        return std::nullopt;
    }

    bool set_endpoint_volume(const std::string& endpoint_id, float volume) override {
        for (auto& ep : endpoints_) {
            if (ep.id == endpoint_id) {
                ep.volume = std::clamp(volume, 0.0f, 1.0f);
                if (ep.is_default && ep.direction == EndpointDirection::Output) {
                    master_volume_ = ep.volume;
                }
                return true;
            }
        }
        return false;
    }

    bool set_endpoint_mute(const std::string& endpoint_id, bool mute) override {
        for (auto& ep : endpoints_) {
            if (ep.id == endpoint_id) {
                ep.is_muted = mute;
                if (ep.is_default && ep.direction == EndpointDirection::Output) {
                    master_muted_ = mute;
                }
                return true;
            }
        }
        return false;
    }

    void set_volume_callback(AudioManager::VolumeCallback cb) override {
        vol_cb_ = std::move(cb);
    }

    void set_endpoint_callback(AudioManager::EndpointListCallback cb) override {
        endpoint_cb_ = std::move(cb);
    }

private:
    float master_volume_ = 0.8f;
    bool master_muted_ = false;
    std::vector<AudioEndpoint> endpoints_;
    AudioManager::VolumeCallback vol_cb_;
    AudioManager::EndpointListCallback endpoint_cb_;
};

std::unique_ptr<IAudioBackend> create_platform_audio_backend() {
    return std::make_unique<LinuxAudioBackend>();
}

} // namespace brosys

#endif // !_WIN32
