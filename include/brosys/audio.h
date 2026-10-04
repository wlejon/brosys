#pragma once

#include "brosys/export.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brosys {

enum class EndpointDirection {
    Output = 0, // Speakers, Headphones, HDMI
    Input = 1   // Microphones, Line-in
};

struct AudioEndpoint {
    std::string id;
    std::string name;
    std::string description;
    EndpointDirection direction = EndpointDirection::Output;
    bool is_default = false;
    float volume = 0.0f; // 0.0f - 1.0f
    bool is_muted = false;
    std::vector<std::string> supported_formats;

    bool operator==(const AudioEndpoint& other) const = default;
};

struct VolumeNotification {
    std::string endpoint_id;
    EndpointDirection direction = EndpointDirection::Output;
    float volume = 0.0f;
    bool is_muted = false;

    bool operator==(const VolumeNotification& other) const = default;
};

class BROSYS_API AudioManager {
public:
    using VolumeCallback = std::function<void(const VolumeNotification&)>;
    using EndpointListCallback = std::function<void(EndpointDirection direction)>;

    AudioManager();
    ~AudioManager();

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;
    AudioManager(AudioManager&&) noexcept;
    AudioManager& operator=(AudioManager&&) noexcept;

    // Master volume & mute
    [[nodiscard]] float get_master_volume() const;
    bool set_master_volume(float volume);
    [[nodiscard]] bool is_master_muted() const;
    bool set_master_mute(bool mute);
    bool toggle_master_mute();

    // Endpoints
    [[nodiscard]] std::vector<AudioEndpoint> get_output_endpoints() const;
    [[nodiscard]] std::vector<AudioEndpoint> get_input_endpoints() const;
    [[nodiscard]] std::optional<AudioEndpoint> get_default_output() const;
    [[nodiscard]] std::optional<AudioEndpoint> get_default_input() const;

    bool set_endpoint_volume(const std::string& endpoint_id, float volume);
    bool set_endpoint_mute(const std::string& endpoint_id, bool mute);

    // Callbacks
    void register_volume_callback(VolumeCallback cb);
    void register_endpoint_callback(EndpointListCallback cb);

    // Mocking / headless support
    void set_mock_endpoints(const std::vector<AudioEndpoint>& outputs,
                            const std::vector<AudioEndpoint>& inputs);
    void clear_mock_endpoints();
    [[nodiscard]] bool is_mocked() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace audio {
    BROSYS_API float get_master_volume();
    BROSYS_API bool set_master_volume(float volume);
    BROSYS_API bool is_master_muted();
    BROSYS_API bool set_master_mute(bool mute);
    BROSYS_API bool toggle_master_mute();
    BROSYS_API std::vector<AudioEndpoint> get_output_endpoints();
    BROSYS_API std::vector<AudioEndpoint> get_input_endpoints();
} // namespace audio

} // namespace brosys
