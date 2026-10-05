#include "linux/support/private_bus.h"

#include <unistd.h>

namespace bstest {

PrivateBus::PrivateBus() : dir_(std::make_unique<TempDir>("brosys-bus")) {
    if (dir_->path().empty()) {
        error_ = "cannot create a temporary directory";
        return;
    }
    if (!have_program("dbus-daemon")) {
        error_ = "dbus-daemon not installed";
        return;
    }
    std::string socket = dir_->path() + "/bus";
    std::string config = dir_->path() + "/bus.conf";
    std::string xml =
        "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
        " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
        "<busconfig>\n"
        "  <type>session</type>\n"
        "  <listen>unix:path=" + socket + "</listen>\n"
        "  <auth>EXTERNAL</auth>\n"
        "  <policy context=\"default\">\n"
        "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
        "    <allow eavesdrop=\"true\"/>\n"
        "    <allow own=\"*\"/>\n"
        "  </policy>\n"
        "</busconfig>\n";
    if (!write_file(config, xml)) {
        error_ = "cannot write " + config;
        return;
    }
    config_ = config;
    std::string line;
    if (!launch(&line)) {
        error_ = "dbus-daemon did not start (got '" + line + "')";
        return;
    }
    guid_address_ = line;
    address_ = line.substr(0, line.find(",guid="));
}

bool PrivateBus::launch(std::string* address) {
    daemon_ = Daemon({"dbus-daemon", "--config-file=" + config_, "--nofork", "--nopidfile", "--print-address=1"});
    std::string line;
    if (!daemon_.read_line(line, std::chrono::milliseconds(10000)) || line.find("unix:") != 0) {
        if (address) *address = line;
        daemon_.stop();
        return false;
    }
    if (address) *address = line;
    return true;
}

bool PrivateBus::restart() {
    if (config_.empty()) return false;
    daemon_.stop();
    ::unlink((dir_->path() + "/bus").c_str());  // a killed daemon leaves its socket behind
    std::string line;
    if (!launch(&line)) return false;
    return line.substr(0, line.find(",guid=")) == address_;
}

PrivateBus::~PrivateBus() { daemon_.stop(); }

Env PrivateBus::env() const {
    return Env{{"DBUS_SESSION_BUS_ADDRESS", address_}, {"DBUS_SYSTEM_BUS_ADDRESS", address_}};
}

void PrivateBus::kill() { daemon_.stop(); }

}  // namespace bstest
