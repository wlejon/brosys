// A second brosys TrayHost in its own process, for the WatcherClient tests:
// it owns org.kde.StatusNotifierWatcher (when free) and prints what it sees.
//
//   ROLE <role> <detail>   ADDED <id>   CHANGED <id> <bits>   REMOVED <id>   MENU <id>
//   DROPPED <id> <reason>
//
// usage: brosys_tray_host --bus ADDRESS [--no-watcher]
#include "brosys/tray.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

void say(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}
}  // namespace

int main(int argc, char** argv) {
    brosys::TrayConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bus") == 0 && i + 1 < argc) cfg.session_bus_address = argv[++i];
        else if (std::strcmp(argv[i], "--no-watcher") == 0) cfg.become_watcher = false;
    }
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    std::string err;
    auto host = brosys::TrayHost::create(cfg, &err);
    if (!host) {
        std::fprintf(stderr, "tray_host: %s\n", err.c_str());
        return 2;
    }
    while (!g_stop) {
        host->events().wait_for(std::chrono::milliseconds(100));
        for (auto& ev : host->events().drain()) {
            std::visit(
                [](auto&& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, brosys::TrayHostStatus>)
                        say(std::string("ROLE ") + brosys::to_string(e.role) + " " + e.detail);
                    else if constexpr (std::is_same_v<T, brosys::TrayItemAdded>)
                        say("ADDED " + e.item.id);
                    else if constexpr (std::is_same_v<T, brosys::TrayItemChanged>)
                        say("CHANGED " + e.item.id + " " + std::to_string(e.changes));
                    else if constexpr (std::is_same_v<T, brosys::TrayItemRemoved>)
                        say("REMOVED " + e.id);
                    else if constexpr (std::is_same_v<T, brosys::TrayMenuChanged>)
                        say("MENU " + e.item_id);
                    else if constexpr (std::is_same_v<T, brosys::TrayItemDropped>)
                        say("DROPPED " + e.id + " " + e.reason);
                },
                ev);
        }
    }
    host.reset();
    return 0;
}
