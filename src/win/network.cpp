// Windows network: one worker thread (COM MTA) owns the snapshot. IP
// Helper change notifications (interfaces, unicast addresses, routes,
// connectivity hint) and WLAN notifications arrive on system threads and
// only mark work; the worker debounces, re-snapshots, and pushes
// NetworkChanged when the state differs, and WifiScanCompleted when a scan
// finishes (or fails / times out).
// winsock2 / ws2ipdef before windows.h: netioapi.h only declares the MIB
// notification API when the IP definitions came first.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "brosys/network.h"

#include "win/network_snapshot.h"
#include "win/util.h"
#include "win/wifi.h"

#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace brosys {

namespace {

constexpr auto kDebounce = std::chrono::milliseconds(150);

struct HintStruct {  // NL_NETWORK_CONNECTIVITY_HINT (declared for newer NTDDI only)
    int level;
    int cost;
    BOOLEAN approaching;
    BOOLEAN over;
    BOOLEAN roaming;
};
using HintCallback = VOID(WINAPI*)(PVOID, HintStruct);
using NotifyHintFn = DWORD(WINAPI*)(HintCallback, PVOID, BOOLEAN, PHANDLE);

std::string luid_of(const GUID& g) {
    NET_LUID luid{};
    if (ConvertInterfaceGuidToLuid(&g, &luid) != NO_ERROR) return {};
    return std::to_string(luid.Value);
}

class WinNetworkService final : public NetworkService {
public:
    explicit WinNetworkService(const NetworkConfig& cfg) : config_(cfg) {}

    ~WinNetworkService() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    bool start(std::string* error) {
        std::promise<std::string> ready;
        auto fut = ready.get_future();
        thread_ = std::thread([this, &ready] { thread_main(ready); });
        std::string err = fut.get();
        if (!err.empty()) {
            if (thread_.joinable()) thread_.join();
            if (error) *error = err;
            return false;
        }
        return true;
    }

    NetworkEventQueue& events() override { return queue_; }

    NetworkState state() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    std::vector<WifiAccessPoint> access_points(const std::string& device_id) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<WifiAccessPoint> out;
        for (auto& [id, aps] : aps_)
            if (device_id.empty() || id == device_id) out.insert(out.end(), aps.begin(), aps.end());
        return out;
    }

    Result request_wifi_scan(const std::string& device_id) override {
        auto task = std::make_shared<std::packaged_task<Result()>>([this, device_id] { return start_scan(device_id); });
        auto fut = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_ || finished_) return Result::failure("the network service is shutting down");
            commands_.push_back([task] { (*task)(); });
        }
        cv_.notify_all();
        return fut.get();
    }

