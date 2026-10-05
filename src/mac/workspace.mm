// NSWorkspaceWillPowerOffNotification: the only shutdown / restart / logout
// announcement macOS gives a user process. AppKit delivers it to GUI
// applications (a process with an NSApplication connected to the window
// server); in a plain command-line process the observer never fires.
#import <AppKit/AppKit.h>

#include "mac/power_mac.h"

namespace brosys::mac {

namespace {

struct PowerOffObserver {
    id token = nil;
    NSOperationQueue* queue = nil;
};

}  // namespace

std::shared_ptr<void> observe_power_off(std::function<void()> fn) {
    auto holder = new PowerOffObserver;
    holder->queue = [[NSOperationQueue alloc] init];
    holder->queue.maxConcurrentOperationCount = 1;
    auto callback = std::make_shared<std::function<void()>>(std::move(fn));
    holder->token = [NSWorkspace.sharedWorkspace.notificationCenter
        addObserverForName:NSWorkspaceWillPowerOffNotification
                    object:nil
                     queue:holder->queue
                usingBlock:^(NSNotification*) {
                  (*callback)();
                }];
    return std::shared_ptr<void>(holder, [](void* p) {
        auto* h = static_cast<PowerOffObserver*>(p);
        [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:h->token];
        [h->queue waitUntilAllOperationsAreFinished];
        delete h;
    });
}

}  // namespace brosys::mac
