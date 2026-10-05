// A real tray client for the shell-mode tests. Launched on a private desktop;
// calls the genuine Shell_NotifyIconW and reports what its windows receive.
//
// stdin, one command per line (each answered "ok <cmd> <result>"):
//   add <slot>                NIM_ADD (icon, tip "tip-<slot>", NIF_SHOWTIP, callback)
//   version <slot> <v>        NIM_SETVERSION
//   tip <slot> <text>         NIM_MODIFY NIF_TIP|NIF_SHOWTIP
//   tipnoshow <slot> <text>   NIM_MODIFY NIF_TIP (no NIF_SHOWTIP)
//   icon <slot> <aarrggbb>    NIM_MODIFY NIF_ICON with a solid icon of that colour
//   hide <slot> <0|1>         NIM_MODIFY NIF_STATE NIS_HIDDEN
//   balloon <slot> <niif-hex> <title>|<text>   NIM_MODIFY NIF_INFO (NIIF_USER: hBalloonIcon = blue)
//   clearballoon <slot>       NIM_MODIFY NIF_INFO with empty szInfo
//   del <slot>                NIM_DELETE
//   focus <slot>              NIM_SETFOCUS
//   rect <slot>               Shell_NotifyIconGetRect -> "rect <hr> <l> <t> <r> <b>"
//   find                      FindWindow(Shell_TrayWnd) -> "find <hwnd> <pid>"
//   window2                   creates the second owner window
//   killwindow2               destroys it
//   quit
// <slot> is a number (hWnd + uID identity on window 1), "g" (GUID identity on
// window 1) or "w<number>" (uID on window 2).
//
// stdout events:
//   cb <window> <wparam hex> <lparam hex>   the callback message arrived
//   taskbarcreated                           then the icons are re-added: "readded <n>"
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kCallback = WM_APP + 1;
constexpr UINT kCommand = WM_APP + 2;
// {7E5C6A55-2B21-4C1E-9D0A-3F1B5E2D9A11}
constexpr GUID kGuid = {0x7e5c6a55, 0x2b21, 0x4c1e, {0x9d, 0x0a, 0x3f, 0x1b, 0x5e, 0x2d, 0x9a, 0x11}};

UINT g_taskbar_created = 0;
HWND g_window1 = nullptr;
HWND g_window2 = nullptr;

struct Slot {
    bool window2 = false;
    bool guid = false;
    UINT uid = 0;
    HICON icon = nullptr;
    std::wstring tip;
    UINT version = 0;
    bool added = false;  // the app believes it has an icon (re-added on TaskbarCreated)
};
std::map<std::string, Slot> g_slots;

void out(const std::string& s) {
    std::fputs((s + "\n").c_str(), stdout);
    std::fflush(stdout);
}

std::string hex(unsigned long long v) {
    char b[32];
    std::snprintf(b, sizeof b, "%llx", v);
    return b;
}

// Solid size x size icon of `argb`; pixel (0,0) is fully transparent.
HICON make_icon(uint32_t argb, int size = 16) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    auto* px = static_cast<uint32_t*>(bits);
    for (int i = 0; i < size * size; ++i) px[i] = argb;
    px[0] = 0;
    std::vector<uint8_t> zero(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zero.data());
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

Slot& slot(const std::string& name) {
    auto it = g_slots.find(name);
    if (it != g_slots.end()) return it->second;
    Slot s;
    if (name == "g") {
        s.guid = true;
    } else if (!name.empty() && name[0] == 'w') {
        s.window2 = true;
        s.uid = static_cast<UINT>(std::strtoul(name.c_str() + 1, nullptr, 10));
    } else {
        s.uid = static_cast<UINT>(std::strtoul(name.c_str(), nullptr, 10));
    }
    s.icon = make_icon(0xFFFF0000);  // opaque red
    s.tip = L"tip-" + std::wstring(name.begin(), name.end());
    return g_slots.emplace(name, s).first->second;
}

NOTIFYICONDATAW base(const Slot& s) {
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof n;
    n.hWnd = s.window2 ? g_window2 : g_window1;
    n.uID = s.uid;
    if (s.guid) {
        n.uFlags |= NIF_GUID;
        n.guidItem = kGuid;
    }
    return n;
}

BOOL add(Slot& s) {
    NOTIFYICONDATAW n = base(s);
    n.uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    n.uCallbackMessage = kCallback;
    n.hIcon = s.icon;
    wcsncpy_s(n.szTip, s.tip.c_str(), _TRUNCATE);
    s.added = true;
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &n);
    if (ok && s.version) {
        n.uVersion = s.version;
        Shell_NotifyIconW(NIM_SETVERSION, &n);
    }
    return ok;
}

