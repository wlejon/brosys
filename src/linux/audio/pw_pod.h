// SPA pod reading / writing for the two params the audio backend uses:
// Props (node volume / mute) and Route (device route + its props).
#pragma once

#include "linux/audio/pw_model.h"

#include <cstdint>
#include <optional>
#include <vector>

struct spa_pod;
struct spa_pod_builder;

namespace brosys::pw {

struct PropsValues {
    bool has_volume = false;
    std::vector<float> volumes;  // linear channelVolumes
    bool has_mute = false;
    bool mute = false;
};

// SPA_TYPE_OBJECT_Props -> values; false when it is not a Props object.
bool parse_props(const spa_pod* pod, PropsValues* out);
// SPA_TYPE_OBJECT_ParamRoute -> Route; false when it is not a Route object.
bool parse_route(const spa_pod* pod, Route* out);

// Props { channelVolumes?, mute? } (for pw_node_set_param(SPA_PARAM_Props)).
const spa_pod* build_props(spa_pod_builder* b, const std::vector<float>* volumes, std::optional<bool> mute);
// Route { index, device, props: Props {...}, save: true } (pw_device_set_param(SPA_PARAM_Route)),
// the way wpctl / pactl change a device's volume.
const spa_pod* build_route(spa_pod_builder* b, const Route& route, const std::vector<float>* volumes,
                           std::optional<bool> mute);

}  // namespace brosys::pw
