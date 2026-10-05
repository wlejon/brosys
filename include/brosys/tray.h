// Tray: the status-notifier area. Other processes publish items; the host
// renders them (icons, tooltips, menus) and sends interaction back.
//
// Linux: StatusNotifierWatcher (org.kde.StatusNotifierWatcher, and
// org.freedesktop.StatusNotifierWatcher when that name is free: one
// registry served under both) + a StatusNotifierHost. Items implement
// org.kde.StatusNotifierItem or org.freedesktop.StatusNotifierItem; their
// properties are tracked through their New* signals; menus
// (com.canonical.dbusmenu) are fetched as data the host renders, and
// clicks are sent back as dbusmenu Events. When another process already
// owns the watcher name, this host registers with it instead (role
// WatcherClient) and still sees every item. When the session bus daemon
// restarts, the host reconnects and takes up its role again; items
// re-register themselves.
// Windows: real tray hosting needs the process to be the shell: it must own
// the Shell_TrayWnd window that Shell_NotifyIcon talks to (WM_COPYDATA). In
// shell mode this host creates it on its own thread's desktop and receives
// every NIM_* call; alongside Explorer that window belongs to Explorer and
// nothing can be hosted (role None, reason in capabilities()).
// macOS: role None in every mode. Menu-bar extras are NSStatusItems each
// application draws into the menu bar itself; no process can host another's.
// TrayMode::Shell fails.
#pragma once

#include "brosys/common.h"
#include "brosys/event_queue.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace brosys {

enum class TrayItemStatus { Passive, Active, NeedsAttention };
enum class TrayCategory { ApplicationStatus, Communications, SystemServices, Hardware };

struct TrayIcon {
    std::string name;            // icon-theme name ("" when only pixmaps)
    std::string theme_path;      // extra theme search path (SNI IconThemePath)
    std::vector<Image> pixmaps;  // one or more sizes (Windows: the HICON's pixels)

    bool empty() const { return name.empty() && pixmaps.empty(); }
    bool operator==(const TrayIcon&) const = default;
};

struct TrayToolTip {
    TrayIcon icon;
    std::string title;
    std::string body;  // may contain markup on Linux
    bool operator==(const TrayToolTip&) const = default;
};

enum class MenuToggle { None, Checkmark, Radio };

// One com.canonical.dbusmenu node (Linux). id 0 is the root.
struct MenuItem {
    int32_t id = 0;
    bool separator = false;
    std::string label;  // mnemonic underscores kept ("_Quit"); "__" is a literal underscore
    bool enabled = true;
    bool visible = true;
    std::string icon_name;
    std::vector<uint8_t> icon_png;  // dbusmenu icon-data: PNG bytes, undecoded
    MenuToggle toggle = MenuToggle::None;
    int32_t toggle_state = -1;  // 0 off, 1 on, -1 indeterminate
    std::vector<std::vector<std::string>> shortcut;  // [["Control", "q"]]
    bool has_submenu = false;   // children-display == "submenu"
    std::vector<MenuItem> children;

    bool operator==(const MenuItem&) const = default;
};

struct TrayItem {
    std::string id;       // unique in this host: "<bus name><object path>" (Linux), "hwnd:<hex>:<uid>" or "guid:{...}" (Windows)
    std::string app_id;   // SNI Id; Windows: executable base name
    std::string title;
    TrayCategory category = TrayCategory::ApplicationStatus;
    TrayItemStatus status = TrayItemStatus::Active;
    TrayIcon icon;
    TrayIcon overlay_icon;
    TrayIcon attention_icon;
    std::string attention_movie;
    TrayToolTip tooltip;  // Windows: title = szTip
    bool item_is_menu = false;  // activation should show the menu
    bool has_menu = false;      // Linux: dbusmenu present (menu() returns it)
    uint64_t window_id = 0;     // SNI WindowId; Windows: the icon's HWND
    uint32_t pid = 0;
    bool hidden = false;        // Windows NIS_HIDDEN

    bool operator==(const TrayItem&) const = default;
};

// Bits for TrayItemChanged::changes.
namespace tray_change {
inline constexpr uint32_t Title = 1u << 0;
inline constexpr uint32_t Icon = 1u << 1;
inline constexpr uint32_t AttentionIcon = 1u << 2;
inline constexpr uint32_t OverlayIcon = 1u << 3;
inline constexpr uint32_t ToolTip = 1u << 4;
inline constexpr uint32_t Status = 1u << 5;
inline constexpr uint32_t Menu = 1u << 6;  // the menu object itself (not its layout)
inline constexpr uint32_t Other = 1u << 7;
}  // namespace tray_change

