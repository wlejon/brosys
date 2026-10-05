// A private PipeWire instance for one test: its own XDG_RUNTIME_DIR,
// config / state dirs and D-Bus (a PrivateBus), pipewire with null audio
// sinks / sources from context.objects, WirePlumber with the "policy"
// profile (default nodes, metadata, no hardware monitors) and, when
// installed, pipewire-pulse so pactl can act as a second client. The
// user's own session is never touched.
#pragma once

#include "linux/support/private_bus.h"

#include <memory>
#include <string>
#include <vector>

namespace bstest {

struct NullNode {
    std::string name;
    std::string description;
    bool sink = true;
    int priority = 1000;  // priority.session: WirePlumber's default pick
};

class PrivatePipeWire {
public:
    explicit PrivatePipeWire(const std::vector<NullNode>& nodes);
    ~PrivatePipeWire();
    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    bool has_pulse() const { return pulse_; }

    const std::string& socket() const { return socket_; }  // absolute path of pipewire-0
    Env env() const;  // for wpctl / pactl / pw-cli against this instance

    void kill_pipewire();

private:
    std::unique_ptr<TempDir> dir_;
    std::unique_ptr<PrivateBus> bus_;
    std::string runtime_, socket_;
    Daemon pipewire_, wireplumber_, pulse_daemon_;
    bool pulse_ = false;
    std::string error_;
};

}  // namespace bstest
