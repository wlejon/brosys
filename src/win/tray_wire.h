// The Shell_NotifyIcon wire protocol: what shell32 sends to the window of
// class Shell_TrayWnd with WM_COPYDATA, and how the shell answers the icon.
//
// dwData 1: tray data. A signature, the NIM_* message, and a NOTIFYICONDATAW
//           whose handles are carried as 32-bit values so 32- and 64-bit
//           clients share one layout (HWND / HICON are session-wide and
//           32-bit significant).
// dwData 3: Shell_NotifyIconGetRect. Identifies an icon (hWnd + uID or GUID)
//           and asks for its rectangle in two calls: part 2 (the size) first,
//           then part 1 (the origin), each answered as MAKELONG(x, y); 0 fails.
// dwData 0: SHAppBarMessage.
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

namespace brosys::win::tray {

inline constexpr ULONG_PTR kCopyDataAppBar = 0;
inline constexpr ULONG_PTR kCopyDataTray = 1;
inline constexpr ULONG_PTR kCopyDataIconRect = 3;
inline constexpr DWORD kTraySignature = 0x34753423;

// NOTIFYICONDATAW with 32-bit handle fields (every member is 4-aligned, so
// the natural layout is the wire layout).
struct NotifyIconData32 {
    DWORD cbSize;
    DWORD hWnd;
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    DWORD hIcon;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    UINT uTimeoutOrVersion;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    DWORD hBalloonIcon;
};
static_assert(sizeof(NotifyIconData32) == 956, "NOTIFYICONDATA32 wire layout");

struct TrayNotifyData32 {
    DWORD dwSignature;
    DWORD dwMessage;
    NotifyIconData32 nid;
};

// One decoded Shell_NotifyIcon call.
struct NotifyRequest {
    DWORD message = 0;  // NIM_ADD, NIM_MODIFY, ...
    HWND hwnd = nullptr;
    UINT uid = 0;
    UINT flags = 0;     // NIF_*
    UINT callback_message = 0;
    HICON icon = nullptr;
    std::wstring tip;
    DWORD state = 0;
    DWORD state_mask = 0;
    std::wstring info;
    std::wstring info_title;
    UINT timeout_or_version = 0;
    DWORD info_flags = 0;
    GUID guid{};
    HICON balloon_icon = nullptr;
};

// Decodes a dwData 1 payload. nullopt (with `why`) for a malformed one.
std::optional<NotifyRequest> parse_notify(const void* data, size_t size, std::string* why);

// A Shell_NotifyIconGetRect query (dwData 3).
struct IconRectQuery {
    DWORD part = 0;  // 1: origin (left, top), 2: size (width, height)
    HWND hwnd = nullptr;
    UINT uid = 0;
    GUID guid{};
    bool by_guid = false;  // guid is non-zero
};
std::optional<IconRectQuery> parse_icon_rect(const void* data, size_t size, std::string* why);

// Stable identities of an icon: "hwnd:<hex>:<uid>" and "guid:{...}".
std::string hwnd_item_id(HWND hwnd, UINT uid);
std::string guid_item_id(const GUID& guid);

// Where an icon wants its interaction delivered.
struct CallbackTarget {
    HWND hwnd = nullptr;
    UINT uid = 0;
    UINT message = 0;  // uCallbackMessage (0: the icon asked for none)
    UINT version = 0;  // NIM_SETVERSION (0, 3 or 4)
    DWORD pid = 0;
};

// Posts `event` (WM_LBUTTONDOWN, NIN_SELECT, NIN_BALLOONUSERCLICK, ...) to the
// icon in the encoding its version expects:
//   version 4:   wParam = MAKEWPARAM(x, y), lParam = MAKELPARAM(event, uID)
//   older:       wParam = uID,              lParam = event
bool post_callback(const CallbackTarget& target, UINT event, int x, int y);

}  // namespace brosys::win::tray
