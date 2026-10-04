#pragma once

#include "brosys/export.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brosys {

enum class ItemStatus {
    Passive = 0,
    Active = 1,
    NeedsAttention = 2
};

enum class ItemCategory {
    ApplicationStatus = 0,
    Communications = 1,
    SystemServices = 2,
    Hardware = 3,
    Other = 4
};

struct TrayIconPixmap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool operator==(const TrayIconPixmap& other) const = default;
};

struct TrayMenuItem {
    int id = 0;
    std::string label;
    bool enabled = true;
    bool visible = true;
    bool is_separator = false;
    bool is_check = false;
    bool checked = false;
    std::string icon_name;
    std::vector<TrayMenuItem> children;

    bool operator==(const TrayMenuItem& other) const = default;
};

struct StatusNotifierItem {
    std::string id;
    std::string service_name;
    std::string title;
    ItemCategory category = ItemCategory::ApplicationStatus;
    ItemStatus status = ItemStatus::Active;
    std::string icon_name;
    std::vector<TrayIconPixmap> icon_pixmaps;
    std::string attention_icon_name;
    std::string tooltip_title;
    std::string tooltip_body;
    std::string menu_path;
    std::vector<TrayMenuItem> menu_items;

    bool operator==(const StatusNotifierItem& other) const = default;
};

class BROSYS_API TrayHost {
public:
    using ItemAddedCallback = std::function<void(const StatusNotifierItem&)>;
    using ItemUpdatedCallback = std::function<void(const StatusNotifierItem&)>;
    using ItemRemovedCallback = std::function<void(const std::string& id)>;
    using ItemActivatedCallback = std::function<void(const std::string& id, int x, int y)>;
    using MenuActionCallback = std::function<void(const std::string& id, int action_id)>;

    TrayHost();
    ~TrayHost();

    TrayHost(const TrayHost&) = delete;
    TrayHost& operator=(const TrayHost&) = delete;
    TrayHost(TrayHost&&) noexcept;
    TrayHost& operator=(TrayHost&&) noexcept;

    bool start();
    void stop();
    [[nodiscard]] bool is_running() const;

    [[nodiscard]] std::vector<StatusNotifierItem> get_items() const;
    [[nodiscard]] std::optional<StatusNotifierItem> get_item(const std::string& id) const;
    [[nodiscard]] size_t item_count() const;

    bool register_item(const StatusNotifierItem& item);
    bool update_item(const StatusNotifierItem& item);
    bool unregister_item(const std::string& id);
    void clear();

    bool activate_item(const std::string& id, int x = 0, int y = 0);
    bool secondary_activate_item(const std::string& id, int x = 0, int y = 0);
    bool context_menu(const std::string& id, int x = 0, int y = 0);
    bool trigger_menu_action(const std::string& id, int action_id);

    void set_item_added_callback(ItemAddedCallback cb);
    void set_item_updated_callback(ItemUpdatedCallback cb);
    void set_item_removed_callback(ItemRemovedCallback cb);
    void set_item_activated_callback(ItemActivatedCallback cb);
    void set_menu_action_callback(MenuActionCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace brosys
