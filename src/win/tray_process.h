// Process identity of a tray icon's owner.
#pragma once

#include "win/util.h"

#include <iterator>
#include <string>

namespace brosys::win::tray {

// Executable base name ("app.exe") of a process, "" when it cannot be queried.
inline std::string process_exe_name(DWORD pid) {
    if (!pid) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(std::size(path));
    BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!ok) return {};
    std::wstring_view full(path, size);
    size_t slash = full.find_last_of(L"\\/");
    return to_utf8(slash == std::wstring_view::npos ? full : full.substr(slash + 1));
}

}  // namespace brosys::win::tray
