#include "win/tray_wire.h"

#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace brosys::win::tray {

namespace {

template <size_t N>
std::wstring fixed_string(const WCHAR (&s)[N]) {
    return std::wstring(s, wcsnlen(s, N));
}

HWND to_hwnd(DWORD v) { return static_cast<HWND>(LongToHandle(static_cast<LONG>(v))); }
HICON to_hicon(DWORD v) { return static_cast<HICON>(LongToHandle(static_cast<LONG>(v))); }

bool guid_is_null(const GUID& g) {
    static const GUID zero{};
    return std::memcmp(&g, &zero, sizeof g) == 0;
}

// Shell_NotifyIconGetRect's identifier (40 bytes, observed from shell32 on
// Windows 11). The handle is carried as 32 bits like every other tray payload.
struct IconIdentifier32 {
    DWORD dwMagic;
    DWORD dwMessage;  // 1: origin, 2: size
    DWORD cbSize;     // the caller's NOTIFYICONIDENTIFIER size
    DWORD dwPadding;
    DWORD hWnd;
    UINT uID;
    GUID guidItem;
};

}  // namespace

std::optional<NotifyRequest> parse_notify(const void* data, size_t size, std::string* why) {
    constexpr size_t header = offsetof(TrayNotifyData32, nid);
    constexpr size_t minimum = header + offsetof(NotifyIconData32, szTip);
    if (!data || size < minimum) {
        if (why) *why = "tray payload too small (" + std::to_string(size) + " bytes)";
        return std::nullopt;
    }
    // Fields beyond what the sender provided read as zero.
    TrayNotifyData32 raw{};
    std::memcpy(&raw, data, size < sizeof raw ? size : sizeof raw);
    if (raw.dwSignature != kTraySignature) {
        if (why) *why = "tray payload has a bad signature";
        return std::nullopt;
    }
    const NotifyIconData32& n = raw.nid;
    NotifyRequest r;
    r.message = raw.dwMessage;
    r.hwnd = to_hwnd(n.hWnd);
    r.uid = n.uID;
    r.flags = n.uFlags;
    r.callback_message = n.uCallbackMessage;
    r.icon = to_hicon(n.hIcon);
    r.tip = fixed_string(n.szTip);
    r.state = n.dwState;
    r.state_mask = n.dwStateMask;
    r.info = fixed_string(n.szInfo);
    r.info_title = fixed_string(n.szInfoTitle);
    r.timeout_or_version = n.uTimeoutOrVersion;
    r.info_flags = n.dwInfoFlags;
    r.guid = n.guidItem;
    r.balloon_icon = to_hicon(n.hBalloonIcon);
    return r;
}

std::optional<IconRectQuery> parse_icon_rect(const void* data, size_t size, std::string* why) {
    if (!data || size < sizeof(IconIdentifier32)) {
        if (why) *why = "icon identifier too small (" + std::to_string(size) + " bytes)";
        return std::nullopt;
    }
    IconIdentifier32 raw{};
    std::memcpy(&raw, data, sizeof raw);
    if (raw.dwMagic != kTraySignature) {
        if (why) *why = "icon identifier has a bad signature";
        return std::nullopt;
    }
    IconRectQuery q;
    q.part = raw.dwMessage;
    q.hwnd = to_hwnd(raw.hWnd);
    q.uid = raw.uID;
    q.guid = raw.guidItem;
    q.by_guid = !guid_is_null(raw.guidItem);
    return q;
}

std::string hwnd_item_id(HWND hwnd, UINT uid) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "hwnd:%llx:%u",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)), uid);
    return buf;
}

std::string guid_item_id(const GUID& g) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "guid:{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
                  g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

bool post_callback(const CallbackTarget& t, UINT event, int x, int y) {
    if (!t.hwnd || !t.message) return false;
    WPARAM wp;
    LPARAM lp;
    if (t.version >= NOTIFYICON_VERSION_4) {
        wp = MAKEWPARAM(static_cast<WORD>(static_cast<int16_t>(x)), static_cast<WORD>(static_cast<int16_t>(y)));
        lp = MAKELPARAM(event, t.uid);
    } else {
        wp = t.uid;
        lp = static_cast<LPARAM>(event);
    }
    return PostMessageW(t.hwnd, t.message, wp, lp) != FALSE;
}

}  // namespace brosys::win::tray
