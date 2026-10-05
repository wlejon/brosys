// CoreAudio HAL access for the macOS audio service: reading devices into
// AudioDevice values and the three mutations. No state of its own.
//
// A HAL device may have output streams, input streams or both; brosys
// models each direction as its own AudioDevice whose id is the device UID
// prefixed with the direction: "out:<uid>" / "in:<uid>".
#pragma once

#include "brosys/audio.h"

#include <CoreAudio/CoreAudio.h>

#include <string>
#include <vector>

namespace brosys::mac {

struct HalDevice {
    AudioObjectID object = kAudioObjectUnknown;
    AudioDevice device;
};

std::string audio_id(const std::string& uid, AudioDirection direction);

// Every visible (not hidden) device, once per direction it has streams for.
std::vector<HalDevice> read_hal_devices();
std::vector<AudioObjectID> hal_device_objects();

// kAudioObjectUnknown when there is none.
AudioObjectID default_device(AudioDirection direction);
std::string device_uid(AudioObjectID object);

Result hal_set_volume(AudioObjectID object, AudioDirection direction, float volume);
Result hal_set_mute(AudioObjectID object, AudioDirection direction, bool muted);
Result hal_set_default(AudioObjectID object, AudioDirection direction);

// Device properties whose change can alter an AudioDevice (listened to per device).
std::vector<AudioObjectPropertyAddress> watched_device_properties();

}  // namespace brosys::mac
