// Pure model of the PipeWire audio graph the backend mirrors: nodes, their
// devices' active routes, the "default" metadata. Turns it into AudioState
// and diffs states into events. No PipeWire types here, so tests can feed it.
#pragma once

#include "brosys/audio.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace brosys::pw {

using Dict = std::map<std::string, std::string>;

// One active route of a device (SPA_PARAM_Route).
struct Route {
    int32_t index = -1;
    int32_t device = -1;  // card.profile.device of the node it feeds
    bool output = true;
    std::string name;
    std::string description;
    std::string port_type;  // info "port.type": speaker, headphones, hdmi, spdif, mic, ...
    bool has_volume = false;
    std::vector<float> volumes;  // linear channelVolumes
    bool has_mute = false;
    bool mute = false;
};

struct Node {
    uint32_t id = 0;
    Dict props;            // node info props
    bool settled = false;  // info + subscribed params received
    bool has_volume = false;
    std::vector<float> volumes;  // linear channelVolumes (Props)
    bool has_mute = false;
    bool mute = false;
};

struct Device {
    uint32_t id = 0;
    Dict props;
    bool settled = false;
    std::vector<Route> routes;
};

struct Graph {
    std::map<uint32_t, Node> nodes;      // audio sink / source nodes only
    std::map<uint32_t, Device> devices;  // Audio/Device
    std::string default_sink;            // node.name from default.audio.sink ("" none)
    std::string default_source;
};

// Audio/Sink -> Output, Audio/Source (and Audio/Source/Virtual) -> Input;
// nullopt for streams, monitors, video, anything else.
std::optional<AudioDirection> direction_of(const std::string& media_class, const Dict& props);

// The cubic volume wpctl / pactl show: ui = cbrt(linear).
float linear_to_ui(float linear);
float ui_to_linear(float ui);

std::string form_factor_from_props(const Dict& props);       // device.form-factor, normalized; "" none
std::string form_factor_from_port_type(const std::string& t);

// The device route that carries `node` (device.id + card.profile.device), if any.
const Route* route_for_node(const Graph& g, const Node& node);
// Settled, and its device (when it has one in the graph) settled too.
bool node_ready(const Graph& g, const Node& node);

AudioState build_state(const Graph& g);

// One AudioDeviceAdded per device, then one AudioDefaultChanged per direction.
std::vector<AudioEvent> initial_events(const AudioState& s);
// Removed, Added, Changed (with bits), then AudioDefaultChanged per changed direction.
std::vector<AudioEvent> diff(const AudioState& before, const AudioState& after);

// "default.audio.sink" values: {"name":"alsa_output..."} -> the name ("" when unparsable).
std::string parse_default_name(const std::string& json);
std::string default_json(const std::string& name);

}  // namespace brosys::pw
