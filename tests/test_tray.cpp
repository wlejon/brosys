#include "brosys/tray.h"
#include "test_common.h"

#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_tray] Testing status notifier / tray host...\n";

    brosys::TrayHost host;
    assert(!host.is_running());

    bool started = host.start();
    assert(started);
    assert(host.is_running());
    assert(host.item_count() == 0);

    // Callbacks
    bool added_fired = false;
    bool updated_fired = false;
    bool removed_fired = false;
    bool activated_fired = false;
    bool menu_action_fired = false;
    int triggered_action_id = -1;

    host.set_item_added_callback([&](const brosys::StatusNotifierItem& item) {
        if (item.id == "discord") added_fired = true;
    });

    host.set_item_updated_callback([&](const brosys::StatusNotifierItem& item) {
        if (item.id == "discord" && item.status == brosys::ItemStatus::NeedsAttention) {
            updated_fired = true;
        }
    });

    host.set_item_removed_callback([&](const std::string& id) {
        if (id == "discord") removed_fired = true;
    });

    host.set_item_activated_callback([&](const std::string& id, int x, int y) {
        if (id == "discord" && x == 100 && y == 200) {
            activated_fired = true;
        }
    });

    host.set_menu_action_callback([&](const std::string& id, int action_id) {
        if (id == "discord") {
            menu_action_fired = true;
            triggered_action_id = action_id;
        }
    });

    // 1. Register item
    brosys::StatusNotifierItem item;
    item.id = "discord";
    item.title = "Discord";
    item.service_name = "com.discord.Discord";
    item.category = brosys::ItemCategory::Communications;
    item.status = brosys::ItemStatus::Active;
    item.icon_name = "discord-tray";
    item.tooltip_title = "Discord";
    item.tooltip_body = "Connected - #general";

    brosys::TrayMenuItem m_mute;
    m_mute.id = 1;
    m_mute.label = "Mute";
    m_mute.is_check = true;
    m_mute.checked = false;

    brosys::TrayMenuItem m_quit;
    m_quit.id = 2;
    m_quit.label = "Quit Discord";

    item.menu_items.push_back(m_mute);
    item.menu_items.push_back(m_quit);

    assert(host.register_item(item));
    assert(added_fired);
    assert(host.item_count() == 1);

    auto retrieved = host.get_item("discord");
    assert(retrieved.has_value());
    assert(retrieved->title == "Discord");
    assert(retrieved->menu_items.size() == 2);

    // 2. Update item status (e.g. mention received -> NeedsAttention)
    item.status = brosys::ItemStatus::NeedsAttention;
    item.attention_icon_name = "discord-tray-unread";
    assert(host.update_item(item));
    assert(updated_fired);

    auto updated = host.get_item("discord");
    assert(updated.has_value());
    assert(updated->status == brosys::ItemStatus::NeedsAttention);

    // 3. Activate item
    assert(host.activate_item("discord", 100, 200));
    assert(activated_fired);

    assert(host.secondary_activate_item("discord", 100, 200));
    assert(host.context_menu("discord", 100, 200));

    // 4. Trigger menu action
    assert(host.trigger_menu_action("discord", 2));
    assert(menu_action_fired);
    assert(triggered_action_id == 2);

    // 5. Unregister item
    assert(host.unregister_item("discord"));
    assert(removed_fired);
    assert(host.item_count() == 0);
    assert(!host.get_item("discord").has_value());

    host.stop();
    assert(!host.is_running());

    std::cout << "[test_tray] PASSED\n";
    return 0;
}
