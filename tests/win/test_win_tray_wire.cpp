// The pure pieces of the Windows shell: the Shell_NotifyIcon wire layout,
// item ids, callback encodings, HICON -> RGBA, and the balloon mapping.
#include "check.h"
#include "win/notifications_balloon.h"
#include "win/tray_icon.h"
#include "win/tray_wire.h"

#include <shellapi.h>

#include <cstring>

using namespace brosys;
using namespace brosys::win;

namespace {

constexpr const char* kName = "test_win_tray_wire";

void test_parse() {
    tray::TrayNotifyData32 raw{};
    raw.dwSignature = tray::kTraySignature;
    raw.dwMessage = NIM_MODIFY;
    raw.nid.cbSize = sizeof raw.nid;
    raw.nid.hWnd = 0x1234;
    raw.nid.uID = 7;
    raw.nid.uFlags = NIF_TIP | NIF_INFO;
    wcscpy_s(raw.nid.szTip, L"tip");
    wcscpy_s(raw.nid.szInfo, L"body");
    wcscpy_s(raw.nid.szInfoTitle, L"title");
    raw.nid.dwInfoFlags = NIIF_WARNING;
    raw.nid.hBalloonIcon = 0xFFFF8000;  // sign-extended like any 32-bit handle
    auto r = tray::parse_notify(&raw, sizeof raw, nullptr);
    REQUIRE(r.has_value());
    CHECK_EQ(r->message, DWORD(NIM_MODIFY));
    CHECK(r->hwnd == reinterpret_cast<HWND>(uintptr_t(0x1234)));
    CHECK_EQ(r->uid, 7u);
    CHECK(r->tip == L"tip" && r->info == L"body" && r->info_title == L"title");
    CHECK_EQ(r->info_flags, DWORD(NIIF_WARNING));
    CHECK(reinterpret_cast<intptr_t>(r->balloon_icon) == -0x8000);

    // A truncated payload reads the missing tail as zero.
    auto shorter = tray::parse_notify(&raw, 8 + offsetof(tray::NotifyIconData32, szInfo), nullptr);
    REQUIRE(shorter.has_value());
    CHECK(shorter->info.empty());
    CHECK(shorter->tip == L"tip");

    std::string why;
    raw.dwSignature = 1;
    CHECK(!tray::parse_notify(&raw, sizeof raw, &why).has_value());
    CHECK(!why.empty());
    CHECK(!tray::parse_notify(&raw, 10, &why).has_value());

    CHECK_EQ(tray::hwnd_item_id(reinterpret_cast<HWND>(uintptr_t(0xabc)), 3), std::string("hwnd:abc:3"));
    const GUID g = {0x7e5c6a55, 0x2b21, 0x4c1e, {0x9d, 0x0a, 0x3f, 0x1b, 0x5e, 0x2d, 0x9a, 0x11}};
    CHECK_EQ(tray::guid_item_id(g), std::string("guid:{7E5C6A55-2B21-4C1E-9D0A-3F1B5E2D9A11}"));
}

void test_callback_encoding() {
    HWND w = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    REQUIRE(w != nullptr);
    tray::CallbackTarget t{w, 9, WM_APP + 5, NOTIFYICON_VERSION_4, 0};
    CHECK(tray::post_callback(t, WM_CONTEXTMENU, -3, 40));
    MSG m{};
    CHECK(PeekMessageW(&m, w, WM_APP + 5, WM_APP + 5, PM_REMOVE));
    CHECK_EQ(LOWORD(m.lParam), WORD(WM_CONTEXTMENU));
    CHECK_EQ(HIWORD(m.lParam), WORD(9));
    CHECK_EQ(int(static_cast<int16_t>(LOWORD(m.wParam))), -3);
    CHECK_EQ(int(HIWORD(m.wParam)), 40);
    t.version = 0;
    CHECK(tray::post_callback(t, WM_LBUTTONUP, 1, 1));
    CHECK(PeekMessageW(&m, w, WM_APP + 5, WM_APP + 5, PM_REMOVE));
    CHECK_EQ(m.wParam, WPARAM(9));
    CHECK_EQ(m.lParam, LPARAM(WM_LBUTTONUP));
    t.message = 0;
    CHECK(!tray::post_callback(t, WM_LBUTTONUP, 0, 0));
    DestroyWindow(w);
}

HICON icon_from(const uint32_t* argb, const uint8_t* mask_rows, bool with_color) {
    HBITMAP color = nullptr;
    if (with_color) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = 2;
        bi.bmiHeader.biHeight = -2;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        std::memcpy(bits, argb, 16);
    }
    // Monochrome rows are WORD-aligned: one byte of bits + one pad byte each.
    HBITMAP mask = CreateBitmap(2, with_color ? 2 : 4, 1, 1, mask_rows);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    if (color) DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void test_icons() {
    CHECK(!tray::icon_to_image(nullptr).has_value());

    // 32bpp with alpha: straight alpha kept, BGRA -> RGBA.
    const uint32_t alpha_px[4] = {0x80102030, 0xFF405060, 0x00000000, 0xFFFFFFFF};
    const uint8_t no_mask[4] = {0, 0, 0, 0};
    HICON a = icon_from(alpha_px, no_mask, true);
    auto img = tray::icon_to_image(a);
    REQUIRE(img.has_value());
    CHECK(img->width == 2 && img->height == 2);
    CHECK(img->rgba[0] == 0x10 && img->rgba[1] == 0x20 && img->rgba[2] == 0x30 && img->rgba[3] == 0x80);
    CHECK(img->rgba[11] == 0);
    DestroyIcon(a);

    // No alpha channel: the AND mask decides (top-left pixel masked out).
    const uint32_t opaque_px[4] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF};
    const uint8_t mask[4] = {0x80, 0, 0, 0};
    HICON b = icon_from(opaque_px, mask, true);
    img = tray::icon_to_image(b);
    REQUIRE(img.has_value());
    CHECK(img->rgba[3] == 0 && img->rgba[7] == 255);
    CHECK(img->rgba[4] == 0 && img->rgba[5] == 255 && img->rgba[6] == 0);
    DestroyIcon(b);

