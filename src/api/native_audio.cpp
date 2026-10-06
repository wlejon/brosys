#include "api.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <string>

namespace brosys::api {

namespace {

Value buildAudioDevice(const AudioDevice& d) {
    ObjectBuilder b;
    b.set("id", d.id);
    b.set("description", d.description);
    b.set("deviceName", d.device_name);
    b.set("formFactor", d.form_factor);
    b.set("direction", to_string(d.direction));
    b.set("state", to_string(d.state));
    b.set("isDefault", d.is_default);
    b.set("hasVolume", d.has_volume);
    b.set("volume", static_cast<double>(d.volume));
    b.set("muted", d.muted);
    b.set("nativeId", static_cast<double>(d.native_id));

    ArrayBuilder chans(d.channel_volumes.size());
    for (size_t i = 0; i < d.channel_volumes.size(); ++i) {
        chans.set(static_cast<uint32_t>(i), static_cast<double>(d.channel_volumes[i]));
    }
    b.set("channelVolumes", chans.build());
    return b.build();
}

Value buildAudioState(const AudioState& s) {
    ObjectBuilder b;
    b.set("defaultOutput", s.default_output);
    b.set("defaultInput", s.default_input);

    ArrayBuilder devs(s.devices.size());
    for (size_t i = 0; i < s.devices.size(); ++i) {
        ev::Persistent devVal(buildAudioDevice(s.devices[i]));
        devs.set(static_cast<uint32_t>(i), devVal.get());
    }
    b.set("devices", devs.build());
    return b.build();
}

} // namespace

void installAudio(ObjectBuilder& sys) {
    ObjectBuilder audio;

    audio.def("getState", 0, [](Value, std::span<const Value>) {
        AudioService* svc = getAudioService();
        if (!svc) {
            AudioState empty;
            return buildAudioState(empty);
        }
        return buildAudioState(svc->state());
    });

    audio.def("getDevices", 0, [](Value, std::span<const Value>) {
        AudioService* svc = getAudioService();
        if (!svc) {
            ArrayBuilder arr(0);
            return arr.build();
        }
        auto s = svc->state();
        ArrayBuilder arr(s.devices.size());
        for (size_t i = 0; i < s.devices.size(); ++i) {
            ev::Persistent devVal(buildAudioDevice(s.devices[i]));
            arr.set(static_cast<uint32_t>(i), devVal.get());
        }
        return arr.build();
    });

    audio.def("setVolume", 2, [](Value, std::span<const Value> args) {
        AudioService* svc = getAudioService();
        if (!svc) return ev::throwError("AudioService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());

        std::string id = strVal(arg0.get());
        double vol = numVal(arg1.get());

        Result r = svc->set_volume(id, static_cast<float>(vol));
        if (!r.ok) {
            return ev::throwError("Audio setVolume failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    audio.def("setMute", 2, [](Value, std::span<const Value> args) {
        AudioService* svc = getAudioService();
        if (!svc) return ev::throwError("AudioService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());

        std::string id = strVal(arg0.get());
        bool muted = boolVal(arg1.get());

        Result r = svc->set_mute(id, muted);
        if (!r.ok) {
            return ev::throwError("Audio setMute failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    audio.def("setDefault", 1, [](Value, std::span<const Value> args) {
        AudioService* svc = getAudioService();
        if (!svc) return ev::throwError("AudioService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string id = strVal(arg0.get());

        Result r = svc->set_default(id);
        if (!r.ok) {
            return ev::throwError("Audio setDefault failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    audio.def("setDefaultSink", 1, [](Value, std::span<const Value> args) {
        AudioService* svc = getAudioService();
        if (!svc) return ev::throwError("AudioService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string id = strVal(arg0.get());

        Result r = svc->set_default(id);
        if (!r.ok) {
            return ev::throwError("Audio setDefaultSink failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    audio.def("setDefaultSource", 1, [](Value, std::span<const Value> args) {
        AudioService* svc = getAudioService();
        if (!svc) return ev::throwError("AudioService is unavailable");

        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        std::string id = strVal(arg0.get());

        Result r = svc->set_default(id);
        if (!r.ok) {
            return ev::throwError("Audio setDefaultSource failed: " + r.error);
        }
        return ev::fromBool(true);
    });

    audio.def("on", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        addEventListener("audio", evName, arg1.get());
        return ev::fromBool(true);
    });

    audio.def("off", 2, [](Value, std::span<const Value> args) {
        ev::Persistent arg0(args.size() > 0 ? args[0] : ev::undefined());
        ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());
        std::string evName = strVal(arg0.get());
        removeEventListener("audio", evName, arg1.get());
        return ev::fromBool(true);
    });

    sys.set("audio", audio.build());
}

void tickAudio() {
    AudioService* svc = getAudioService();
    if (!svc) return;

    auto events = svc->events().drain();
    for (const auto& evItem : events) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, AudioDeviceAdded>) {
                ev::Persistent p(buildAudioDevice(e.device));
                dispatchEvent("audio", "deviceAdded", p.get());
            } else if constexpr (std::is_same_v<T, AudioDeviceRemoved>) {
                ObjectBuilder b;
                b.set("id", e.id);
                b.set("direction", to_string(e.direction));
                dispatchEvent("audio", "deviceRemoved", b.build());
            } else if constexpr (std::is_same_v<T, AudioDeviceChanged>) {
                ObjectBuilder b;
                ev::Persistent devVal(buildAudioDevice(e.device));
                b.set("device", devVal.get());
                b.set("changes", static_cast<double>(e.changes));
                dispatchEvent("audio", "deviceChanged", b.build());
            } else if constexpr (std::is_same_v<T, AudioDefaultChanged>) {
                ObjectBuilder b;
                b.set("direction", to_string(e.direction));
                b.set("id", e.id);
                dispatchEvent("audio", "defaultChanged", b.build());
            }
        }, evItem);
    }
}

void shutdownAudio() {
}

} // namespace brosys::api