std::wstring widen(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

std::string run(const std::string& line) {
    std::istringstream in(line);
    std::string cmd, name;
    in >> cmd;
    if (cmd == "find") {
        HWND w = FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD pid = 0;
        if (w) GetWindowThreadProcessId(w, &pid);
        out("find " + hex(reinterpret_cast<uintptr_t>(w)) + " " + std::to_string(pid));
        return "1";
    }
    if (cmd == "window2") {
        g_window2 = CreateWindowExW(0, L"brosys_tray_client", L"owner2", WS_OVERLAPPED, 0, 0, 10, 10, nullptr,
                                    nullptr, GetModuleHandleW(nullptr), nullptr);
        return g_window2 ? "1" : "0";
    }
    if (cmd == "killwindow2") return DestroyWindow(g_window2) ? "1" : "0";
    in >> name;
    Slot& s = slot(name);
    std::string rest;
    std::getline(in, rest);
    if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    NOTIFYICONDATAW n = base(s);
    BOOL ok = FALSE;
    if (cmd == "add") {
        ok = add(s);
    } else if (cmd == "version") {
        s.version = static_cast<UINT>(std::strtoul(rest.c_str(), nullptr, 10));
        n.uVersion = s.version;
        ok = Shell_NotifyIconW(NIM_SETVERSION, &n);
    } else if (cmd == "tip" || cmd == "tipnoshow") {
        s.tip = widen(rest);
        n.uFlags |= NIF_TIP | (cmd == "tip" ? NIF_SHOWTIP : 0);
        wcsncpy_s(n.szTip, s.tip.c_str(), _TRUNCATE);
        ok = Shell_NotifyIconW(NIM_MODIFY, &n);
    } else if (cmd == "icon") {
        DestroyIcon(s.icon);
        s.icon = make_icon(static_cast<uint32_t>(std::strtoul(rest.c_str(), nullptr, 16)));
        n.uFlags |= NIF_ICON;
        n.hIcon = s.icon;
        ok = Shell_NotifyIconW(NIM_MODIFY, &n);
    } else if (cmd == "hide") {
        n.uFlags |= NIF_STATE;
        n.dwStateMask = NIS_HIDDEN;
        n.dwState = rest == "1" ? NIS_HIDDEN : 0;
        ok = Shell_NotifyIconW(NIM_MODIFY, &n);
    } else if (cmd == "balloon" || cmd == "clearballoon") {
        n.uFlags |= NIF_INFO;
        HICON balloon_icon = nullptr;
        if (cmd == "balloon") {
            std::istringstream r(rest);
            std::string flags, text;
            r >> flags;
            std::getline(r, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            n.dwInfoFlags = static_cast<DWORD>(std::strtoul(flags.c_str(), nullptr, 16));
            size_t bar = text.find('|');
            wcsncpy_s(n.szInfoTitle, widen(text.substr(0, bar)).c_str(), _TRUNCATE);
            wcsncpy_s(n.szInfo, widen(bar == std::string::npos ? "" : text.substr(bar + 1)).c_str(), _TRUNCATE);
            if ((n.dwInfoFlags & NIIF_ICON_MASK) == NIIF_USER) {
                // Opaque blue; NIIF_LARGE_ICON wants SM_CXICON-sized (32px), shell32 refuses a small one.
                balloon_icon = make_icon(0xFF0000FF, (n.dwInfoFlags & NIIF_LARGE_ICON) ? 32 : 16);
                n.hBalloonIcon = balloon_icon;
            }
        }
        ok = Shell_NotifyIconW(NIM_MODIFY, &n);
        if (balloon_icon) DestroyIcon(balloon_icon);  // the shell must have read it already
    } else if (cmd == "del") {
        s.added = false;
        ok = Shell_NotifyIconW(NIM_DELETE, &n);
    } else if (cmd == "focus") {
        ok = Shell_NotifyIconW(NIM_SETFOCUS, &n);
    } else if (cmd == "rect") {
        NOTIFYICONIDENTIFIER id{};
        id.cbSize = sizeof id;
        id.hWnd = n.hWnd;
        id.uID = n.uID;
        if (s.guid) id.guidItem = kGuid;
        RECT r{};
        HRESULT hr = Shell_NotifyIconGetRect(&id, &r);
        out("rect " + hex(static_cast<unsigned long>(hr)) + " " + std::to_string(r.left) + " " +
            std::to_string(r.top) + " " + std::to_string(r.right) + " " + std::to_string(r.bottom));
        return "1";
    } else {
        return "unknown";
    }
    return ok ? "1" : "0";
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kCallback) {
        out(std::string("cb ") + (hwnd == g_window2 ? "2" : "1") + " " + hex(wp) + " " +
            hex(static_cast<unsigned long long>(lp)));
        return 0;
    }
    if (msg == kCommand) {
        auto* line = reinterpret_cast<const std::string*>(lp);
        std::string cmd = line->substr(0, line->find(' '));
        std::string result = run(*line);
        out("ok " + cmd + " " + result);
        return 0;
    }
    if (g_taskbar_created && msg == g_taskbar_created && hwnd == g_window1) {
        out("taskbarcreated");
        int n = 0;
        for (auto& [name, s] : g_slots)
            if (s.added && add(s)) ++n;
        out("readded " + std::to_string(n));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int main() {
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"brosys_tray_client";
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only: TaskbarCreated goes to top-level windows).
    g_window1 = CreateWindowExW(0, wc.lpszClassName, L"owner1", WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!g_window1) return 2;
    ChangeWindowMessageFilterEx(g_window1, g_taskbar_created, MSGFLT_ALLOW, nullptr);
    out("ready " + std::to_string(GetCurrentProcessId()) + " " + hex(reinterpret_cast<uintptr_t>(g_window1)));

    const DWORD main_thread = GetCurrentThreadId();
    std::thread reader([main_thread] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "quit") break;
            SendMessageW(g_window1, kCommand, 0, reinterpret_cast<LPARAM>(&line));
        }
        PostThreadMessageW(main_thread, WM_QUIT, 0, 0);
    });
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    reader.join();
    return 0;
}
