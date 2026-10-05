// macOS power service. One serial dispatch queue receives every source of
// change and re-snapshots on it:
//   - IOPS notify(3) keys (any power-source change, providing source,
//     attach / detach, time remaining, low battery),
//   - IORegisterForSystemPower (can-sleep / will-sleep / has-powered-on),
//   - root-domain interest notifications (clamshell state),
//   - a poll timer for values that change without a notification (rate),
//   - NSWorkspaceWillPowerOff (GUI applications only) for ShutdownPrepare.
// PowerChanged / PowerCapabilitiesChanged are pushed only when the snapshot
// changed.
//
// Inhibitors: Sleep / Idle Block = IOPM assertions (visible in
// `pmset -g assertions`). A Sleep Delay inhibitor holds back the
// kIOMessageSystemWillSleep acknowledgement (IOAllowPowerChange) until it is
// released; the kernel stops waiting after about 30 s. macOS has no
// system-wide shutdown, lid-switch or power-key inhibitor.
#include "brosys/power.h"

#include "mac/power_mac.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dispatch/dispatch.h>
#include <notify.h>

#include <mutex>
#include <optional>

namespace brosys {

namespace {

// Will-sleep acknowledgement state, shared with Delay inhibitors (which may
// outlive the service).
struct SleepGate {
    std::mutex mutex;
    io_connect_t root = MACH_PORT_NULL;  // IORegisterForSystemPower connection; null once gone
    int delays = 0;
    std::optional<intptr_t> pending;
    bool pending_synthetic = false;
    std::vector<intptr_t> acknowledged;  // synthetic notifications (test seam)

    void ack_locked(intptr_t id, bool synthetic) {
        if (synthetic) acknowledged.push_back(id);
        else if (root != MACH_PORT_NULL) IOAllowPowerChange(root, id);
    }
    void release_pending_locked() {
        if (!pending) return;
        ack_locked(*pending, pending_synthetic);
        pending.reset();
    }
};

class MacInhibitor final : public Inhibitor {
public:
    std::vector<IOPMAssertionID> assertions;
    std::shared_ptr<SleepGate> gate;  // a Delay inhibitor

    ~MacInhibitor() override {
        for (IOPMAssertionID a : assertions) IOPMAssertionRelease(a);
        if (gate) {
            std::lock_guard<std::mutex> lock(gate->mutex);
            if (--gate->delays == 0) gate->release_pending_locked();
        }
    }
};

class MacPowerService final : public PowerService {
public:
    explicit MacPowerService(const PowerConfig& cfg)
        : config_(cfg), gate_(std::make_shared<SleepGate>()),
          queue_(dispatch_queue_create("brosys.power", DISPATCH_QUEUE_SERIAL)) {}

    ~MacPowerService() override {
        observer_.reset();
        if (timer_) {
            dispatch_source_cancel(timer_);
            dispatch_release(timer_);
        }
        for (int t : tokens_) notify_cancel(t);
        if (notifier_) IODeregisterForSystemPower(&notifier_);
        if (interest_) IOObjectRelease(interest_);
        {
            std::lock_guard<std::mutex> lock(gate_->mutex);
            gate_->release_pending_locked();  // never hold up a sleep we will not see through
            gate_->root = MACH_PORT_NULL;
        }
        if (root_) IOServiceClose(root_);
        dispatch_sync(queue_, ^{});  // callbacks already queued finish before teardown
        if (port_) IONotificationPortDestroy(port_);
        if (root_domain_) IOObjectRelease(root_domain_);
        dispatch_release(queue_);
    }

