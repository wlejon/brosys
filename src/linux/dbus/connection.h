// One sd-bus connection with its own thread: the unit of a "role" (the
// notification server, the tray watcher, the UPower / logind client, ...).
//
// sd-bus objects are not thread-safe, so every sd-bus call happens on the
// connection's thread. Other threads post jobs (post / run_sync) or make
// blocking calls (call), which are marshalled onto that thread. Handlers
// (method handlers, signal handlers, reply handlers, timers) all run on the
// connection thread; they may call any member except run_sync-style waits
// for work queued behind them (call() on the bus thread is fine: it uses a
// synchronous sd_bus_call).
//
// Destroying the Connection stops and joins the thread first, so a service
// that owns its Connection as the first-destroyed member (declare it last)
// can capture `this` in handlers.
#pragma once

#include "linux/dbus/object.h"
#include "linux/dbus/value.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

typedef struct sd_bus sd_bus;
typedef struct sd_bus_slot sd_bus_slot;

namespace brosys::dbus {

enum class BusKind { System, Session };

struct Reply {
    bool ok = false;
    Args values;
    std::string error_name;
    std::string error_message;

    // "org.foo.Error: message" for Result::failure.
    std::string error() const;
    const Value* first() const { return values.empty() ? nullptr : &values[0]; }
};

using ReplyHandler = std::function<void(Reply)>;

// An incoming signal (or any message delivered to a match).
struct Message {
    std::string sender;
    std::string path;
    std::string interface;
    std::string member;
    Args args;
};

using SignalHandler = std::function<void(const Message&)>;

enum class NameRequest { PrimaryOwner, InQueue, Exists, AlreadyOwner, Error };

namespace name_flags {
inline constexpr uint32_t AllowReplacement = 1u << 0;
inline constexpr uint32_t ReplaceExisting = 1u << 1;
inline constexpr uint32_t Queue = 1u << 2;
}  // namespace name_flags

inline constexpr int kDefaultTimeoutMs = 25000;

class Connection {
public:
    // address empty: the default bus of that kind (DBUS_*_BUS_ADDRESS
    // honoured). `description` names the connection in sd-bus debug output.
    static std::unique_ptr<Connection> open(BusKind kind, const std::string& address,
                                            const std::string& description, std::string* error);
    ~Connection();
    // Stops and joins the bus thread (idempotent; the destructor calls it).
    // The object stays valid, so handlers still running during the join may
    // use it through the owner's pointer; afterwards calls fail as disconnected.
    void shutdown();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    std::string unique_name() const;
    bool connected() const { return connected_.load(); }
    // Called on the bus thread once when the connection drops.
    void set_disconnect_handler(std::function<void()> handler);

    // ---- threading
    bool on_bus_thread() const { return std::this_thread::get_id() == thread_id_; }
    void post(std::function<void()> job);
    // Runs `fn` on the bus thread and returns its result (inline when
    // already on the bus thread).
    template <class F>
    auto run_sync(F&& fn) -> std::invoke_result_t<F> {
        using R = std::invoke_result_t<F>;
        if (on_bus_thread()) return fn();
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        auto fut = task->get_future();
        post([task] { (*task)(); });
        return fut.get();
    }

    // One-shot timer on the bus thread; returns an id for cancel_timer.
    uint64_t add_timer(std::chrono::milliseconds delay, std::function<void()> fn);
    void cancel_timer(uint64_t id);

    // ---- client side
    // Blocking call from any thread.
    Reply call(const std::string& destination, const std::string& path, const std::string& interface,
               const std::string& member, const Args& args = Args(), int timeout_ms = kDefaultTimeoutMs);
    // Asynchronous call; `handler` runs on the bus thread (may be empty).
    void call_async(const std::string& destination, const std::string& path, const std::string& interface,
                    const std::string& member, const Args& args, ReplyHandler handler,
                    int timeout_ms = kDefaultTimeoutMs);
    // org.freedesktop.DBus.Properties helpers (blocking).
    Reply get_property(const std::string& destination, const std::string& path, const std::string& interface,
                       const std::string& name);
    // GetAll: a{sv} as a name -> (unwrapped) value map; empty map + *error on failure.
    std::map<std::string, Value> get_all_properties(const std::string& destination, const std::string& path,
                                                    const std::string& interface, std::string* error = nullptr);
    // Async GetAll; the handler receives (ok, map, error).
    void get_all_properties_async(const std::string& destination, const std::string& path,
                                  const std::string& interface,
                                  std::function<void(bool, std::map<std::string, Value>, std::string)> handler);

