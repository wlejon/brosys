// Services whose only backend is Linux (BlueZ, the exported
// org.freedesktop.ScreenSaver) must say so on Windows and macOS: create()
// returns nullptr with a reason, never an empty service that looks working.
// Built on Windows and macOS only; Linux drives the real ones in
// test_bluetooth and test_screensaver.
#include "check.h"
#include "brosys/bluetooth.h"
#include "brosys/power.h"

#include <cstdio>

using namespace brosys;

int main() {
    std::string err;
    auto bt = BluetoothService::create({}, &err);
    CHECK(bt == nullptr);
    CHECK(!err.empty());
    std::printf("BluetoothService::create: %s\n", err.c_str());

    err.clear();
    auto ss = ScreenSaverServer::create({}, &err);
    CHECK(ss == nullptr);
    CHECK(!err.empty());
    std::printf("ScreenSaverServer::create: %s\n", err.c_str());

    // Null error pointers are allowed.
    CHECK(BluetoothService::create({}, nullptr) == nullptr);
    return bstest::finish("test_linux_only_services");
}