    bool start(std::string* error) {
        dispatch_sync(queue_, ^{ refresh(); });  // the first PowerChanged is queued before create() returns

        for (const char* key : {kIOPSNotifyAnyPowerSource, kIOPSNotifyPowerSource, kIOPSNotifyAttach,
                                kIOPSNotifyTimeRemaining, kIOPSNotifyLowBattery}) {
            int token = 0;
            if (notify_register_dispatch(key, &token, queue_, ^(int) { refresh(); }) == NOTIFY_STATUS_OK)
                tokens_.push_back(token);
        }
        root_ = IORegisterForSystemPower(this, &port_, &MacPowerService::system_power_cb, &notifier_);
        if (!root_) {
            if (error) *error = "IORegisterForSystemPower failed";
            return false;
        }
        IONotificationPortSetDispatchQueue(port_, queue_);
        {
            std::lock_guard<std::mutex> lock(gate_->mutex);
            gate_->root = root_;
        }
        root_domain_ = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
        if (root_domain_)
            IOServiceAddInterestNotification(port_, root_domain_, kIOGeneralInterest, &MacPowerService::interest_cb,
                                             this, &interest_);
        if (config_.poll_interval_ms) {
            timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
            uint64_t ns = uint64_t(config_.poll_interval_ms) * NSEC_PER_MSEC;
            dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, int64_t(ns)), ns, ns / 10);
            dispatch_source_set_event_handler(timer_, ^{ refresh(); });
            dispatch_resume(timer_);
        }
        observer_ = mac::observe_power_off([this] { events_.push(ShutdownPrepare{true}); });
        return true;
    }

    PowerEventQueue& events() override { return events_; }

    PowerState state() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    PowerCapabilities capabilities() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return caps_;
    }

    Result request(PowerAction action) override {
        if (action != PowerAction::Hibernate && action != PowerAction::HybridSleep) {
            Availability a = mac::read_power_capabilities().of(action);
            if (a == Availability::No || a == Availability::Unknown)
                return Result::failure(std::string(to_string(action)) + " is not available (" + to_string(a) + ")");
        }
        return mac::perform_power_action(action);
    }

    std::unique_ptr<Inhibitor> inhibit(const InhibitRequest& req, std::string* error) override {
        auto fail = [&](std::string why) -> std::unique_ptr<Inhibitor> {
            if (error) *error = std::move(why);
            return nullptr;
        };
        if (req.what == 0) return fail("nothing to inhibit");
        if (req.what & (inhibit::LidSwitch | inhibit::PowerKey))
            return fail("lid-switch / power-key inhibitors are logind's; macOS has none");
        if (req.what & inhibit::Shutdown)
            return fail("macOS has no system-wide shutdown inhibitor: only a GUI application can delay logout or "
                        "shutdown, from its applicationShouldTerminate: reply");
        auto inh = std::make_unique<MacInhibitor>();
        if (req.mode == InhibitMode::Delay) {
            if (req.what != inhibit::Sleep) return fail("only sleep can be delayed on macOS");
            std::lock_guard<std::mutex> lock(gate_->mutex);
            ++gate_->delays;
            inh->gate = gate_;
            return inh;
        }
        std::string name = req.who.empty() ? req.why : req.who + ": " + req.why;
        auto assert_type = [&](CFStringRef type) -> bool {
            CFStringRef cfname = CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8);
            IOPMAssertionID id = kIOPMNullAssertionID;
            IOReturn r = IOPMAssertionCreateWithName(type, kIOPMAssertionLevelOn,
                                                     cfname ? cfname : CFSTR("brosys"), &id);
            if (cfname) CFRelease(cfname);
            if (r != kIOReturnSuccess) return false;
            inh->assertions.push_back(id);
            return true;
        };
        if ((req.what & inhibit::Sleep) && !assert_type(kIOPMAssertionTypePreventSystemSleep))
            return fail("IOPMAssertionCreateWithName(PreventSystemSleep) failed");
        if (req.what & inhibit::Idle) {
            if (!assert_type(kIOPMAssertPreventUserIdleSystemSleep) ||
                !assert_type(kIOPMAssertPreventUserIdleDisplaySleep))
                return fail("IOPMAssertionCreateWithName(PreventUserIdle*) failed");
        }
        return inh;
    }

    // Test seam: the handler with a synthetic notification, on the queue.
    void deliver(uint32_t message, intptr_t id) {
        dispatch_sync(queue_, ^{ on_system_power(message, id, true); });
    }
    std::vector<intptr_t> acknowledged() {
        std::lock_guard<std::mutex> lock(gate_->mutex);
        return gate_->acknowledged;
    }

