// macOS network service. One serial dispatch queue receives SCDynamicStore
// notifications (State:/Network and Setup:/Network/Service keys), CoreWLAN
// events and Network.framework path updates, and re-snapshots there
// (coalesced); NetworkChanged is pushed only when the snapshot changed.
// Wi-Fi scans run on their own queue (CoreWLAN scans block for seconds);
// access_points() answers from the system's cached scan results.
#include "brosys/network.h"

#include "mac/cf.h"
#include "mac/net_snapshot.h"

#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>

#include <atomic>
#include <mutex>

namespace brosys {

namespace {

class MacNetworkService final : public NetworkService {
public:
    explicit MacNetworkService(const NetworkConfig& cfg)
        : config_(cfg),
          queue_(dispatch_queue_create("brosys.network", DISPATCH_QUEUE_SERIAL)),
          scan_queue_(dispatch_queue_create("brosys.network.scan", DISPATCH_QUEUE_SERIAL)) {}

    ~MacNetworkService() override {
        stopping_ = true;
        alive_->store(false);  // delayed blocks (refresh coalescing, scan timeouts) fire after this into nothing
        path_.reset();
        wifi_watch_.reset();
        if (store_) SCDynamicStoreSetDispatchQueue(store_.get(), nullptr);
        dispatch_sync(scan_queue_, ^{});  // a scan in progress completes (CoreWLAN cannot cancel one)
        dispatch_sync(queue_, ^{});
        dispatch_release(scan_queue_);
        dispatch_release(queue_);
    }

    bool start(std::string* error) {
        path_ = mac::watch_path([this](Connectivity c) {
            connectivity_ = c;
            schedule_refresh();
        });
        SCDynamicStoreContext ctx{0, this, nullptr, nullptr, nullptr};
        store_ = mac::CFRef<SCDynamicStoreRef>(
            SCDynamicStoreCreate(nullptr, CFSTR("brosys.network"), &MacNetworkService::store_cb, &ctx));
        if (!store_) {
            if (error) *error = "SCDynamicStoreCreate failed";
            return false;
        }
        std::vector<mac::CFRef<CFStringRef>> pats;
        std::vector<const void*> raw;
        for (const auto& p : mac::network_watch_patterns()) {
            pats.push_back(mac::make_string(p));
            raw.push_back(pats.back().get());
        }
        mac::CFRef<CFArrayRef> patterns(CFArrayCreate(nullptr, raw.data(), CFIndex(raw.size()), &kCFTypeArrayCallBacks));
        if (!SCDynamicStoreSetNotificationKeys(store_.get(), nullptr, patterns.get()) ||
            !SCDynamicStoreSetDispatchQueue(store_.get(), queue_)) {
            if (error) *error = std::string("SCDynamicStore notifications: ") + SCErrorString(SCError());
            return false;
        }
        wifi_watch_ = mac::watch_wifi([this] { schedule_refresh(); });
        dispatch_sync(queue_, ^{ refresh(); });  // the first NetworkChanged is queued before create() returns
        return true;
    }

    NetworkEventQueue& events() override { return events_; }

    NetworkState state() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    std::vector<WifiAccessPoint> access_points(const std::string& device_id) const override {
        std::vector<WifiAccessPoint> out;
        for (const auto& w : mac::wifi_interfaces()) {
            if (!device_id.empty() && w.name != device_id) continue;
            auto aps = mac::wifi_cached(w.name);
            out.insert(out.end(), aps.begin(), aps.end());
        }
        return out;
    }

    Result request_wifi_scan(const std::string& device_id) override {
        std::vector<std::string> targets;
        std::string off;
        for (const auto& w : mac::wifi_interfaces()) {
            if (!device_id.empty() && w.name != device_id) continue;
            if (!w.power_on) off += (off.empty() ? "" : ", ") + w.name;
            else targets.push_back(w.name);
        }
        if (targets.empty()) {
            if (!off.empty()) return Result::failure("Wi-Fi is powered off (" + off + ")");
            return Result::failure(device_id.empty() ? "no Wi-Fi device" : "no Wi-Fi device " + device_id);
        }
        for (const auto& name : targets) start_scan(name);
        return Result::success();
    }

private:
    static void store_cb(SCDynamicStoreRef, CFArrayRef, void* info) {
        static_cast<MacNetworkService*>(info)->schedule_refresh();
    }

    // One scan per device at a time; a scan that outlives scan_timeout_ms is
    // reported as failed and its late result dropped.
    void start_scan(const std::string& name) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        dispatch_async(scan_queue_, ^{
          if (stopping_) return;
          mac::WifiScanResult r = mac::wifi_scan(name);
          if (done->exchange(true)) return;
          events_.push(WifiScanCompleted{name, r.ok, r.error, std::move(r.access_points)});
        });
        if (config_.scan_timeout_ms) {
            auto alive = alive_;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(config_.scan_timeout_ms) * int64_t(NSEC_PER_MSEC)),
                           queue_, ^{
                             if (!alive->load() || done->exchange(true)) return;
                             events_.push(WifiScanCompleted{name, false, "scan timed out", {}});
                           });
        }
    }

    void schedule_refresh() {
        if (stopping_ || refresh_pending_.exchange(true)) return;
        // Bursts (a link change rewrites several keys) collapse into one snapshot.
        auto alive = alive_;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * int64_t(NSEC_PER_MSEC)), queue_, ^{
          if (!alive->load()) return;
          refresh_pending_ = false;
          refresh();
        });
    }

    void refresh() {
        NetworkState s = mac::read_network_state(mac::wifi_interfaces(), connectivity_.load());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (have_state_ && s == state_) return;
            state_ = s;
            have_state_ = true;
        }
        events_.push(NetworkChanged{std::move(s)});
    }

    NetworkConfig config_;
    NetworkEventQueue events_;
    mutable std::mutex mutex_;
    NetworkState state_;
    bool have_state_ = false;

    dispatch_queue_t queue_;
    dispatch_queue_t scan_queue_;
    mac::CFRef<SCDynamicStoreRef> store_;
    std::shared_ptr<void> path_;
    std::shared_ptr<void> wifi_watch_;
    std::atomic<Connectivity> connectivity_{Connectivity::Unknown};
    std::atomic<bool> refresh_pending_{false};
    std::atomic<bool> stopping_{false};
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace

std::unique_ptr<NetworkService> NetworkService::create(const NetworkConfig& config, std::string* error) {
    auto s = std::make_unique<MacNetworkService>(config);
    if (!s->start(error)) return nullptr;
    return s;
}

}  // namespace brosys
