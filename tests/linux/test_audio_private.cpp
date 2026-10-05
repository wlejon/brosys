// AudioService against a private PipeWire + WirePlumber (+ pipewire-pulse)
// with null sinks / sources: the initial model, volume / mute / default set
// through the library and verified with wpctl and pactl, the same changes
// made by wpctl / pactl arriving as events, nodes appearing and vanishing
// at runtime (pw-cli), and the server going away and coming back (a real
// restart of pipewire + WirePlumber on the same socket).
#include "brosys/audio.h"
#include "check.h"
#include "linux/audio_cli.h"
#include "linux/fakes/event_log.h"
#include "linux/fakes/private_pipewire.h"

#include <cmath>
#include <optional>

using namespace std::chrono_literals;
using brosys::AudioDefaultChanged;
using brosys::AudioDeviceAdded;
using brosys::AudioDeviceChanged;
using brosys::AudioDeviceRemoved;
using brosys::AudioDirection;
namespace cli = bstest::audio_cli;

namespace {

constexpr const char* kName = "test_audio_private";
constexpr const char* kSinkA = "brosys_sink_a";
constexpr const char* kSinkB = "brosys_sink_b";
constexpr const char* kSrcA = "brosys_source_a";
constexpr const char* kSrcB = "brosys_source_b";

const brosys::AudioDevice* find(const brosys::AudioState& s, const std::string& id) {
    for (auto& d : s.devices)
        if (d.id == id) return &d;
    return nullptr;
}

// A copy: audio->state() returns a temporary.
std::optional<brosys::AudioDevice> current(brosys::AudioService& audio, const std::string& id) {
    auto s = audio.state();
    if (auto* d = find(s, id)) return *d;
    return std::nullopt;
}

bool near(float a, float b, float eps = 0.006f) { return std::fabs(a - b) < eps; }

void run_test() {
    // Sources rank above the sinks: WirePlumber would otherwise pick sink A's
    // monitor as the default source (which the backend reports as none).
    auto pwp = std::make_unique<bstest::PrivatePipeWire>(std::vector<bstest::NullNode>{
        {kSinkA, "Brosys Sink A", true, 2000},
        {kSinkB, "Brosys Sink B", true, 1000},
        {kSrcA, "Brosys Source A", false, 4000},
        {kSrcB, "Brosys Source B", false, 3000}});
    if (!pwp->ok()) {
        std::string why = pwp->error();
        pwp.reset();  // skip() exits without unwinding: stop the daemons first
        bstest::skip(kName, "private PipeWire: " + why);
    }
    auto& pw = *pwp;
    auto env = pw.env();

    // An unreachable remote fails cleanly.
    std::string err;
    brosys::AudioConfig bad;
    bad.pipewire_remote = pw.socket() + "-nope";
    CHECK(brosys::AudioService::create(bad, &err) == nullptr);
    CHECK(!err.empty());

    brosys::AudioConfig cfg;
    cfg.pipewire_remote = pw.socket();
    cfg.max_volume = 1.5f;
    auto audio = brosys::AudioService::create(cfg, &err);
    REQUIRE(audio);
    auto s = audio->state();
    bstest::EventLog<brosys::AudioEvent> log(audio->events());

    // ---- the initial model: one Added per device, then a default per direction
    auto first = audio->events().drain();
    REQUIRE(first.size() == 6);
    for (int i = 0; i < 4; ++i) CHECK(std::holds_alternative<AudioDeviceAdded>(first[static_cast<size_t>(i)]));
    REQUIRE(std::holds_alternative<AudioDefaultChanged>(first[4]) && std::holds_alternative<AudioDefaultChanged>(first[5]));
    CHECK_EQ(std::get<AudioDefaultChanged>(first[4]).id, std::string(kSinkA));
    CHECK_EQ(std::get<AudioDefaultChanged>(first[5]).id, std::string(kSrcA));
    CHECK_EQ(s.default_output, std::string(kSinkA));
    CHECK_EQ(s.default_input, std::string(kSrcA));
    REQUIRE(s.devices.size() == 4);
    for (auto& d : s.devices) {
        bool out = d.id.find("sink") != std::string::npos;
        CHECK(d.direction == (out ? AudioDirection::Output : AudioDirection::Input));
        CHECK(d.has_volume);
        CHECK_EQ(d.channel_volumes.size(), size_t(2));
        CHECK_EQ(d.is_default, d.id == kSinkA || d.id == kSrcA);
        CHECK_EQ(cli::wpctl_prop(std::to_string(d.native_id), "node.name", env), d.id);
        CHECK_EQ(cli::wpctl_prop(std::to_string(d.native_id), "node.description", env), d.description);
        auto v = cli::wpctl_volume(std::to_string(d.native_id), env);
        CHECK(v && near(v->volume, d.volume) && v->muted == d.muted);
    }
    const auto* sink_b = find(s, kSinkB);
    REQUIRE(sink_b != nullptr);
    std::string sink_b_target = std::to_string(sink_b->native_id);

    // Nothing more arrives while nothing changes.
    std::this_thread::sleep_for(300ms);
    CHECK(log.unseen().empty());

    // ---- volume through the library -> event, wpctl, pactl
    CHECK(audio->set_volume(kSinkB, 0.5f).ok);
    auto ch = log.wait<AudioDeviceChanged>([](const AudioDeviceChanged& c) {
        return c.device.id == kSinkB && (c.changes & brosys::audio_change::Volume) && near(c.device.volume, 0.5f, 1e-3f);
    });
    REQUIRE(ch.has_value());
    CHECK_EQ(ch->changes & brosys::audio_change::Mute, 0u);
    for (float c : ch->device.channel_volumes) CHECK(near(c, 0.5f, 1e-3f));
    auto v = cli::wpctl_volume(sink_b_target, env);
    CHECK(v && near(v->volume, 0.5f));
    if (pw.has_pulse()) {
        auto pct = cli::pactl_percent(true, kSinkB, env);
        CHECK(pct && *pct == 50);
    }
    auto now_b = current(*audio, kSinkB);
    CHECK(now_b && near(now_b->volume, 0.5f, 1e-3f));

    // Over-amplification is clamped to max_volume.
    CHECK(audio->set_volume(kSinkB, 3.0f).ok);
    ch = log.wait<AudioDeviceChanged>(
        [](const AudioDeviceChanged& c) { return c.device.id == kSinkB && near(c.device.volume, 1.5f, 1e-3f); });
    CHECK(ch.has_value());
    v = cli::wpctl_volume(sink_b_target, env);
    CHECK(v && near(v->volume, 1.5f));

    // ---- mute
    CHECK(audio->set_mute(kSinkB, true).ok);
    ch = log.wait<AudioDeviceChanged>([](const AudioDeviceChanged& c) { return c.device.id == kSinkB && c.device.muted; });
    REQUIRE(ch.has_value());
    CHECK(ch->changes & brosys::audio_change::Mute);
    v = cli::wpctl_volume(sink_b_target, env);
    CHECK(v && v->muted);
    if (pw.has_pulse()) CHECK(cli::pactl_mute(true, kSinkB, env) == std::optional<bool>(true));
    CHECK(audio->set_mute(kSinkB, false).ok);
    CHECK(log.wait<AudioDeviceChanged>([](const AudioDeviceChanged& c) { return c.device.id == kSinkB && !c.device.muted; }));

    // ---- default device, both directions
    CHECK(audio->set_default(kSinkB).ok);
    auto dc = log.wait<AudioDefaultChanged>([](const AudioDefaultChanged& d) { return d.direction == AudioDirection::Output; });
    REQUIRE(dc.has_value());
    CHECK_EQ(dc->id, std::string(kSinkB));
    CHECK_EQ(cli::wpctl_prop("@DEFAULT_AUDIO_SINK@", "node.name", env), std::string(kSinkB));
    CHECK(audio->state().default_output == kSinkB);
    auto def_b = current(*audio, kSinkB);
    auto def_a = current(*audio, kSinkA);
    CHECK(def_b && def_b->is_default && def_a && !def_a->is_default);
    CHECK(audio->set_default(kSrcB).ok);
    dc = log.wait<AudioDefaultChanged>([](const AudioDefaultChanged& d) { return d.direction == AudioDirection::Input; });
    CHECK(dc && dc->id == kSrcB);
    CHECK_EQ(cli::wpctl_prop("@DEFAULT_AUDIO_SOURCE@", "node.name", env), std::string(kSrcB));

    // ---- the other way round: wpctl / pactl change things, events follow
    auto sink_a = current(*audio, kSinkA);
    REQUIRE(sink_a.has_value());
    std::string sink_a_target = std::to_string(sink_a->native_id);
    CHECK_EQ(bstest::run({"wpctl", "set-volume", sink_a_target, "0.3"}, env).exit_code, 0);
    CHECK(log.wait<AudioDeviceChanged>(
        [](const AudioDeviceChanged& c) { return c.device.id == kSinkA && near(c.device.volume, 0.3f, 1e-3f); }));
    CHECK_EQ(bstest::run({"wpctl", "set-mute", sink_a_target, "1"}, env).exit_code, 0);
    CHECK(log.wait<AudioDeviceChanged>([](const AudioDeviceChanged& c) { return c.device.id == kSinkA && c.device.muted; }));
    CHECK_EQ(bstest::run({"wpctl", "set-default", sink_a_target}, env).exit_code, 0);
    dc = log.wait<AudioDefaultChanged>([](const AudioDefaultChanged& d) { return d.direction == AudioDirection::Output; });
    CHECK(dc && dc->id == kSinkA);
    if (pw.has_pulse()) {
        CHECK_EQ(bstest::run({"pactl", "set-sink-volume", kSinkA, "70%"}, env).exit_code, 0);
        CHECK(log.wait<AudioDeviceChanged>(
            [](const AudioDeviceChanged& c) { return c.device.id == kSinkA && near(c.device.volume, 0.7f, 2e-3f); }));
        CHECK_EQ(bstest::run({"pactl", "set-source-mute", kSrcA, "1"}, env).exit_code, 0);
        CHECK(log.wait<AudioDeviceChanged>([](const AudioDeviceChanged& c) { return c.device.id == kSrcA && c.device.muted; }));
    }

    // ---- a node appears and goes away at runtime
    if (bstest::have_program("pw-cli")) {
        auto r = bstest::run({"pw-cli", "create-node", "adapter",
                              "{ factory.name = support.null-audio-sink node.name = brosys_hot "
                              "node.description = \"Hot Plug\" media.class = Audio/Sink audio.position = [ FL FR ] "
                              "object.linger = true }"},
                             env);
        CHECK_EQ(r.exit_code, 0);
        auto added = log.wait<AudioDeviceAdded>([](const AudioDeviceAdded& a) { return a.device.id == "brosys_hot"; });
        REQUIRE(added.has_value());
        CHECK_EQ(added->device.description, std::string("Hot Plug"));
        CHECK(added->device.has_volume);
        CHECK(current(*audio, "brosys_hot").has_value());
        r = bstest::run({"pw-cli", "destroy", std::to_string(added->device.native_id)}, env);
        CHECK_EQ(r.exit_code, 0);
        auto removed = log.wait<AudioDeviceRemoved>([](const AudioDeviceRemoved& a) { return a.id == "brosys_hot"; });
        CHECK(removed && removed->direction == AudioDirection::Output);
        CHECK(!current(*audio, "brosys_hot").has_value());
    }

    // ---- mutations of unknown devices
    CHECK(!audio->set_volume("nope", 0.5f).ok);
    CHECK(!audio->set_mute("nope", true).ok);
    CHECK(!audio->set_default("nope").ok);

    // ---- the server goes away: every device is removed, defaults cleared
    pw.kill_pipewire();
    CHECK(bstest::wait_until([&] { return audio->state().devices.empty(); }, 10000ms));
    int removed = 0;
    for (auto& e : log.unseen()) removed += std::holds_alternative<AudioDeviceRemoved>(e);
    CHECK_EQ(removed, 4);
    CHECK(audio->state().default_output.empty() && audio->state().default_input.empty());
    CHECK(!audio->set_volume(kSinkA, 0.5f).ok);

    // ---- it comes back (same socket, config and WirePlumber state): the
    // service reconnects by itself and every device is added again, as
    // wpctl sees them; defaults are WirePlumber's (it may restore the one
    // set above), checked against wpctl rather than assumed.
    for (int round = 0; round < 2; ++round) {
        log.skip_all();
        REQUIRE(pw.restart_pipewire());
        CHECK(bstest::wait_until([&] {
            auto st = audio->state();
            return st.devices.size() == 4 && !st.default_output.empty() && !st.default_input.empty();
        }, 15000ms));
        int added = 0;
        for (auto& e : log.unseen()) added += std::holds_alternative<AudioDeviceAdded>(e);
        CHECK_EQ(added, 4);
        auto back = audio->state();
        for (auto& d : back.devices) {
            CHECK_EQ(cli::wpctl_prop(std::to_string(d.native_id), "node.name", env), d.id);
            auto v = cli::wpctl_volume(std::to_string(d.native_id), env);
            CHECK(v && near(v->volume, d.volume) && v->muted == d.muted);
        }
        CHECK_EQ(cli::wpctl_prop("@DEFAULT_AUDIO_SINK@", "node.name", env), back.default_output);
        CHECK_EQ(cli::wpctl_prop("@DEFAULT_AUDIO_SOURCE@", "node.name", env), back.default_input);
        // Mutations work on the new connection.
        float target = round == 0 ? 0.3f : 0.6f;
        CHECK(audio->set_volume(kSinkB, target).ok);
        CHECK(log.wait<AudioDeviceChanged>([&](const AudioDeviceChanged& c) {
            return c.device.id == kSinkB && near(c.device.volume, target, 1e-3f);
        }));
        auto cur = current(*audio, kSinkB);
        REQUIRE(cur.has_value());
        auto v = cli::wpctl_volume(std::to_string(cur->native_id), env);
        CHECK(v && near(v->volume, target));
        if (round == 0) pw.kill_pipewire();  // the next round starts from a dead server too
        if (round == 0) CHECK(bstest::wait_until([&] { return audio->state().devices.empty(); }, 10000ms));
    }
}

}  // namespace

int main() {
    run_test();
    return bstest::finish(kName);
}
