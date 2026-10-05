#include "linux/audio/pw_pod.h"

#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/defs.h>

namespace brosys::pw {

namespace {

constexpr uint32_t kMaxChannels = 64;

std::vector<float> float_array(const spa_pod* pod) {
    float vals[kMaxChannels];
    uint32_t n = spa_pod_copy_array(pod, SPA_TYPE_Float, vals, kMaxChannels);
    return std::vector<float>(vals, vals + n);
}

// Route info: Struct(Int n, String key, String value, ...).
void parse_info(const spa_pod* pod, Route* out) {
    if (!spa_pod_is_struct(pod)) return;
    bool first = true;
    const char* key = nullptr;
    const void* body = SPA_POD_BODY_CONST(pod);
    uint32_t size = SPA_POD_BODY_SIZE(pod);
    for (auto* it = static_cast<const spa_pod*>(body); spa_pod_is_inside(body, size, it);
         it = static_cast<const spa_pod*>(spa_pod_next(it))) {
        if (first) {  // the count
            first = false;
            continue;
        }
        const char* s = nullptr;
        if (spa_pod_get_string(it, &s) < 0) continue;
        if (!key) {
            key = s;
            continue;
        }
        if (key && s && std::string(key) == "port.type") out->port_type = s;
        key = nullptr;
    }
}

}  // namespace

bool parse_props(const spa_pod* pod, PropsValues* out) {
    if (!pod || !spa_pod_is_object_type(pod, SPA_TYPE_OBJECT_Props)) return false;
    auto* obj = reinterpret_cast<const spa_pod_object*>(pod);
    const spa_pod_prop* prop;
    SPA_POD_OBJECT_FOREACH(obj, prop) {
        switch (prop->key) {
            case SPA_PROP_channelVolumes:
                out->volumes = float_array(&prop->value);
                out->has_volume = !out->volumes.empty();
                break;
            case SPA_PROP_mute: {
                bool b = false;
                if (spa_pod_get_bool(&prop->value, &b) >= 0) {
                    out->mute = b;
                    out->has_mute = true;
                }
                break;
            }
            default: break;
        }
    }
    return true;
}

bool parse_route(const spa_pod* pod, Route* out) {
    if (!pod || !spa_pod_is_object_type(pod, SPA_TYPE_OBJECT_ParamRoute)) return false;
    auto* obj = reinterpret_cast<const spa_pod_object*>(pod);
    const spa_pod_prop* prop;
    SPA_POD_OBJECT_FOREACH(obj, prop) {
        const spa_pod* v = &prop->value;
        switch (prop->key) {
            case SPA_PARAM_ROUTE_index: spa_pod_get_int(v, &out->index); break;
            case SPA_PARAM_ROUTE_device: spa_pod_get_int(v, &out->device); break;
            case SPA_PARAM_ROUTE_direction: {
                uint32_t d = 0;
                if (spa_pod_get_id(v, &d) >= 0) out->output = d == SPA_DIRECTION_OUTPUT;
                break;
            }
            case SPA_PARAM_ROUTE_name: {
                const char* s = nullptr;
                if (spa_pod_get_string(v, &s) >= 0 && s) out->name = s;
                break;
            }
            case SPA_PARAM_ROUTE_description: {
                const char* s = nullptr;
                if (spa_pod_get_string(v, &s) >= 0 && s) out->description = s;
                break;
            }
            case SPA_PARAM_ROUTE_info: parse_info(v, out); break;
            case SPA_PARAM_ROUTE_props: {
                PropsValues p;
                if (parse_props(v, &p)) {
                    out->has_volume = p.has_volume;
                    out->volumes = std::move(p.volumes);
                    out->has_mute = p.has_mute;
                    out->mute = p.mute;
                }
                break;
            }
            default: break;
        }
    }
    return true;
}

namespace {

void add_props_body(spa_pod_builder* b, const std::vector<float>* volumes, std::optional<bool> mute) {
    if (volumes && !volumes->empty()) {
        spa_pod_builder_prop(b, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_array(b, sizeof(float), SPA_TYPE_Float, static_cast<uint32_t>(volumes->size()),
                              volumes->data());
    }
    if (mute) {
        spa_pod_builder_prop(b, SPA_PROP_mute, 0);
        spa_pod_builder_bool(b, *mute);
    }
}

}  // namespace

const spa_pod* build_props(spa_pod_builder* b, const std::vector<float>* volumes, std::optional<bool> mute) {
    spa_pod_frame f;
    spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    add_props_body(b, volumes, mute);
    return static_cast<const spa_pod*>(spa_pod_builder_pop(b, &f));
}

const spa_pod* build_route(spa_pod_builder* b, const Route& route, const std::vector<float>* volumes,
                           std::optional<bool> mute) {
    spa_pod_frame outer, inner;
    spa_pod_builder_push_object(b, &outer, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
    spa_pod_builder_prop(b, SPA_PARAM_ROUTE_index, 0);
    spa_pod_builder_int(b, route.index);
    spa_pod_builder_prop(b, SPA_PARAM_ROUTE_device, 0);
    spa_pod_builder_int(b, route.device);
    spa_pod_builder_prop(b, SPA_PARAM_ROUTE_props, 0);
    spa_pod_builder_push_object(b, &inner, SPA_TYPE_OBJECT_Props, SPA_PARAM_Route);
    add_props_body(b, volumes, mute);
    spa_pod_builder_pop(b, &inner);
    spa_pod_builder_prop(b, SPA_PARAM_ROUTE_save, 0);
    spa_pod_builder_bool(b, true);
    return static_cast<const spa_pod*>(spa_pod_builder_pop(b, &outer));
}

}  // namespace brosys::pw
