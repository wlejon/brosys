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
#if defined(_WIN32)
    auto bt = BluetoothService::create({}, &err);
    CHECK(bt != nullptr);
    CHECK(err.empty());
    std::printf("BluetoothService::create (Windows): ok (adapters=%zu, devices=%zu)\n",
                bt ? bt->adapters().size() : 0, bt ? bt->devices().size() : 0);
    CHECK(BluetoothService::create({}, nullptr) != nullptr);
#else
    auto bt = BluetoothService::create({}, &err);
    CHECK(bt == nullptr);
    CHECK(!err.empty());
    std::printf("BluetoothService::create: %s\n", err.c_str());
    CHECK(BluetoothService::create({}, nullptr) == nullptr);
#endif

    err.clear();
    auto ss = ScreenSaverServer::create({}, &err);
    CHECK(ss == nullptr);
    CHECK(!err.empty());
    std::printf("ScreenSaverServer::create: %s\n", err.c_str());

    return bstest::finish("test_linux_only_services");
}
