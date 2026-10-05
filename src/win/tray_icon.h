// HICON -> straight RGBA. Icons are USER objects shared across the session,
// so the shell reads another process's icon directly while that process is
// blocked in Shell_NotifyIcon, and keeps only the pixels.
#pragma once

#include "brosys/common.h"

#include <windows.h>

#include <optional>

namespace brosys::win::tray {

// nullopt for a null or invalid handle. Colour icons keep their alpha
// channel (icon alpha is straight); icons without alpha take it from the
// AND mask; monochrome icons become black/white with mask transparency.
std::optional<Image> icon_to_image(HICON icon);

}  // namespace brosys::win::tray
