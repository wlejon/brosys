#include "brosys/version.h"

namespace brosys {

std::string version_string() {
    return BRO_SYS_VERSION_STRING;
}

int version_major() {
    return BRO_SYS_VERSION_MAJOR;
}

int version_minor() {
    return BRO_SYS_VERSION_MINOR;
}

int version_patch() {
    return BRO_SYS_VERSION_PATCH;
}

} // namespace brosys
