// wpctl / pactl as independent oracles for the audio tests: node names,
// descriptions, cubic volumes, mute and the defaults, as those tools see them.
#pragma once

#include "linux/support/proc.h"

#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>

namespace bstest::audio_cli {

struct WpVolume {
    float volume = -1;
    bool muted = false;
};

// `wpctl get-volume <target>`: "Volume: 0.40" / "Volume: 0.40 [MUTED]".
inline std::optional<WpVolume> wpctl_volume(const std::string& target, const Env& env = Env()) {
    auto r = run({"wpctl", "get-volume", target}, env);
    auto p = r.out.find("Volume:");
    if (r.exit_code != 0 || p == std::string::npos) return std::nullopt;
    WpVolume v;
    v.volume = static_cast<float>(std::atof(r.out.c_str() + p + 7));
    v.muted = r.out.find("[MUTED]") != std::string::npos;
    return v;
}

// One property out of `wpctl inspect <target>` (lines like `  * node.name = "x"`).
inline std::string wpctl_prop(const std::string& target, const std::string& key, const Env& env = Env()) {
    auto r = run({"wpctl", "inspect", target}, env);
    std::istringstream in(r.out);
    std::string line;
    while (std::getline(in, line)) {
        auto k = line.find(key + " = ");
        if (k == std::string::npos) continue;
        // Exact key: preceded by a space.
        if (k > 0 && line[k - 1] != ' ') continue;
        auto a = line.find('"', k);
        auto b = line.rfind('"');
        if (a != std::string::npos && b > a) return line.substr(a + 1, b - a - 1);
    }
    return {};
}

// `pactl get-sink-volume <name>` -> the first channel's percent.
inline std::optional<int> pactl_percent(bool sink, const std::string& name, const Env& env = Env()) {
    auto r = run({"pactl", sink ? "get-sink-volume" : "get-source-volume", name}, env);
    auto pct = r.out.find('%');
    if (r.exit_code != 0 || pct == std::string::npos) return std::nullopt;
    size_t start = pct;
    while (start > 0 && (std::isdigit(static_cast<unsigned char>(r.out[start - 1])) != 0)) --start;
    return std::atoi(r.out.substr(start, pct - start).c_str());
}

inline std::optional<bool> pactl_mute(bool sink, const std::string& name, const Env& env = Env()) {
    auto r = run({"pactl", sink ? "get-sink-mute" : "get-source-mute", name}, env);
    if (r.exit_code != 0) return std::nullopt;
    return r.out.find("yes") != std::string::npos;
}

}  // namespace bstest::audio_cli
