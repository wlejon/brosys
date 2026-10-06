// macOS audio against the OS's own answers: SwitchAudioSource (Homebrew
// switchaudio-osx) for the device list and the defaults, osascript's "get
// volume settings" for the default devices' volume and mute,
// system_profiler SPAudioDataType for the manufacturers. Read-only toward
// the Mac: the only writes are same-value ones (the current volume, mute and
// default written back), and the Added / Removed events come from a
// *private* aggregate device, which only this process can see and which is
// destroyed again.
#include "check.h"
#include "mac/support/oracle.h"

#include "brosys/audio.h"

#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>

#include <cmath>
#include <map>
#include <set>

using namespace brosys;
using namespace std::chrono_literals;
namespace om = bstest::mac;

namespace {

const AudioDevice* find(const AudioState& s, const std::string& id) {
    for (auto& d : s.devices)
        if (d.id == id) return &d;
    return nullptr;
}

std::string id_of(const std::string& type, const std::string& uid) { return (type == "output" ? "out:" : "in:") + uid; }

void test_initial_events(AudioService& svc) {
    // One Added per device and one DefaultChanged per direction, before create() returned.
    AudioState s = svc.state();
    std::set<std::string> added;
    int defaults = 0;
    for (auto& e : svc.events().drain()) {
        if (auto* a = std::get_if<AudioDeviceAdded>(&e)) added.insert(a->device.id);
        if (auto* d = std::get_if<AudioDefaultChanged>(&e)) {
            ++defaults;
            CHECK_EQ(d->id, d->direction == AudioDirection::Output ? s.default_output : s.default_input);
        }
    }
    CHECK_EQ(added.size(), s.devices.size());
    CHECK_EQ(defaults, 2);
}

void test_devices(AudioService& svc) {
    AudioState s = svc.state();
    for (auto& d : s.devices)
        std::printf("  %s '%s' by '%s' ff=%s %s default=%d vol=%.4f (%zu ch) muted=%d has_volume=%d\n", d.id.c_str(),
                    d.description.c_str(), d.device_name.c_str(), d.form_factor.c_str(), to_string(d.direction),
                    d.is_default, d.volume, d.channel_volumes.size(), d.muted, d.has_volume);

    // The device list: SwitchAudioSource lists one line per device and direction.
    om::Output all = om::run("SwitchAudioSource -a -f json");
    REQUIRE(all.ok());
    std::set<std::string> oracle_ids;
    for (const auto& line : om::lines(all.out)) {
        std::string type = om::json_field(line, "type"), uid = om::json_field(line, "uid");
        if (type != "output" && type != "input") continue;
        std::string id = id_of(type, uid);
        oracle_ids.insert(id);
        const AudioDevice* d = find(s, id);
        CHECK(d != nullptr);
        if (!d) {
            std::printf("  missing %s\n", id.c_str());
            continue;
        }
        CHECK_EQ(d->description, om::json_field(line, "name"));
        CHECK(d->direction == (type == "output" ? AudioDirection::Output : AudioDirection::Input));
        CHECK(d->state == AudioDeviceState::Active);
        CHECK(d->native_id == uint32_t(std::atoi(om::json_field(line, "id").c_str())));
    }
    for (auto& d : s.devices) {
        CHECK(oracle_ids.count(d.id) == 1);
        if (!oracle_ids.count(d.id)) std::printf("  not listed by SwitchAudioSource: %s\n", d.id.c_str());
    }

    // The defaults.
    std::string out_uid = om::json_field(om::run("SwitchAudioSource -c -t output -f json").out, "uid");
    std::string in_uid = om::json_field(om::run("SwitchAudioSource -c -t input -f json").out, "uid");
    CHECK_EQ(s.default_output, out_uid.empty() ? std::string() : id_of("output", out_uid));
    CHECK_EQ(s.default_input, in_uid.empty() ? std::string() : id_of("input", in_uid));
    size_t defaults = 0;
    for (auto& d : s.devices)
        if (d.is_default) {
            ++defaults;
            CHECK(d.id == s.default_output || d.id == s.default_input);
        }
    CHECK_EQ(defaults, size_t(!s.default_output.empty()) + size_t(!s.default_input.empty()));

    // Volume and mute of the defaults, the way the Sound menu shows them.
    // "output volume:19, input volume:50, alert volume:100, output muted:false"
    // (osascript says "missing value" for a device without the control).
    std::string vs = om::run("osascript -e 'get volume settings'").out;
    std::printf("osascript: %s", vs.c_str());
    auto field = [&](const std::string& key) {
        std::string v = om::after(vs, key + ":").value_or("");
        return v.substr(0, v.find(','));
    };
    if (const AudioDevice* o = find(s, s.default_output)) {
        std::string ov = field("output volume");
        if (ov != "missing value" && !ov.empty()) {
            CHECK(o->has_volume);
            CHECK(std::fabs(o->volume * 100.0f - float(std::atoi(ov.c_str()))) <= 1.0f);
        }
        std::string om_ = field("output muted");
        if (om_ == "true" || om_ == "false") CHECK_EQ(o->muted, om_ == "true");
        for (float c : o->channel_volumes) CHECK(c >= 0.0f && c <= 1.0f);
    }
    if (const AudioDevice* i = find(s, s.default_input)) {
        std::string iv = field("input volume");
        if (iv != "missing value" && !iv.empty() && i->has_volume)
            CHECK(std::fabs(i->volume * 100.0f - float(std::atoi(iv.c_str()))) <= 1.0f);
    }

    // Manufacturers, for the devices system_profiler names:
    //         MacBook Pro Speakers:
    //
    //           Manufacturer: Apple Inc.
    std::string prof = om::run("system_profiler SPAudioDataType").out;
    std::string current;
    std::map<std::string, std::string> maker;
    for (const auto& line : om::lines(prof)) {
        std::string t = om::trim(line);
        if (t.empty()) continue;
        if (t.back() == ':' && t.find(": ") == std::string::npos) current = t.substr(0, t.size() - 1);
        else if (om::starts_with(t, "Manufacturer: ") && !current.empty()) maker[current] = t.substr(14);
    }
    size_t compared = 0;
    for (auto& d : s.devices) {
        auto it = maker.find(d.description);
        if (it == maker.end()) continue;
        CHECK_EQ(d.device_name, it->second);
        ++compared;
    }
    std::printf("manufacturers compared: %zu\n", compared);
    CHECK(compared > 0 || maker.empty());
}

// Writes of the current values: accepted, and nothing changes.
void test_same_value_writes(AudioService& svc) {
    AudioState before = svc.state();
    svc.events().drain();
    for (const std::string& id : {before.default_output, before.default_input}) {
        const AudioDevice* d = find(before, id);
        if (!d) continue;
        if (d->has_volume) {
            Result r = svc.set_volume(id, d->volume);
            CHECK(r.ok);
            if (!r.ok) std::printf("set_volume(%s): %s\n", id.c_str(), r.error.c_str());
        }
        Result m = svc.set_mute(id, d->muted);
        if (!m.ok) std::printf("note: set_mute(%s, same): %s\n", id.c_str(), m.error.c_str());
        Result def = svc.set_default(id);
        CHECK(def.ok);
        if (!def.ok) std::printf("set_default(%s): %s\n", id.c_str(), def.error.c_str());
    }
    std::this_thread::sleep_for(300ms);
    AudioState after = svc.state();
    CHECK_EQ(after.default_output, before.default_output);
    CHECK_EQ(after.default_input, before.default_input);
    for (const std::string& id : {before.default_output, before.default_input}) {
        const AudioDevice *a = find(before, id), *b = find(after, id);
        if (!a || !b) continue;
        CHECK(std::fabs(a->volume - b->volume) < 0.01f);
        CHECK_EQ(a->muted, b->muted);
    }
    for (auto& e : svc.events().drain()) CHECK(!std::holds_alternative<AudioDefaultChanged>(e));

    // Unknown ids are refused.
    CHECK(!svc.set_volume("out:no-such-device", 0.5f).ok);
    CHECK(!svc.set_mute("in:no-such-device", true).ok);
    CHECK(!svc.set_default("bogus").ok);
}

// A private aggregate device (visible to this process only) over the default
// output: AudioDeviceAdded when it appears, AudioDeviceRemoved when destroyed.
void test_hotplug(AudioService& svc) {
    std::string out_id = svc.state().default_output;
    if (out_id.rfind("out:", 0) != 0) {
        std::printf("note: no default output, hot-plug check skipped\n");
        return;
    }
    std::string sub_uid = out_id.substr(4);
    std::string agg_uid = "com.bro.brosys.test." + std::to_string(getpid());
    std::string agg_id = "out:" + agg_uid;

    CFStringRef uid = CFStringCreateWithCString(nullptr, agg_uid.c_str(), kCFStringEncodingUTF8);
    CFStringRef sub = CFStringCreateWithCString(nullptr, sub_uid.c_str(), kCFStringEncodingUTF8);
    const void* sk[] = {CFSTR(kAudioSubDeviceUIDKey)};
    const void* sv[] = {sub};
    CFDictionaryRef subdev =
        CFDictionaryCreate(nullptr, sk, sv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef subs = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&subdev), 1, &kCFTypeArrayCallBacks);
    int one = 1;
    CFNumberRef yes = CFNumberCreate(nullptr, kCFNumberIntType, &one);
    const void* k[] = {CFSTR(kAudioAggregateDeviceUIDKey), CFSTR(kAudioAggregateDeviceNameKey),
                       CFSTR(kAudioAggregateDeviceIsPrivateKey), CFSTR(kAudioAggregateDeviceSubDeviceListKey)};
    const void* v[] = {uid, CFSTR("brosys test aggregate"), yes, subs};
    CFDictionaryRef desc =
        CFDictionaryCreate(nullptr, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    AudioObjectID agg = kAudioObjectUnknown;
    OSStatus st = AudioHardwareCreateAggregateDevice(desc, &agg);
    CFRelease(desc);
    CFRelease(yes);
    CFRelease(subs);
    CFRelease(subdev);
    CFRelease(sub);
    CFRelease(uid);
    if (st != noErr) {
        std::printf("note: AudioHardwareCreateAggregateDevice: %d, hot-plug check skipped\n", int(st));
        return;
    }
    svc.events().drain();
    bool added = bstest::wait_until(
        [&] {
            for (auto& e : svc.events().drain())
                if (auto* a = std::get_if<AudioDeviceAdded>(&e); a && a->device.id == agg_id) return true;
            return false;
        },
        5s);
    CHECK(added);
    const AudioState with = svc.state();
    const AudioDevice* d = find(with, agg_id);
    CHECK(d != nullptr);
    if (d) {
        CHECK_EQ(d->description, std::string("brosys test aggregate"));
        CHECK(!d->is_default);
    }
    CHECK_EQ(with.default_output, out_id);  // a private aggregate never becomes the default

    AudioHardwareDestroyAggregateDevice(agg);
    bool removed = bstest::wait_until(
        [&] {
            for (auto& e : svc.events().drain())
                if (auto* r = std::get_if<AudioDeviceRemoved>(&e); r && r->id == agg_id) return true;
            return false;
        },
        5s);
    CHECK(removed);
    CHECK(find(svc.state(), agg_id) == nullptr);
}

}  // namespace

int main() {
    if (!om::have("SwitchAudioSource")) bstest::skip("test_mac_audio", "SwitchAudioSource not installed (brew install switchaudio-osx)");
    std::string err;
    auto svc = AudioService::create({}, &err);
    if (!svc) bstest::skip("test_mac_audio", "AudioService::create: " + err);
    test_initial_events(*svc);
    test_devices(*svc);
    if (bstest::mutate_opted_in()) {
        test_same_value_writes(*svc);
        test_hotplug(*svc);
    } else {
        std::printf("note: same-value writes and the aggregate-device hot-plug not exercised; "
                    "set BROSYS_TEST_MUTATE=1\n");
    }
    svc.reset();
    for (int i = 0; i < 5; ++i) CHECK(AudioService::create({}, &err) != nullptr);
    return bstest::finish("test_mac_audio");
}
