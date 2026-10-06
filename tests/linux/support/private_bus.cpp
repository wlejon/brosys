#include "linux/support/private_bus.h"

namespace bstest {

PrivateBus::PrivateBus() {
    if (!bus_.ok()) {
        error_ = "dbus-daemon failed to start";
        return;
    }
    update_addresses();
}

void PrivateBus::update_addresses() {
    const auto& full = bus_.address();
    auto pos = full.find(",guid=");
    address_ = (pos != std::string::npos) ? full.substr(0, pos) : full;
}

Env PrivateBus::env() const {
    return Env{{"DBUS_SESSION_BUS_ADDRESS", address_}, {"DBUS_SYSTEM_BUS_ADDRESS", address_}};
}

void PrivateBus::kill() {
    bus_.stop();
}

bool PrivateBus::restart() {
    bool ok = bus_.restart();
    if (ok) {
        update_addresses();
    }
    return ok;
}

}  // namespace bstest
