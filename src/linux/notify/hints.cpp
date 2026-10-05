#include "linux/notify/hints.h"

#include <cstdint>

namespace brosys::notify {

std::optional<Image> image_from_struct(const dbus::Value& raw) {
    const dbus::Value& v = raw.unwrap();
    if (v.type() != '(') return std::nullopt;
    auto& f = v.items();
    if (f.size() != 7) return std::nullopt;
    auto w = f[0].to_int(), h = f[1].to_int(), stride = f[2].to_int();
    auto alpha = f[3].to_bool();
    auto bps = f[4].to_int(), ch = f[5].to_int();
    const auto* data = f[6].as_bytes();
    if (!w || !h || !stride || !alpha || !bps || !ch || !data) return std::nullopt;
    if (*w <= 0 || *h <= 0 || *w > 16384 || *h > 16384) return std::nullopt;
    if (*bps != 8) return std::nullopt;
    if (*ch != 3 && *ch != 4) return std::nullopt;
    if (*alpha && *ch != 4) return std::nullopt;
    const size_t width = static_cast<size_t>(*w), height = static_cast<size_t>(*h);
    const size_t channels = static_cast<size_t>(*ch);
    const size_t row_bytes = width * channels;
    if (*stride < 0 || static_cast<size_t>(*stride) < row_bytes) return std::nullopt;
    const size_t rowstride = static_cast<size_t>(*stride);
    // GdkPixbuf does not pad the last row.
    const size_t need = rowstride * (height - 1) + row_bytes;
    if (data->size() < need) return std::nullopt;

    Image img;
    img.width = static_cast<int32_t>(width);
    img.height = static_cast<int32_t>(height);
    img.rgba.resize(width * height * 4);
    uint8_t* out = img.rgba.data();
    for (size_t y = 0; y < height; ++y) {
        const uint8_t* row = data->data() + y * rowstride;
        for (size_t x = 0; x < width; ++x, out += 4) {
            const uint8_t* px = row + x * channels;
            out[0] = px[0];
            out[1] = px[1];
            out[2] = px[2];
            // 4 channels without has_alpha: the fourth byte is padding.
            out[3] = (*alpha && channels == 4) ? px[3] : 255;
        }
    }
    return img;
}

namespace {

bool is_string(const dbus::Value& v) { return v.type() == 's' || v.type() == 'o'; }

}  // namespace

void apply_hints(const dbus::Value& hints, Notification& n) {
    const dbus::Value* best_image[3] = {nullptr, nullptr, nullptr};  // image-data, image_data, icon_data
    std::string path_dash, path_underscore;
    bool have_path_dash = false, have_path_underscore = false;

    auto keep = [&](const std::string& key, const dbus::Value& value) {
        n.other_hints.emplace_back(key, value.to_text());
    };

    for (auto& entry : hints.unwrap().items()) {
        auto& kv = entry.items();
        if (kv.size() != 2) continue;
        std::string key = kv[0].as_string();
        const dbus::Value& v = kv[1].unwrap();
        if (key == "urgency") {
            auto u = v.to_uint();
            if (!u || *u > 2) {
                keep(key, v);
                continue;
            }
            n.urgency = static_cast<Urgency>(*u);
        } else if (key == "category" && is_string(v)) {
            n.category = v.as_string();
        } else if (key == "desktop-entry" && is_string(v)) {
            n.desktop_entry = v.as_string();
        } else if (key == "image-data" || key == "image_data" || key == "icon_data") {
            int slot = key == "image-data" ? 0 : key == "image_data" ? 1 : 2;
            best_image[slot] = &v;
        } else if (key == "image-path" && is_string(v)) {
            path_dash = v.as_string();
            have_path_dash = true;
        } else if (key == "image_path" && is_string(v)) {
            path_underscore = v.as_string();
            have_path_underscore = true;
        } else if (key == "sound-file" && is_string(v)) {
            n.sound_file = v.as_string();
        } else if (key == "sound-name" && is_string(v)) {
            n.sound_name = v.as_string();
        } else if (key == "suppress-sound" && v.to_bool()) {
            n.suppress_sound = *v.to_bool();
        } else if (key == "transient" && v.to_bool()) {
            n.transient = *v.to_bool();
        } else if (key == "resident" && v.to_bool()) {
            n.resident = *v.to_bool();
        } else if (key == "action-icons" && v.to_bool()) {
            n.action_icons = *v.to_bool();
        } else if ((key == "x" || key == "y") && v.to_int()) {
            int64_t c = *v.to_int();
            if (c < INT32_MIN || c > INT32_MAX) {
                keep(key, v);
                continue;
            }
            (key == "x" ? n.x : n.y) = static_cast<int32_t>(c);
        } else {
            keep(key, v);
        }
    }

    n.image.reset();
    static const char* const kImageKeys[3] = {"image-data", "image_data", "icon_data"};
    for (int i = 0; i < 3; ++i) {
        if (!best_image[i]) continue;
        if (!n.image) {
            if (auto img = image_from_struct(*best_image[i])) {
                n.image = std::move(*img);
                continue;
            }
        }
        // Malformed, or shadowed by a higher-precedence image: keep it visible.
        n.other_hints.emplace_back(kImageKeys[i], best_image[i]->to_text());
    }
    if (have_path_dash)
        n.image_path = path_dash;
    else if (have_path_underscore)
        n.image_path = path_underscore;
}

}  // namespace brosys::notify
