// Desktop Notifications hints (a{sv}) -> Notification fields.
#pragma once

#include "brosys/notifications.h"
#include "linux/dbus/value.h"

#include <optional>

namespace brosys::notify {

// Converts the spec's raw image struct (iiibiiay: width, height, rowstride,
// has_alpha, bits_per_sample, channels, data) to straight tightly packed
// RGBA8. nullopt when the struct is malformed (wrong shape, bits per sample
// other than 8, channels other than 3/4, rowstride or data too short).
std::optional<Image> image_from_struct(const dbus::Value& v);

// Fills the hint-derived fields of `n` from a hints dictionary. Image
// precedence is image-data > image_data > icon_data (first valid one);
// image-path > image_path. Hints of an unexpected type and every unknown
// hint land in other_hints as GVariant-like text.
void apply_hints(const dbus::Value& hints, Notification& n);

}  // namespace brosys::notify
