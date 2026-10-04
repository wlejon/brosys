#pragma once

#include <string>

#define BRO_SYS_VERSION_MAJOR 0
#define BRO_SYS_VERSION_MINOR 1
#define BRO_SYS_VERSION_PATCH 0
#define BRO_SYS_VERSION_STRING "0.1.0"

namespace brosys {

std::string version_string();
int version_major();
int version_minor();
int version_patch();

} // namespace brosys