    // ---- signals
    // Adds a match rule ("type='signal',interface='...'"); the match is
    // installed before this returns. Returns an id for remove_match.
    uint64_t add_match(const std::string& rule, SignalHandler handler);
    void remove_match(uint64_t id);
    // Convenience: watch NameOwnerChanged for `name` ("" = every name). The
    // handler gets (name, old_owner, new_owner).
    uint64_t watch_name_owner(const std::string& name,
                              std::function<void(const std::string&, const std::string&, const std::string&)> handler);

    // ---- names
    NameRequest request_name(const std::string& name, uint32_t flags, std::string* error);
    bool release_name(const std::string& name);
    // Called on the bus thread for NameAcquired (true) / NameLost (false) of our names.
    void set_name_handler(std::function<void(const std::string& name, bool acquired)> handler);
    std::string get_name_owner(const std::string& name);  // "" when unowned
    std::vector<std::string> list_names();

    // ---- server side
    // Exports `iface` at `path` (several interfaces per path are fine).
    // Introspectable / Properties / Peer are provided automatically.
    bool export_interface(const std::string& path, std::shared_ptr<Interface> iface, std::string* error);
    void unexport_interface(const std::string& path, const std::string& interface_name);
    void unexport_path(const std::string& path);

    bool emit_signal(const std::string& path, const std::string& interface, const std::string& member,
                     const Args& args, const std::string& destination = std::string());
    // PropertiesChanged with the current values of `names` (from the getters).
    bool emit_properties_changed(const std::string& path, const std::string& interface,
                                 const std::vector<std::string>& names);

    // The raw bus, bus thread only (for the rare sd-bus feature not wrapped here).
    sd_bus* raw() const { return bus_; }

    // ---- internal (used by the sd-bus trampolines)
    struct Node {
        Connection* conn = nullptr;
        std::string path;
        sd_bus_slot* slot = nullptr;
        std::vector<std::shared_ptr<Interface>> interfaces;
    };
    struct Match;
    int dispatch(sd_bus_message* msg, Node& node);
    void handle_match(Match& match, sd_bus_message* msg);

private:
    Connection() = default;
    void run();
    void wake();
    void run_jobs();
    int run_timers();  // returns ms until the next timer (-1 none)
    void close_bus();
    Reply call_on_thread(const std::string& destination, const std::string& path, const std::string& interface,
                         const std::string& member, const Args& args, int timeout_ms);
    void call_async_on_thread(const std::string& destination, const std::string& path, const std::string& interface,
                              const std::string& member, const Args& args, ReplyHandler handler, int timeout_ms);
    std::vector<std::string> child_names(const std::string& path) const;
    int reply_properties(sd_bus_message* m, Node& node, const std::string& member);

    sd_bus* bus_ = nullptr;
    int wake_fd_ = -1;
    std::thread thread_;
    std::thread::id thread_id_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::string unique_name_;

    std::mutex jobs_mutex_;
    std::deque<std::function<void()>> jobs_;
    bool finished_ = false;  // guarded by jobs_mutex_: the thread has exited

    // Bus-thread state.
    struct Timer {
        std::chrono::steady_clock::time_point due;
        std::function<void()> fn;
    };
    std::map<uint64_t, Timer> timers_;
    std::atomic<uint64_t> next_id_{1};
    std::map<uint64_t, std::unique_ptr<Match>> matches_;
    std::map<std::string, std::unique_ptr<Node>> nodes_;
    std::function<void()> disconnect_handler_;
    std::function<void(const std::string&, bool)> name_handler_;
};

}  // namespace brosys::dbus
