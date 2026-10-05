// Built without PipeWire (BROSYS_WITH_PIPEWIRE=OFF): audio is unavailable,
// said so plainly instead of pretending.
#include "brosys/audio.h"

namespace brosys {

std::unique_ptr<AudioService> AudioService::create(const AudioConfig&, std::string* error) {
    if (error) *error = "brosys was built without PipeWire support (BROSYS_WITH_PIPEWIRE=OFF)";
    return nullptr;
}

}  // namespace brosys