private:
    static void system_power_cb(void* refcon, io_service_t, natural_t type, void* arg) {
        static_cast<MacPowerService*>(refcon)->on_system_power(type, reinterpret_cast<intptr_t>(arg), false);
    }

    static void interest_cb(void* refcon, io_service_t, natural_t type, void*) {
        if (type == kIOPMMessageClamshellStateChange) static_cast<MacPowerService*>(refcon)->refresh();
    }

    void on_system_power(uint32_t type, intptr_t id, bool synthetic) {
        switch (type) {
            case kIOMessageCanSystemSleep: {
                // Idle sleep is vetoed by assertions, not here.
                std::lock_guard<std::mutex> lock(gate_->mutex);
                gate_->ack_locked(id, synthetic);
                break;
            }
            case kIOMessageSystemWillSleep: {
                sleeping_ = true;
                events_.push(SleepPrepare{true});
                std::lock_guard<std::mutex> lock(gate_->mutex);
                if (gate_->delays > 0) {
                    gate_->release_pending_locked();
                    gate_->pending = id;
                    gate_->pending_synthetic = synthetic;
                } else {
                    gate_->ack_locked(id, synthetic);
                }
                break;
            }
            case kIOMessageSystemHasPoweredOn:
                if (sleeping_) {
                    sleeping_ = false;
                    events_.push(SleepPrepare{false});
                }
                refresh();
                break;
            default: break;
        }
    }

    void refresh() {
        PowerState s = mac::read_power_state();
        PowerCapabilities c = mac::read_power_capabilities();
        bool state_changed, caps_changed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_changed = !have_state_ || !(s == state_);
            caps_changed = !have_state_ || !(c == caps_);
            state_ = s;
            caps_ = c;
            have_state_ = true;
        }
        if (state_changed) events_.push(PowerChanged{std::move(s)});
        if (caps_changed) events_.push(PowerCapabilitiesChanged{c});
    }

    PowerConfig config_;
    PowerEventQueue events_;
    mutable std::mutex mutex_;
    PowerState state_;
    PowerCapabilities caps_;
    bool have_state_ = false;
    bool sleeping_ = false;  // queue only

    std::shared_ptr<SleepGate> gate_;
    dispatch_queue_t queue_;
    dispatch_source_t timer_ = nullptr;
    std::vector<int> tokens_;
    io_connect_t root_ = MACH_PORT_NULL;
    IONotificationPortRef port_ = nullptr;
    io_object_t notifier_ = 0;
    io_service_t root_domain_ = 0;
    io_object_t interest_ = 0;
    std::shared_ptr<void> observer_;
};

}  // namespace

namespace mac::power_testing {

void deliver_system_power(PowerService& service, uint32_t message, intptr_t notification_id) {
    if (auto* s = dynamic_cast<MacPowerService*>(&service)) s->deliver(message, notification_id);
}

std::vector<intptr_t> acknowledged(PowerService& service) {
    auto* s = dynamic_cast<MacPowerService*>(&service);
    return s ? s->acknowledged() : std::vector<intptr_t>{};
}

}  // namespace mac::power_testing

std::unique_ptr<PowerService> PowerService::create(const PowerConfig& config, std::string* error) {
    auto s = std::make_unique<MacPowerService>(config);
    if (!s->start(error)) return nullptr;
    return s;
}

std::unique_ptr<ScreenSaverServer> ScreenSaverServer::create(const ScreenSaverConfig&, std::string* error) {
    if (error) *error = "ScreenSaverServer is unsupported on macOS";
    return nullptr;
}

}  // namespace brosys
