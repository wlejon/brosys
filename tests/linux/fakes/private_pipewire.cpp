#include "linux/fakes/private_pipewire.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <thread>
#include <vector>

namespace bstest {

namespace {

bool exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

bool wait_path(const std::string& path, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!exists(path)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

// WirePlumber 0.5 runs a named profile ("policy": the session policy, no hardware
// monitors). 0.4 (Ubuntu 24.04, Debian 12) has no profiles and rejects the option; its
// default configuration is the whole session manager, which on a private PipeWire with
// only null nodes does the same job. `wireplumber --version` prints
// "Linked with libwireplumber X.Y.Z".
std::vector<std::string> wireplumber_argv(const Env& env) {
    auto r = run({"wireplumber", "--version"}, env, std::chrono::milliseconds(5000));
    const std::string key = "libwireplumber ";
    size_t at = r.out.rfind(key);
    int major = 0, minor = 0;
    if (at != std::string::npos && std::sscanf(r.out.c_str() + at + key.size(), "%d.%d", &major, &minor) == 2 &&
        major == 0 && minor < 5)
        return {"wireplumber"};
    return {"wireplumber", "--profile", "policy"};
}

std::string pipewire_conf(const std::vector<NullNode>& nodes) {
    std::string objects;
    for (auto& n : nodes) {
        objects += "    { factory = adapter\n"
                   "      args = {\n"
                   "        factory.name = support.null-audio-sink\n"
                   "        node.name = " + quote(n.name) + "\n"
                   "        node.description = " + quote(n.description) + "\n"
                   "        media.class = " + std::string(n.sink ? "Audio/Sink" : "Audio/Source/Virtual") + "\n"
                   "        audio.position = [ FL FR ]\n"
                   "        priority.session = " + std::to_string(n.priority) + "\n"
                   "        priority.driver = " + std::to_string(n.priority) + "\n"
                   "        monitor.channel-volumes = true\n"
                   "        object.linger = true\n"
                   "      }\n"
                   "    }\n";
    }
    return "context.properties = {\n"
           "    core.daemon = true\n"
           "    core.name = pipewire-0\n"
           "    module.x11.bell = false\n"
           "}\n"
           "context.spa-libs = {\n"
           "    audio.convert.* = audioconvert/libspa-audioconvert\n"
           "    support.*       = support/libspa-support\n"
           "}\n"
           "context.modules = [\n"
           "    { name = libpipewire-module-protocol-native }\n"
           "    { name = libpipewire-module-metadata }\n"
           "    { name = libpipewire-module-spa-device-factory }\n"
           "    { name = libpipewire-module-spa-node-factory }\n"
           "    { name = libpipewire-module-client-node }\n"
           "    { name = libpipewire-module-client-device }\n"
           "    { name = libpipewire-module-access }\n"
           "    { name = libpipewire-module-adapter }\n"
           "    { name = libpipewire-module-link-factory }\n"
           "    { name = libpipewire-module-session-manager flags = [ ifexists nofail ] }\n"
           "]\n"
           "context.objects = [\n"
           "    { factory = spa-node-factory\n"
           "      args = { factory.name = support.node.driver node.name = Dummy-Driver priority.driver = 20000 }\n"
           "    }\n" +
           objects + "]\n";
}

}  // namespace

PrivatePipeWire::PrivatePipeWire(const std::vector<NullNode>& nodes) {
    for (const char* p : {"pipewire", "wireplumber", "wpctl"})
        if (!have_program(p)) {
            error_ = std::string(p) + " not installed";
            return;
        }
    dir_ = std::make_unique<TempDir>("brosys-pw");
    bus_ = std::make_unique<PrivateBus>();
    if (!bus_->ok()) {
        error_ = bus_->error();
        return;
    }
    runtime_ = dir_->path() + "/run";
    for (const char* sub : {"/run", "/config", "/state", "/data"})
        mkdir((dir_->path() + sub).c_str(), 0700);
    socket_ = runtime_ + "/pipewire-0";
    conf_ = dir_->path() + "/brosys-pipewire.conf";
    nodes_ = nodes;
    if (!write_file(conf_, pipewire_conf(nodes))) {
        error_ = "cannot write " + conf_;
        return;
    }
    error_ = launch();
}

bool PrivatePipeWire::restart_pipewire() {
    kill_pipewire();
    ::unlink(socket_.c_str());  // a killed server can leave its socket behind
    ::unlink((socket_ + ".lock").c_str());
    error_ = launch();
    return error_.empty();
}

std::string PrivatePipeWire::launch() {
    const auto& nodes = nodes_;
    pipewire_ = Daemon({"pipewire", "-c", conf_}, env(), false);
    if (!wait_path(socket_, std::chrono::milliseconds(10000))) return "pipewire did not create " + socket_;
    wireplumber_ = Daemon(wireplumber_argv(env()), env(), false);
    // Ready once WirePlumber has chosen a default sink.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(15000);
    while (true) {
        auto r = run({"wpctl", "inspect", "@DEFAULT_AUDIO_SINK@"}, env(), std::chrono::milliseconds(5000));
        if (r.exit_code == 0 && r.out.find("node.name") != std::string::npos) break;
        if (std::chrono::steady_clock::now() >= deadline) return "WirePlumber did not pick a default sink";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // An adapter node ignores Props (volume / mute) until it has run once:
    // PipeWire applies them to its converter, which exists only after a
    // format was negotiated (wpctl set-volume is equally ignored before).
    // Run each node briefly so the tests exercise the volume path.
    if (have_program("pw-cat")) {
        for (auto& n : nodes) {
            std::vector<std::string> argv{"pw-cat", n.sink ? "-p" : "-r", "--raw", "--format", "s16", "--rate", "48000",
                                          "--channels", "2", "--target", n.name, n.sink ? "/dev/zero" : "/dev/null"};
            run(argv, env(), std::chrono::milliseconds(700));  // killed after 0.7 s by design
        }
    }
    if (have_program("pipewire-pulse") && have_program("pactl")) {
        ::unlink((runtime_ + "/pulse/native").c_str());
        pulse_daemon_ = Daemon({"pipewire-pulse"}, env(), false);
        pulse_ = wait_path(runtime_ + "/pulse/native", std::chrono::milliseconds(10000));
    }
    return {};
}

PrivatePipeWire::~PrivatePipeWire() {
    pulse_daemon_.stop();
    wireplumber_.stop();
    pipewire_.stop();
}

Env PrivatePipeWire::env() const {
    Env e;
    if (bus_) e = bus_->env();
    e["XDG_RUNTIME_DIR"] = runtime_;
    e["PIPEWIRE_RUNTIME_DIR"] = runtime_;
    e["PIPEWIRE_REMOTE"] = "";
    e["PIPEWIRE_CONFIG_DIR"] = "";
    e["XDG_CONFIG_HOME"] = dir_ ? dir_->path() + "/config" : "";
    e["XDG_STATE_HOME"] = dir_ ? dir_->path() + "/state" : "";
    e["XDG_DATA_HOME"] = dir_ ? dir_->path() + "/data" : "";
    e["PULSE_SERVER"] = "unix:" + runtime_ + "/pulse/native";
    e["PULSE_RUNTIME_PATH"] = runtime_ + "/pulse";
    e["DISPLAY"] = "";
    e["WAYLAND_DISPLAY"] = "";
    return e;
}

void PrivatePipeWire::kill_pipewire() {
    pipewire_.stop();  // the server goes first: clients see their connection drop
    pulse_daemon_.stop();
    wireplumber_.stop();
}

}  // namespace bstest
