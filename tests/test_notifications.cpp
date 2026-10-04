#include "brosys/notifications.h"
#include "test_common.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    init_test();
    std::cout << "[test_notifications] Testing notification server...\n";

    brosys::NotificationServer server;
    assert(!server.is_running());

    bool started = server.start();
    assert(started);
    assert(server.is_running());

    // Check server info & capabilities
    auto info = server.get_server_info();
    assert(info.name == "brosys");
    assert(!info.version.empty());

    auto caps = server.get_capabilities();
    assert(!caps.empty());

    // Setup callbacks
    std::atomic<bool> received_fired = false;
    std::atomic<bool> action_fired = false;
    std::atomic<bool> closed_fired = false;
    uint32_t last_closed_id = 0;
    brosys::CloseReason last_close_reason = brosys::CloseReason::Undefined;

    server.set_notification_received_callback([&](const brosys::NotificationItem& item) {
        if (item.summary == "Download Completed") {
            received_fired.store(true);
        }
    });

    server.set_action_invoked_callback([&](uint32_t /*id*/, const std::string& key) {
        if (key == "open_folder") {
            action_fired.store(true);
        }
    });

    server.set_notification_closed_callback([&](uint32_t id, brosys::CloseReason reason) {
        last_closed_id = id;
        last_close_reason = reason;
        closed_fired.store(true);
    });

    // 1. Post notification
    brosys::NotificationItem item1;
    item1.app_name = "Bro Browser";
    item1.summary = "Download Completed";
    item1.body = "bro-setup.exe has finished downloading.";
    item1.icon_name = "document-save";
    item1.urgency = brosys::NotificationUrgency::Normal;
    item1.actions.push_back(brosys::NotificationAction{"open_folder", "Open Folder"});
    item1.actions.push_back(brosys::NotificationAction{"dismiss", "Dismiss"});
    item1.timeout_ms = 0; // Does not expire automatically

    uint32_t id1 = server.post_notification(item1);
    assert(id1 > 0);
    assert(received_fired.load());
    assert(server.active_count() == 1);

    auto notif = server.get_notification(id1);
    assert(notif.has_value());
    assert(notif->summary == "Download Completed");
    assert(notif->actions.size() == 2);

    // 2. Invoke action
    assert(server.invoke_action(id1, "open_folder"));
    assert(action_fired.load());

    // 3. Close notification
    assert(server.close_notification(id1, brosys::CloseReason::DismissedByUser));
    assert(closed_fired.load());
    assert(last_closed_id == id1);
    assert(last_close_reason == brosys::CloseReason::DismissedByUser);
    assert(server.active_count() == 0);

    // 4. Test auto-expiration with timeout_ms
    closed_fired.store(false);
    brosys::NotificationItem item2;
    item2.app_name = "System";
    item2.summary = "Battery Low";
    item2.body = "Plug in your charger soon.";
    item2.timeout_ms = 150; // Expire after 150ms

    uint32_t id2 = server.post_notification(item2);
    assert(id2 > 0);
    assert(server.active_count() == 1);

    // Wait for expiration thread to reap it (polls every 250ms)
    for (int i = 0; i < 30; ++i) {
        if (closed_fired.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(closed_fired.load());
    assert(last_closed_id == id2);
    assert(last_close_reason == brosys::CloseReason::Expired);
    assert(server.active_count() == 0);

    server.stop();
    assert(!server.is_running());

    std::cout << "[test_notifications] PASSED\n";
    return 0;
}
