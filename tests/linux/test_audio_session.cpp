// AudioService against the user's PipeWire session, read-only: every
// device must be a sink / source wpctl lists (same node.name, description,
// direction), with the cubic volume and mute wpctl and pactl report, and the
// defaults must be what @DEFAULT_AUDIO_SINK@ / @DEFAULT_AUDIO_SOURCE@
// resolve to. Nothing is changed.
#include "brosys/audio.h"
#include "check.h"
#include "linux/audio_cli.h"

#include <cmath>
#include <cstdlib>
#include <unistd.h>

namespace {

constexpr const char* kName = "test_audio_session";

// Sinks / sources in the Audio section of `wpctl status`.
std::pair<int, int> wpctl_counts() {
    auto r = bstest::run({"wpctl", "status"});
    std::istringstream in(r.out);
    std::string line;
    bool audio = false;
    int section = 0;  // 1 sinks, 2 sources
    int sinks = 0, sources = 0;
    while (std::getline(in, line)) {
        if (line == "Audio") audio = true;
        else if (line == "Video" || line == "Settings") audio = false;
        if (!audio) continue;
        if (line.find("Sinks:") != std::string::npos) section = 1;
        else if (line.find("Sources:") != std::string::npos) section = 2;
        else if (line.find("Filters:") != std::string::npos || line.find("Streams:") != std::string::npos ||
                 line.find("Devices:") != std::string::npos)
            section = 0;
        else if (section && line.find(". ") != std::string::npos && line.find("[vol:") != std::string::npos)
            (section == 1 ? sinks : sources)++;
    }
    return {sinks, sources};
}

void run_test() {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    std::string socket = std::string(rt ? rt : "") + "/pipewire-0";
    if (!rt || access(socket.c_str(), F_OK) != 0) bstest::skip(kName, "no PipeWire session socket (" + socket + ")");
    if (!bstest::have_program("wpctl")) bstest::skip(kName, "wpctl not installed");

    std::string err;
    auto audio = brosys::AudioService::create(brosys::AudioConfig(), &err);
    if (!audio) bstest::skip(kName, "cannot connect to PipeWire: " + err);
    auto s = audio->state();

    // Events queued before create() returned rebuild the same state.
    // (Taken right after create(), so nothing but the initial events is queued
    // unless the session changed in between, which a read-only test tolerates.)
    auto evs = audio->events().drain();
    brosys::AudioState rebuilt;
    REQUIRE(evs.size() >= s.devices.size() + 2);
    for (size_t i = 0; i < s.devices.size(); ++i) {
        auto* a = std::get_if<brosys::AudioDeviceAdded>(&evs[i]);
        CHECK(a != nullptr);
        if (a) rebuilt.devices.push_back(a->device);
    }
    for (size_t i = s.devices.size(); i < s.devices.size() + 2; ++i) {
        auto* d = std::get_if<brosys::AudioDefaultChanged>(&evs[i]);
        CHECK(d != nullptr);
        if (d) (d->direction == brosys::AudioDirection::Output ? rebuilt.default_output : rebuilt.default_input) = d->id;
    }
    if (evs.size() == s.devices.size() + 2) CHECK(rebuilt == s);

    auto [sinks, sources] = wpctl_counts();
    int outs = 0, ins = 0;
    for (auto& d : s.devices) (d.direction == brosys::AudioDirection::Output ? outs : ins)++;
    std::printf("devices: %d output(s) (wpctl %d), %d input(s) (wpctl %d)\n", outs, sinks, ins, sources);
    CHECK_EQ(outs, sinks);
    CHECK_EQ(ins, sources);

    bool pactl = bstest::have_program("pactl");
    for (auto& d : s.devices) {
        std::string target = std::to_string(d.native_id);
        std::printf("%s %-60s '%s' card='%s' form='%s' vol=%.3f muted=%d default=%d\n", brosys::to_string(d.direction),
                    d.id.c_str(), d.description.c_str(), d.device_name.c_str(), d.form_factor.c_str(), d.volume, d.muted,
                    d.is_default);
        CHECK_EQ(bstest::audio_cli::wpctl_prop(target, "node.name"), d.id);
        CHECK_EQ(bstest::audio_cli::wpctl_prop(target, "node.description"), d.description);
        std::string cls = bstest::audio_cli::wpctl_prop(target, "media.class");
        CHECK_EQ(cls.rfind(d.direction == brosys::AudioDirection::Output ? "Audio/Sink" : "Audio/Source", 0), size_t(0));
        auto v = bstest::audio_cli::wpctl_volume(target);
        CHECK(v.has_value());
        if (v) {
            CHECK(std::fabs(v->volume - d.volume) < 0.006f);  // wpctl prints two decimals
            CHECK_EQ(v->muted, d.muted);
        }
        CHECK(d.has_volume);
        for (float c : d.channel_volumes) CHECK(c >= 0.0f);
        if (pactl) {
            bool sink = d.direction == brosys::AudioDirection::Output;
            auto pct = bstest::audio_cli::pactl_percent(sink, d.id);
            CHECK(pct.has_value());
            if (pct) CHECK(std::abs(*pct - static_cast<int>(std::lround(d.channel_volumes.at(0) * 100))) <= 1);
            auto m = bstest::audio_cli::pactl_mute(sink, d.id);
            CHECK(m && *m == d.muted);
        }
    }

    // Defaults: what wpctl resolves, when it is a device of that direction
    // (WirePlumber may point the default source at a sink's monitor).
    auto expect_default = [&](const char* alias, const char* cls_prefix) {
        std::string name = bstest::audio_cli::wpctl_prop(alias, "node.name");
        std::string cls = bstest::audio_cli::wpctl_prop(alias, "media.class");
        return cls.rfind(cls_prefix, 0) == 0 ? name : std::string();
    };
    std::string def_out = expect_default("@DEFAULT_AUDIO_SINK@", "Audio/Sink");
    std::string def_in = expect_default("@DEFAULT_AUDIO_SOURCE@", "Audio/Source");
    std::printf("default output '%s' (wpctl '%s'), input '%s' (wpctl '%s')\n", s.default_output.c_str(),
                def_out.c_str(), s.default_input.c_str(), def_in.c_str());
    CHECK_EQ(s.default_output, def_out);
    CHECK_EQ(s.default_input, def_in);

    // Mutations of unknown devices fail without touching anything.
    CHECK(!audio->set_volume("brosys-no-such-device", 0.5f).ok);
    CHECK(!audio->set_mute("brosys-no-such-device", true).ok);
    CHECK(!audio->set_default("brosys-no-such-device").ok);
}

}  // namespace

int main() {
    run_test();
    return bstest::finish(kName);
}