struct TrayItemAdded {
    TrayItem item;
};
struct TrayItemChanged {
    TrayItem item;
    uint32_t changes = 0;
};
struct TrayItemRemoved {
    std::string id;
};
// A registration that never became an item (no TrayItemAdded was sent for
// it): its object never answered, or its owner left before it did.
struct TrayItemDropped {
    std::string id;
    std::string reason;
};
// The item's menu layout or item properties changed; `root` is the new full menu.
struct TrayMenuChanged {
    std::string item_id;
    MenuItem root;
};

enum class TrayRole {
    None,           // hosting nothing (see capabilities().detail)
    Watcher,        // Linux: owns org.kde.StatusNotifierWatcher (and is a host of it)
    WatcherClient,  // Linux: another process owns the watcher; registered as a host with it
    Shell,          // Windows: owns Shell_TrayWnd on this desktop
};

struct TrayHostStatus {
    TrayRole role = TrayRole::None;
    std::string detail;
};

using TrayEvent =
    std::variant<TrayItemAdded, TrayItemChanged, TrayItemRemoved, TrayMenuChanged, TrayHostStatus, TrayItemDropped>;
using TrayEventQueue = MessageQueue<TrayEvent>;

enum class TrayMode {
    Auto,       // Windows: Shell if no Shell_TrayWnd exists on this desktop, else Alongside
    Shell,      // Windows: create Shell_TrayWnd; create() fails if another exists
    Alongside,  // Windows: never claim the tray (role None)
};

struct TrayConfig {
    // Linux
    std::string session_bus_address;   // override (tests use a private bus); empty = default
    bool become_watcher = true;        // false: only register as a host with an existing watcher
    // Windows
    TrayMode mode = TrayMode::Auto;
    // Shell mode: tell running apps to re-add their icons (RegisterWindowMessage
    // "TaskbarCreated"), sent to the top-level windows of this desktop.
    bool announce_taskbar_created = true;
};

struct Rect32 {
    int32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Rect32&) const = default;
};

enum class ScrollOrientation { Vertical, Horizontal };
enum class MenuEventType { Clicked, Hovered, Opened, Closed };

// The rest of a dbusmenu Event call: `data` (the spec leaves its meaning to
// the event; most items ignore it, and an int32 0 is what most hosts send)
// and the timestamp (0: the time of the call).
struct MenuEventData {
    std::variant<int32_t, std::string, bool> data = int32_t(0);
    uint32_t timestamp = 0;
};

enum class HoverPhase { Enter, Move, Leave };

class TrayHost {
public:
    static std::unique_ptr<TrayHost> create(const TrayConfig& config, std::string* error);
    virtual ~TrayHost() = default;

    virtual TrayEventQueue& events() = 0;
    virtual TrayHostStatus status() const = 0;
    virtual std::vector<TrayItem> items() const = 0;

    // Interaction. (x, y) are screen coordinates of the pointer / icon.
    virtual Result activate(const std::string& item_id, int32_t x, int32_t y) = 0;            // primary click
    virtual Result secondary_activate(const std::string& item_id, int32_t x, int32_t y) = 0;  // middle click
    virtual Result context_menu(const std::string& item_id, int32_t x, int32_t y) = 0;        // the item shows its own menu
    virtual Result scroll(const std::string& item_id, int32_t delta, ScrollOrientation orientation) = 0;
    // Double click. Windows: down, up, (NIN_SELECT,) WM_LBUTTONDBLCLK, up.
    // Linux: StatusNotifierItem has no double click (failure).
    virtual Result double_click(const std::string& item_id, int32_t x, int32_t y) = 0;
    // The item chosen from the keyboard (Enter / Space on a focused icon).
    // Windows: NIN_KEYSELECT (version 3+ icons; older ones get the right
    // button pair, as Explorer sends). Linux: Activate.
    virtual Result keyboard_select(const std::string& item_id, int32_t x, int32_t y) = 0;
    // The pointer over the item. Windows: WM_MOUSEMOVE, plus NIN_POPUPOPEN on
    // Enter / NIN_POPUPCLOSE on Leave for version 4 icons (which draw their
    // own rich tooltip). Linux: StatusNotifierItem has no hover (failure).
    virtual Result hover(const std::string& item_id, int32_t x, int32_t y, HoverPhase phase) = 0;

    // Linux dbusmenu: the cached layout, and events sent to it.
    virtual std::optional<MenuItem> menu(const std::string& item_id) const = 0;
    virtual Result menu_about_to_show(const std::string& item_id, int32_t menu_item_id) = 0;
    virtual Result menu_event(const std::string& item_id, int32_t menu_item_id, MenuEventType type,
                              const MenuEventData& data = MenuEventData()) = 0;

    // Windows: where the host drew the icon (answers Shell_NotifyIconGetRect).
    virtual Result set_item_rect(const std::string& item_id, const Rect32& rect) = 0;
};

const char* to_string(TrayRole r);
const char* to_string(TrayItemStatus s);

}  // namespace brosys
