// Audio: output (sink) and input (source) devices, the default device per
// direction, volume / mute, and change events.
//
// Linux: a PipeWire client (audio Sink / Source nodes, the "default"
// metadata, device Route volumes the way wpctl / pactl set them). When the
// PipeWire server goes away every device is removed; the client reconnects
// to the same remote when it is back and the devices are added again.
// Windows: Core Audio (IMMDeviceEnumerator, IMMNotificationClient,
// IAudioEndpointVolume + IAudioEndpointVolumeCallback).
// macOS: the CoreAudio HAL with property listeners. A device appears once per
// direction it has streams for, with id "out:<UID>" / "in:<UID>"; volume is
// the virtual main volume (the Sound menu's slider), clamped to 1.0; hidden
// devices are left out.
//
// Volume is the value the OS mixer UI shows: the endpoint scalar on Windows,
// the cubic ("wpctl" / pactl percent) volume on PipeWire. 1.0 is 100 %;
// PipeWire allows over-amplification (> 1.0) up to AudioConfig::max_volume.
#pragma once

#include "brosys/common.h"
#include "brosys/event_queue.h"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace brosys {

enum class AudioDirection { Output, Input };

enum class AudioDeviceState { Active, Unplugged, Disabled, NotPresent };

struct AudioDevice {
    std::string id;           // stable: endpoint id string (Windows), node.name (PipeWire)
    std::string description;  // human name: "Speakers (Realtek(R) Audio)", node.description
    std::string device_name;  // the card / adapter: "Realtek(R) Audio", device.description
    std::string form_factor;  // speakers, headphones, headset, hdmi, microphone, ... ("" unknown)
    AudioDirection direction = AudioDirection::Output;
    AudioDeviceState state = AudioDeviceState::Active;
    bool is_default = false;
    bool has_volume = true;      // the device exposes a volume control
    float volume = 0.0f;         // UI scale, see top of file
    std::vector<float> channel_volumes;  // same scale, per channel (empty when unknown)
    bool muted = false;
    uint32_t native_id = 0;      // PipeWire global id (0 on Windows); informational

    bool operator==(const AudioDevice&) const = default;
};

struct AudioState {
    std::vector<AudioDevice> devices;  // active devices of both directions
    std::string default_output;        // id, "" when none
    std::string default_input;

    bool operator==(const AudioState&) const = default;
};

// Bits for AudioDeviceChanged::changes.
namespace audio_change {
inline constexpr uint32_t Volume = 1u << 0;
inline constexpr uint32_t Mute = 1u << 1;
inline constexpr uint32_t Description = 1u << 2;
inline constexpr uint32_t State = 1u << 3;
inline constexpr uint32_t Default = 1u << 4;
}  // namespace audio_change

struct AudioDeviceAdded {
    AudioDevice device;
};
struct AudioDeviceRemoved {
    std::string id;
    AudioDirection direction = AudioDirection::Output;
};
struct AudioDeviceChanged {
    AudioDevice device;
    uint32_t changes = 0;
};
// The default device of a direction changed ("" = there is none now).
struct AudioDefaultChanged {
    AudioDirection direction = AudioDirection::Output;
    std::string id;
};

using AudioEvent = std::variant<AudioDeviceAdded, AudioDeviceRemoved, AudioDeviceChanged, AudioDefaultChanged>;
using AudioEventQueue = MessageQueue<AudioEvent>;

struct AudioConfig {
    // Linux: PipeWire remote name (PIPEWIRE_REMOTE semantics); empty = the
    // default ("pipewire-0" in $XDG_RUNTIME_DIR).
    std::string pipewire_remote;
    float max_volume = 1.5f;  // clamp for set_volume (Windows and macOS always clamp to 1.0)
};

class AudioService {
public:
    // Connects and takes an initial snapshot. One AudioDeviceAdded per
    // device and one AudioDefaultChanged per direction are queued before
    // create() returns, so a host can build its model from events alone.
    static std::unique_ptr<AudioService> create(const AudioConfig& config, std::string* error);
    virtual ~AudioService() = default;

    virtual AudioEventQueue& events() = 0;
    virtual AudioState state() const = 0;

    // Mutations. Completion is observed as AudioDeviceChanged /
    // AudioDefaultChanged events.
    virtual Result set_volume(const std::string& id, float volume) = 0;  // all channels
    virtual Result set_mute(const std::string& id, bool muted) = 0;
    // Windows uses the undocumented-but-stable IPolicyConfig interface the
    // Sound control panel uses; PipeWire sets default.configured.audio.*.
    virtual Result set_default(const std::string& id) = 0;
};

const char* to_string(AudioDirection d);
const char* to_string(AudioDeviceState s);

}  // namespace brosys
