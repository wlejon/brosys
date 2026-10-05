#include "linux/audio/pw_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace brosys::pw {

namespace {

std::string get(const Dict& d, const char* key) {
    auto it = d.find(key);
    return it == d.end() ? std::string() : it->second;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

uint32_t changes_between(const AudioDevice& a, const AudioDevice& b) {
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

}  // namespace

std::optional<AudioDirection> direction_of(const std::string& media_class, const Dict& props) {
    if (get(props, "stream.monitor") == "true" || ends_with(get(props, "node.name"), ".monitor")) return std::nullopt;
    if (media_class == "Audio/Sink") return AudioDirection::Output;
    if (media_class == "Audio/Source" || media_class == "Audio/Source/Virtual") return AudioDirection::Input;
    return std::nullopt;
}

float linear_to_ui(float linear) { return linear <= 0.0f ? 0.0f : std::cbrt(linear); }
float ui_to_linear(float ui) { return ui <= 0.0f ? 0.0f : ui * ui * ui; }

std::string form_factor_from_port_type(const std::string& type) {
    std::string t = lower(type);
    if (t.empty() || t == "unknown") return {};
    if (t == "speaker") return "speakers";
    if (t == "mic") return "microphone";
    if (t == "earpiece") return "handset";
    return t;  // headphones, headset, hdmi, spdif, line, handset, ...
}

std::string form_factor_from_props(const Dict& props) {
    std::string f = get(props, "device.form-factor");
    if (f.empty()) f = get(props, "device.form_factor");
    f = lower(f);
    if (f == "speaker") return "speakers";
    if (f == "headphone") return "headphones";
    return f;  // internal, handset, tv, webcam, microphone, headset, hands-free, car, hifi, computer, portable
}

const Route* route_for_node(const Graph& g, const Node& node) {
    std::string dev = get(node.props, "device.id");
    std::string pd = get(node.props, "card.profile.device");
    if (dev.empty() || pd.empty()) return nullptr;
    uint32_t dev_id = 0;
    int32_t profile_device = -1;
    try {
        dev_id = static_cast<uint32_t>(std::stoul(dev));
        profile_device = static_cast<int32_t>(std::stol(pd));
    } catch (...) {
        return nullptr;
    }
    auto it = g.devices.find(dev_id);
    if (it == g.devices.end()) return nullptr;
    for (auto& r : it->second.routes)
        if (r.device == profile_device) return &r;
    return nullptr;
}

bool node_ready(const Graph& g, const Node& node) {
    if (!node.settled) return false;
    std::string dev = get(node.props, "device.id");
    if (dev.empty()) return true;
    try {
        auto it = g.devices.find(static_cast<uint32_t>(std::stoul(dev)));
        return it == g.devices.end() || it->second.settled;
    } catch (...) {
        return true;
    }
}

AudioState build_state(const Graph& g) {
    AudioState s;
    for (auto& [id, node] : g.nodes) {
        auto dir = direction_of(get(node.props, "media.class"), node.props);
        if (!dir || !node_ready(g, node)) continue;
        AudioDevice d;
        d.id = get(node.props, "node.name");
        if (d.id.empty()) continue;
        d.direction = *dir;
        d.native_id = id;
        d.description = get(node.props, "node.description");
        if (d.description.empty()) d.description = get(node.props, "node.nick");
        if (d.description.empty()) d.description = d.id;

        const Device* device = nullptr;
        try {
            std::string dev = get(node.props, "device.id");
            if (!dev.empty()) {
                auto it = g.devices.find(static_cast<uint32_t>(std::stoul(dev)));
                if (it != g.devices.end()) device = &it->second;
            }
        } catch (...) {
        }
        const Route* route = route_for_node(g, node);
        d.device_name = device ? get(device->props, "device.description") : std::string();
        if (d.device_name.empty()) d.device_name = get(node.props, "device.description");
        d.form_factor = form_factor_from_props(node.props);
        if (d.form_factor.empty() && device) d.form_factor = form_factor_from_props(device->props);
        if (d.form_factor.empty() && route) d.form_factor = form_factor_from_port_type(route->port_type);

        const std::vector<float>* linear = nullptr;
        if (node.has_volume) linear = &node.volumes;
        else if (route && route->has_volume) linear = &route->volumes;
        d.has_volume = linear && !linear->empty();
        if (d.has_volume) {
            for (float v : *linear) {
                float ui = linear_to_ui(v);
                d.channel_volumes.push_back(ui);
                d.volume = std::max(d.volume, ui);
            }
        }
        if (node.has_mute) d.muted = node.mute;
        else if (route && route->has_mute) d.muted = route->mute;

        std::string& def = d.direction == AudioDirection::Output ? s.default_output : s.default_input;
        const std::string& want = d.direction == AudioDirection::Output ? g.default_sink : g.default_source;
        if (!want.empty() && want == d.id) {
            d.is_default = true;
            def = d.id;
        }
        s.devices.push_back(std::move(d));
    }
    return s;
}

std::vector<AudioEvent> initial_events(const AudioState& s) {
    std::vector<AudioEvent> out;
    for (auto& d : s.devices) out.emplace_back(AudioDeviceAdded{d});
    out.emplace_back(AudioDefaultChanged{AudioDirection::Output, s.default_output});
    out.emplace_back(AudioDefaultChanged{AudioDirection::Input, s.default_input});
    return out;
}

std::vector<AudioEvent> diff(const AudioState& before, const AudioState& after) {
    std::vector<AudioEvent> out;
    auto find = [](const AudioState& s, const AudioDevice& d) -> const AudioDevice* {
        for (auto& x : s.devices)
            if (x.id == d.id && x.direction == d.direction) return &x;
        return nullptr;
    };
    for (auto& d : before.devices)
        if (!find(after, d)) out.emplace_back(AudioDeviceRemoved{d.id, d.direction});
    for (auto& d : after.devices)
        if (!find(before, d)) out.emplace_back(AudioDeviceAdded{d});
    for (auto& d : after.devices) {
        const AudioDevice* old = find(before, d);
        if (!old) continue;
        uint32_t c = changes_between(*old, d);
        if (c == 0 && old->native_id == d.native_id) continue;
        out.emplace_back(AudioDeviceChanged{d, c});
    }
    if (before.default_output != after.default_output)
        out.emplace_back(AudioDefaultChanged{AudioDirection::Output, after.default_output});
    if (before.default_input != after.default_input)
        out.emplace_back(AudioDefaultChanged{AudioDirection::Input, after.default_input});
    return out;
}

std::string parse_default_name(const std::string& json) {
    // SPA JSON is relaxed JSON: keys may be bare and '=' may separate them.
    size_t i = json.find("name");
    while (i != std::string::npos) {
        bool quoted = i > 0 && json[i - 1] == '"';
        size_t j = i + 4;
        if (quoted) {
            if (j >= json.size() || json[j] != '"') {
                i = json.find("name", i + 4);
                continue;
            }
            ++j;
        }
        while (j < json.size() && std::isspace(static_cast<unsigned char>(json[j]))) ++j;
        if (j < json.size() && (json[j] == ':' || json[j] == '=')) {
            ++j;
            while (j < json.size() && std::isspace(static_cast<unsigned char>(json[j]))) ++j;
            std::string out;
            if (j < json.size() && json[j] == '"') {
                for (++j; j < json.size() && json[j] != '"'; ++j) {
                    char c = json[j];
                    if (c == '\\' && j + 1 < json.size()) {
                        char e = json[++j];
                        switch (e) {
                            case 'n': out += '\n'; break;
                            case 't': out += '\t'; break;
                            case 'r': out += '\r'; break;
                            case 'b': out += '\b'; break;
                            case 'f': out += '\f'; break;
                            case 'u':
                                if (j + 4 < json.size() &&
                                    std::all_of(json.begin() + static_cast<std::ptrdiff_t>(j) + 1,
                                                json.begin() + static_cast<std::ptrdiff_t>(j) + 5,
                                                [](char h) { return std::isxdigit(static_cast<unsigned char>(h)) != 0; })) {
                                    unsigned cp = static_cast<unsigned>(std::stoul(json.substr(j + 1, 4), nullptr, 16));
                                    j += 4;
                                    if (cp < 0x80) {
                                        out += static_cast<char>(cp);
                                    } else if (cp < 0x800) {
                                        out += static_cast<char>(0xC0 | (cp >> 6));
                                        out += static_cast<char>(0x80 | (cp & 0x3F));
                                    } else {
                                        out += static_cast<char>(0xE0 | (cp >> 12));
                                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                        out += static_cast<char>(0x80 | (cp & 0x3F));
                                    }
                                }
                                break;
                            default: out += e; break;
                        }
                    } else {
                        out += c;
                    }
                }
                return out;
            }
            while (j < json.size() && !std::isspace(static_cast<unsigned char>(json[j])) && json[j] != ',' &&
                   json[j] != '}')
                out += json[j++];
            return out;
        }
        i = json.find("name", i + 4);
    }
    return {};
}

std::string default_json(const std::string& name) {
    std::string out = "{ \"name\": \"";
    for (char c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            static const char* hex = "0123456789abcdef";
            out += "\\u00";
            out += hex[u >> 4];
            out += hex[u & 0xF];
        } else {
            out += c;
        }
    }
    out += "\" }";
    return out;
}

}  // namespace brosys::pw