private:
    struct ScanSignal {
        std::string device_id;
        int kind;  // 1 complete, 2 failed, 3 list refreshed
        DWORD reason;
    };

    // ---- notification callbacks (system threads): mark and wake.
    void mark_dirty() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!dirty_) dirty_at_ = std::chrono::steady_clock::now();
            dirty_ = true;
        }
        cv_.notify_all();
    }
    static VOID WINAPI on_interface(PVOID ctx, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
        static_cast<WinNetworkService*>(ctx)->mark_dirty();
    }
    static VOID WINAPI on_address(PVOID ctx, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
        static_cast<WinNetworkService*>(ctx)->mark_dirty();
    }
    static VOID WINAPI on_route(PVOID ctx, PMIB_IPFORWARD_ROW2, MIB_NOTIFICATION_TYPE) {
        static_cast<WinNetworkService*>(ctx)->mark_dirty();
    }
    static VOID WINAPI on_hint(PVOID ctx, HintStruct) { static_cast<WinNetworkService*>(ctx)->mark_dirty(); }

    void on_wifi(const GUID& iface, int kind, DWORD reason) {
        if (kind == 0) {
            mark_dirty();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            scan_signals_.push_back(ScanSignal{luid_of(iface), kind, reason});
        }
        cv_.notify_all();
    }

    // ---- worker
    Result start_scan(const std::string& device_id) {
        if (!wifi_.is_open())
            return Result::failure("Wi-Fi is unavailable: " + (wifi_error_.empty() ? std::string("no WLAN service") : wifi_error_));
        bool any = false;
        std::string errors;
        for (auto& w : wifi_.interfaces()) {
            if (!device_id.empty() && w.device_id != device_id) continue;
            any = true;
            DWORD r = wifi_.scan(w.guid);
            if (r != ERROR_SUCCESS) {
                errors += win::win32_error("WlanScan", r) + "; ";
                continue;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            pending_scans_[w.device_id] = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.scan_timeout_ms);
        }
        if (!any) return Result::failure(device_id.empty() ? "no Wi-Fi device" : "no Wi-Fi device " + device_id);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool started = false;
            for (auto& [id, dl] : pending_scans_) started |= device_id.empty() || id == device_id;
            if (!started) return Result::failure(errors);
        }
        return Result::success();
    }

    std::vector<WifiAccessPoint> read_aps(const std::string& device_id) {
        for (auto& w : wifi_.interfaces())
            if (w.device_id == device_id) return wifi_.access_points(w);
        return {};
    }

    void refresh(bool initial) {
        auto ifaces = wifi_.interfaces();
        NetworkState s = win::read_network_state(ifaces);
        bool changed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            changed = initial || !(s == state_);
            state_ = s;
            if (initial)
                for (auto& w : ifaces) aps_[w.device_id] = wifi_.access_points(w);  // the OS's cached list
        }
        if (changed) queue_.push(NetworkChanged{std::move(s)});
    }

    void handle_scan(const ScanSignal& sig) {
        auto aps = read_aps(sig.device_id);
        bool pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            aps_[sig.device_id] = aps;
            pending = pending_scans_.erase(sig.device_id) > 0;
        }
        if (sig.kind == 1) {
            queue_.push(WifiScanCompleted{sig.device_id, true, {}, std::move(aps)});
        } else if (sig.kind == 2 && pending) {
            queue_.push(WifiScanCompleted{sig.device_id, false, win::win32_error("Wi-Fi scan", sig.reason), std::move(aps)});
        } else if (pending) {
            // A list refresh does not end a requested scan: keep waiting.
            std::lock_guard<std::mutex> lock(mutex_);
            pending_scans_.emplace(sig.device_id,
                                   std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.scan_timeout_ms));
        }
    }

    void thread_main(std::promise<std::string>& ready) {
        win::ComInit com;
        if (!wifi_.open([this](const GUID& g, int kind, DWORD reason) { on_wifi(g, kind, reason); }, &wifi_error_))
            wifi_.close();
        std::vector<HANDLE> notes;
        HANDLE h = nullptr;
        if (NotifyIpInterfaceChange(AF_UNSPEC, &on_interface, this, FALSE, &h) == NO_ERROR) notes.push_back(h);
        if (NotifyUnicastIpAddressChange(AF_UNSPEC, &on_address, this, FALSE, &h) == NO_ERROR) notes.push_back(h);
        if (NotifyRouteChange2(AF_UNSPEC, &on_route, this, FALSE, &h) == NO_ERROR) notes.push_back(h);
        auto notify_hint = reinterpret_cast<NotifyHintFn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"iphlpapi.dll"), "NotifyNetworkConnectivityHintChange")));
        if (notify_hint && notify_hint(&on_hint, this, FALSE, &h) == NO_ERROR) notes.push_back(h);

        refresh(true);
        ready.set_value(std::string());

        while (true) {
            std::deque<std::function<void()>> commands;
            std::deque<ScanSignal> signals;
            std::vector<std::string> timed_out;
            bool do_refresh = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                auto next = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                if (dirty_) next = std::min(next, dirty_at_ + kDebounce);
                for (auto& [id, dl] : pending_scans_) next = std::min(next, dl);
                cv_.wait_until(lock, next, [&] {
                    return stop_ || !commands_.empty() || !scan_signals_.empty() ||
                           (dirty_ && std::chrono::steady_clock::now() >= dirty_at_ + kDebounce);
                });
                if (stop_) break;
                commands.swap(commands_);
                signals.swap(scan_signals_);
                auto now = std::chrono::steady_clock::now();
                if (dirty_ && now >= dirty_at_ + kDebounce) {
                    dirty_ = false;
                    do_refresh = true;
                }
                for (auto it = pending_scans_.begin(); it != pending_scans_.end();) {
                    if (it->second <= now) {
                        timed_out.push_back(it->first);
                        it = pending_scans_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (auto& c : commands) c();
            for (auto& s : signals) handle_scan(s);
            for (auto& id : timed_out)
                queue_.push(WifiScanCompleted{id, false, "the scan did not complete within the timeout", read_aps(id)});
            if (do_refresh) refresh(false);
        }
        for (auto n : notes) CancelMibChangeNotify2(n);  // waits for callbacks in flight
        wifi_.close();
        std::deque<std::function<void()>> rest;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
            rest.swap(commands_);
        }
        for (auto& c : rest) c();  // the WLAN handle is closed: they fail cleanly
    }

    NetworkConfig config_;
    NetworkEventQueue queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    NetworkState state_;
    std::map<std::string, std::vector<WifiAccessPoint>> aps_;
    std::map<std::string, std::chrono::steady_clock::time_point> pending_scans_;
    std::deque<std::function<void()>> commands_;
    std::deque<ScanSignal> scan_signals_;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point dirty_at_;
    bool stop_ = false;
    bool finished_ = false;

    win::WifiClient wifi_;
    std::string wifi_error_;
    std::thread thread_;
};

}  // namespace

std::unique_ptr<NetworkService> NetworkService::create(const NetworkConfig& config, std::string* error) {
    auto s = std::make_unique<WinNetworkService>(config);
    if (!s->start(error)) return nullptr;
    return s;
}

}  // namespace brosys
