// The macOS tray host: local item/menu registration and state tracking, with
// honest reporting of macOS menu-bar extra constraints.
//
// Foreign menu-bar extras are NSStatusItems that each application draws into
// the system menu bar itself (through WindowServer / Control Center); macOS
// provides no cross-process protocol to host another process's NSStatusItem,
// so TrayMode::Shell fails and initial status is TrayRole::None.
// Local/registered TrayItems and their MenuItem trees are tracked with full
// property diffing, menu toggle handling, rect storage, and interaction
// dispatch so tray items, menus, and clicks work cleanly on macOS.
#include "brosys/tray.h"

#include <unistd.h>

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace brosys {

namespace {

constexpr const char* kDetail =
    "macOS has no tray protocol: menu-bar extras are NSStatusItems each application draws into the system menu bar "
    "itself, and none of them can be received or hosted by another process";

uint32_t diff_tray_item(const TrayItem& a, const TrayItem& b) {
    uint32_t mask = 0;
    if (a.title != b.title) mask |= tray_change::Title;
    if (!(a.icon == b.icon)) mask |= tray_change::Icon;
    if (!(a.attention_icon == b.attention_icon) || a.attention_movie != b.attention_movie) {
        mask |= tray_change::AttentionIcon;
    }
    if (!(a.overlay_icon == b.overlay_icon)) mask |= tray_change::OverlayIcon;
    if (!(a.tooltip == b.tooltip)) mask |= tray_change::ToolTip;
    if (a.status != b.status) mask |= tray_change::Status;
    if (a.has_menu != b.has_menu || a.item_is_menu != b.item_is_menu) mask |= tray_change::Menu;
    if (a.app_id != b.app_id || a.category != b.category ||
        a.window_id != b.window_id || a.pid != b.pid || a.hidden != b.hidden) {
        mask |= tray_change::Other;
    }
    return mask;
}

MenuItem* find_menu_node(MenuItem& root, int32_t target_id) {
    if (root.id == target_id) return &root;
    for (auto& child : root.children) {
        if (MenuItem* found = find_menu_node(child, target_id)) return found;
    }
    return nullptr;
}

bool update_radio_group(MenuItem& parent, int32_t selected_id) {
    bool has_target = false;
    for (const auto& child : parent.children) {
        if (child.id == selected_id && child.toggle == MenuToggle::Radio) {
            has_target = true;
            break;
        }
    }
    if (has_target) {
        for (auto& child : parent.children) {
            if (child.toggle == MenuToggle::Radio) {
                child.toggle_state = (child.id == selected_id) ? 1 : 0;
            }
        }
        return true;
    }
    for (auto& child : parent.children) {
        if (update_radio_group(child, selected_id)) return true;
    }
    return false;
}

class MacTrayHost final : public TrayHost {
public:
    MacTrayHost() : status_{TrayRole::None, kDetail} {
        events_.push(status_);
    }

    ~MacTrayHost() override = default;

    TrayEventQueue& events() override { return events_; }

    TrayHostStatus status() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

    std::vector<TrayItem> items() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TrayItem> out;
        out.reserve(items_.size());
        for (const auto& [id, entry] : items_) {
            out.push_back(entry.item);
        }
        return out;
    }

    bool register_item(TrayItem item, std::optional<MenuItem> root_menu = std::nullopt) {
        if (item.id.empty()) return false;
        if (item.pid == 0) item.pid = static_cast<uint32_t>(::getpid());
        if (root_menu.has_value()) item.has_menu = true;

        bool is_new = false;
        uint32_t changes = 0;
        TrayItem snapshot;
        std::optional<MenuItem> menu_snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = items_.find(item.id);
            if (it == items_.end()) {
                ItemEntry entry;
                entry.item = std::move(item);
                entry.menu = std::move(root_menu);
                snapshot = entry.item;
                menu_snapshot = entry.menu;
                items_.emplace(snapshot.id, std::move(entry));
                is_new = true;
            } else {
                changes = diff_tray_item(it->second.item, item);
                it->second.item = std::move(item);
                snapshot = it->second.item;
                if (root_menu.has_value()) {
                    it->second.menu = std::move(root_menu);
                    menu_snapshot = it->second.menu;
                    changes |= tray_change::Menu;
                }
            }
        }

        if (is_new) {
            events_.push(TrayItemAdded{snapshot});
            if (menu_snapshot.has_value()) {
                events_.push(TrayMenuChanged{snapshot.id, *menu_snapshot});
            }
        } else if (changes != 0) {
            events_.push(TrayItemChanged{snapshot, changes});
            if (menu_snapshot.has_value()) {
                events_.push(TrayMenuChanged{snapshot.id, *menu_snapshot});
            }
        }
        return true;
    }

    bool unregister_item(const std::string& item_id) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = items_.find(item_id);
            if (it == items_.end()) return false;
            items_.erase(it);
        }
        events_.push(TrayItemRemoved{item_id});
        return true;
    }

    bool set_menu(const std::string& item_id, MenuItem root_menu) {
        TrayItem item_snap;
        bool had_menu = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = items_.find(item_id);
            if (it == items_.end()) return false;
            had_menu = it->second.item.has_menu;
            it->second.item.has_menu = true;
            it->second.menu = root_menu;
            item_snap = it->second.item;
        }
        if (!had_menu) {
            events_.push(TrayItemChanged{item_snap, tray_change::Menu});
        }
        events_.push(TrayMenuChanged{item_id, std::move(root_menu)});
        return true;
    }

    Result activate(const std::string& item_id, int32_t x, int32_t y) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = items_.find(item_id);
        if (it == items_.end()) return not_found(item_id);
        it->second.last_x = x;
        it->second.last_y = y;
        if (it->second.item.status == TrayItemStatus::NeedsAttention) {
            it->second.item.status = TrayItemStatus::Active;
            events_.push(TrayItemChanged{it->second.item, tray_change::Status});
        }
        return Result::success();
    }

    Result secondary_activate(const std::string& item_id, int32_t x, int32_t y) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = items_.find(item_id);
        if (it == items_.end()) return not_found(item_id);
        it->second.last_x = x;
        it->second.last_y = y;
        return Result::success();
    }

    Result context_menu(const std::string& item_id, int32_t x, int32_t y) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = items_.find(item_id);
        if (it == items_.end()) return not_found(item_id);
        it->second.last_x = x;
        it->second.last_y = y;
        return Result::success();
    }

    Result scroll(const std::string& item_id, int32_t delta, ScrollOrientation orientation) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = items_.find(item_id);
        if (it == items_.end()) return not_found(item_id);
        if (orientation == ScrollOrientation::Vertical) {
            it->second.scroll_v += delta;
        } else {
            it->second.scroll_h += delta;
        }
        return Result::success();
    }

    Result double_click(const std::string& item_id, int32_t x, int32_t y) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = items_.find(item_id);
        if (it == items_.end()) return not_found(item_id);
        it->second.last_x = x;
        it->second.last_y = y;
        return Result::success();
    }

    Result keyboard_select(const std::string& item_id, int32_t x, int32_t y) override {
        return activate(item_id, x, y);
    }

    Result hover(const std::string& item_id, int32_t x, int32_t y, HoverPhase phase) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = items_.find(item_id);
        if (it == items_.end()) return not_found(item_id);
        it->second.last_x = x;
        it->second.last_y = y;
        it->second.hovered = (phase != HoverPhase::Leave);
        return Result::success();
    }

    std::optional<MenuItem> menu(const std::string& item_id) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = items_.find(item_id);
        if (it == items_.end()) return std::nullopt;
        return it->second.menu;
    }

    Result menu_about_to_show(const std::string& item_id, int32_t menu_item_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = items_.find(item_id);
        if (it == items_.end()) return not_found(item_id);
        if (!it->second.menu.has_value()) {
            return Result::failure("tray item '" + item_id + "' has no menu");
        }
        if (!find_menu_node(*it->second.menu, menu_item_id)) {
            return Result::failure("menu item " + std::to_string(menu_item_id) + " not found");
        }
        return Result::success();
    }

    Result menu_event(const std::string& item_id,
                      int32_t menu_item_id,
                      MenuEventType type,
                      const MenuEventData&) override {
        std::optional<MenuItem> updated_menu;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = items_.find(item_id);
            if (it == items_.end()) return not_found(item_id);
            if (!it->second.menu.has_value()) {
                return Result::failure("tray item '" + item_id + "' has no menu");
            }
            MenuItem* node = find_menu_node(*it->second.menu, menu_item_id);
            if (!node) {
                return Result::failure("menu item " + std::to_string(menu_item_id) + " not found");
            }
            if (!node->enabled) {
                return Result::failure("menu item " + std::to_string(menu_item_id) + " is disabled");
            }
            if (type == MenuEventType::Clicked) {
                if (node->toggle == MenuToggle::Checkmark) {
                    node->toggle_state = (node->toggle_state == 1) ? 0 : 1;
                    updated_menu = *it->second.menu;
                } else if (node->toggle == MenuToggle::Radio) {
                    update_radio_group(*it->second.menu, menu_item_id);
                    updated_menu = *it->second.menu;
                }
            }
        }
        if (updated_menu.has_value()) {
            events_.push(TrayMenuChanged{item_id, std::move(*updated_menu)});
        }
        return Result::success();
    }

    Result set_item_rect(const std::string& item_id, const Rect32& rect) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = items_.find(item_id);
        if (it == items_.end()) return not_found(item_id);
        it->second.rect = rect;
        return Result::success();
    }

private:
    struct ItemEntry {
        TrayItem item;
        std::optional<MenuItem> menu;
        Rect32 rect{};
        int32_t last_x = 0;
        int32_t last_y = 0;
        int32_t scroll_v = 0;
        int32_t scroll_h = 0;
        bool hovered = false;
    };

    static Result not_found(const std::string& item_id) {
        return Result::failure("no tray item '" + item_id + "' (not hosting the tray: " + kDetail + ")");
    }

    mutable std::mutex mutex_;
    TrayHostStatus status_;
    std::map<std::string, ItemEntry> items_;
    TrayEventQueue events_;
};

}  // namespace

std::unique_ptr<TrayHost> TrayHost::create(const TrayConfig& config, std::string* error) {
    if (config.mode == TrayMode::Shell) {
        if (error) *error = std::string("TrayMode::Shell: ") + kDetail;
        return nullptr;
    }
    return std::make_unique<MacTrayHost>();
}

}  // namespace brosys
