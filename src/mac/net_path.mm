// Network.framework default-path monitoring as Connectivity.
//
// satisfied -> Full: a usable default path exists. macOS keeps its own
// captive-portal / internet probe results private, so Portal and Limited are
// never reported; a path that is not satisfied (or only satisfiable by a
// connection on demand) is None.
#import <Foundation/Foundation.h>
#import <Network/Network.h>

#include "mac/net_os.h"

namespace brosys::mac {

namespace {

struct PathWatch {
    nw_path_monitor_t monitor = nil;
    dispatch_queue_t queue = nil;
};

Connectivity connectivity_of(nw_path_t path) {
    switch (nw_path_get_status(path)) {
        case nw_path_status_satisfied: return Connectivity::Full;
        case nw_path_status_unsatisfied:
        case nw_path_status_satisfiable: return Connectivity::None;
        default: return Connectivity::Unknown;
    }
}

}  // namespace

std::shared_ptr<void> watch_path(std::function<void(Connectivity)> fn) {
    auto* w = new PathWatch;
    w->queue = dispatch_queue_create("brosys.network.path", DISPATCH_QUEUE_SERIAL);
    w->monitor = nw_path_monitor_create();
    dispatch_semaphore_t first = dispatch_semaphore_create(0);
    __block bool signalled = false;
    auto callback = std::make_shared<std::function<void(Connectivity)>>(std::move(fn));
    nw_path_monitor_set_queue(w->monitor, w->queue);
    nw_path_monitor_set_update_handler(w->monitor, ^(nw_path_t path) {
      (*callback)(connectivity_of(path));
      if (!signalled) {
          signalled = true;
          dispatch_semaphore_signal(first);
      }
    });
    nw_path_monitor_start(w->monitor);
    dispatch_semaphore_wait(first, dispatch_time(DISPATCH_TIME_NOW, 2 * int64_t(NSEC_PER_SEC)));
    return std::shared_ptr<void>(w, [](void* p) {
        auto* pw = static_cast<PathWatch*>(p);
        nw_path_monitor_cancel(pw->monitor);
        dispatch_sync(pw->queue, ^{});  // an update in flight finishes first
        delete pw;
    });
}

}  // namespace brosys::mac
