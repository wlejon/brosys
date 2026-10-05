// Small value types shared by every service.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace brosys {

// Outcome of a host command. `error` is a human-readable reason when !ok
// (on Linux it carries the D-Bus error name and message when one exists).
struct Result {
    bool ok = false;
    std::string error;

    explicit operator bool() const { return ok; }
    static Result success() { return Result{true, {}}; }
    static Result failure(std::string why) { return Result{false, std::move(why)}; }
};

// Straight (non-premultiplied) RGBA8, row-major, tightly packed:
// rgba.size() == width * height * 4.
struct Image {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> rgba;

    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
    bool operator==(const Image&) const = default;
};

// Whether an action is available to this process right now.
enum class Availability {
    Unknown,    // the backend could not find out
    No,         // not supported or forbidden by policy
    Yes,        // allowed without further authorization
    NeedsAuth,  // allowed after interactive authorization (polkit "challenge")
};

const char* to_string(Availability a);

}  // namespace brosys