    // Monochrome: AND rows then XOR rows.
    const uint8_t mono[8] = {0x80, 0, 0x00, 0, 0x40, 0, 0x00, 0};
    HICON c = icon_from(nullptr, mono, false);
    img = tray::icon_to_image(c);
    REQUIRE(img.has_value());
    CHECK(img->width == 2 && img->height == 2);
    CHECK(img->rgba[3] == 0);                         // (0,0) transparent
    CHECK(img->rgba[4] == 255 && img->rgba[7] == 255);  // (1,0) white, opaque
    CHECK(img->rgba[8] == 0 && img->rgba[11] == 255);   // (0,1) black, opaque
    DestroyIcon(c);
}

void test_balloon_mapping() {
    tray::BalloonData b;
    b.item_id = "hwnd:1:2";
    b.app_id = "app.exe";
    b.pid = 42;
    b.title = L"T";
    b.text = L"x & <y>";
    b.info_flags = NIIF_ERROR | NIIF_NOSOUND | NIIF_RESPECT_QUIET_TIME;
    Notification n = notify::make_balloon_notification(b, true);
    CHECK_EQ(n.body, std::string("x &amp; &lt;y&gt;"));
    CHECK_EQ(notify::make_balloon_notification(b, false).body, std::string("x & <y>"));
    CHECK_EQ(n.app_icon, std::string("dialog-error"));
    CHECK_EQ(n.sender, std::string("hwnd:1:2"));
    CHECK_EQ(n.sender_pid, 42u);
    CHECK(n.suppress_sound);
    bool quiet = false;
    for (auto& [k, v] : n.other_hints) quiet |= k == "x-windows-respect-quiet-time" && v == "true";
    CHECK(quiet);
    CHECK_EQ(notify::balloon_close_message(CloseReason::Expired), UINT(NIN_BALLOONTIMEOUT));
    CHECK_EQ(notify::balloon_close_message(CloseReason::Dismissed), UINT(NIN_BALLOONTIMEOUT));
    CHECK_EQ(notify::balloon_close_message(CloseReason::Closed), UINT(NIN_BALLOONHIDE));
    CHECK_EQ(notify::balloon_close_message(CloseReason::Undefined), UINT(NIN_BALLOONHIDE));
}

}  // namespace

int main() {
    test_parse();
    test_callback_encoding();
    test_icons();
    test_balloon_mapping();
    return bstest::finish(kName);
}
