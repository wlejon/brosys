// Bluetooth on Windows: no backend yet. BluetoothService speaks BlueZ, which is
// Linux-only; create() says so instead of returning an empty service.
#include "brosys/bluetooth.h"

namespace brosys {

std::unique_ptr<BluetoothService> BluetoothService::create(const BluetoothConfig&, std::string* error) {
    if (error) *error = "BluetoothService is unsupported on Windows (BlueZ backend only)";
    return nullptr;
}

}  // namespace brosys
