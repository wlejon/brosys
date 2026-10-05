#include "linux/dbus/connection.h"

#include <systemd/sd-bus.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace brosys::dbus {

namespace {

std::string errno_text(const char* what, int r) { return std::string(what) + ": " + std::strerror(r < 0 ? -r : r); }

uint64_t monotonic_usec() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

const char* safe(const char* s) { return s ? s : ""; }

Reply reply_from_message(sd_bus_message* m) {
    Reply r;
    if (!m) {
        r.error_name = kErrorFailed;
        r.error_message = "no reply";
        return r;
    }
    if (sd_bus_message_is_method_error(m, nullptr)) {
        const sd_bus_error* e = sd_bus_message_get_error(m);
        r.error_name = e ? safe(e->name) : kErrorFailed;
        r.error_message = e ? safe(e->message) : "";
        return r;
    }
    std::string err;
    if (!read_all(m, r.values, &err)) {
        r.error_name = kErrorFailed;
        r.error_message = "malformed reply: " + err;
        return r;
    }
    r.ok = true;
    return r;
}

// An in-flight async call. Owned by its (floating) slot: the destroy
// callback frees it, and reports an error if no reply was delivered (the
// connection closed first).
struct PendingCall {
    ReplyHandler handler;
    bool done = false;

    void finish(Reply r) {
        if (done) return;
        done = true;
        if (handler) handler(std::move(r));
    }
    ~PendingCall() {
        if (!done) {
            Reply r;
            r.error_name = "org.freedesktop.DBus.Error.Disconnected";
            r.error_message = "connection closed before the reply arrived";
            finish(std::move(r));
        }
    }
};

int pending_reply(sd_bus_message* m, void* userdata, sd_bus_error*) {
    static_cast<PendingCall*>(userdata)->finish(reply_from_message(m));
    return 1;
}

void pending_destroy(void* userdata) { delete static_cast<PendingCall*>(userdata); }

int node_trampoline(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* node = static_cast<Connection::Node*>(userdata);
    return node->conn->dispatch(m, *node);
}

}  // namespace

struct Connection::Match {
    Connection* conn = nullptr;
    sd_bus_slot* slot = nullptr;
    SignalHandler handler;
};

namespace {
int match_trampoline(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* match = static_cast<Connection::Match*>(userdata);
    match->conn->handle_match(*match, m);
    return 0;  // let other matches see it too
}
}  // namespace

std::string Reply::error() const {
    if (ok) return {};
    if (error_message.empty()) return error_name;
    return error_name + ": " + error_message;
}

// ---------------------------------------------------------------- lifecycle

std::unique_ptr<Connection> Connection::open(BusKind kind, const std::string& address,
                                             const std::string& description, std::string* error) {
    std::unique_ptr<Connection> c(new Connection());
    sd_bus* bus = nullptr;
    int r;
    if (address.empty()) {
        r = kind == BusKind::System ? sd_bus_open_system_with_description(&bus, description.c_str())
                                    : sd_bus_open_user_with_description(&bus, description.c_str());
        if (r < 0) {
            if (error) *error = errno_text(kind == BusKind::System ? "open system bus" : "open session bus", r);
            return nullptr;
        }
    } else {
        r = sd_bus_new(&bus);
        if (r >= 0) r = sd_bus_set_address(bus, address.c_str());
        if (r >= 0) r = sd_bus_set_bus_client(bus, 1);
        if (r >= 0) r = sd_bus_negotiate_fds(bus, 1);
        if (r >= 0) r = sd_bus_set_description(bus, description.c_str());
        if (r >= 0) r = sd_bus_start(bus);
        if (r < 0) {
            if (error) *error = errno_text(("connect to " + address).c_str(), r);
            sd_bus_unref(bus);
            return nullptr;
        }
    }
    c->bus_ = bus;
    const char* unique = nullptr;
    if (sd_bus_get_unique_name(bus, &unique) >= 0) c->unique_name_ = safe(unique);
    c->wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (c->wake_fd_ < 0) {
        if (error) *error = errno_text("eventfd", errno);
        sd_bus_flush_close_unref(bus);
        c->bus_ = nullptr;
        return nullptr;
    }
    c->connected_ = true;
    Connection* self = c.get();
    // NameAcquired / NameLost are unicast to us; a local match routes them.
    auto m = std::make_unique<Match>();
    m->conn = self;
    m->handler = [self](const Message& msg) {
        // The match also sees the daemon's NameOwnerChanged broadcasts.
        if (msg.member != "NameAcquired" && msg.member != "NameLost") return;
        if (!self->name_handler_ || msg.args.empty()) return;
        self->name_handler_(msg.args[0].as_string(), msg.member == "NameAcquired");
    };
    r = sd_bus_match_signal(bus, &m->slot, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                            nullptr, match_trampoline, m.get());
    if (r >= 0) c->matches_.emplace(c->next_id_++, std::move(m));
    c->thread_ = std::thread([self] { self->run(); });
    c->thread_id_ = c->thread_.get_id();
    return c;
}

Connection::~Connection() {
    shutdown();
    if (wake_fd_ >= 0) ::close(wake_fd_);
}

void Connection::shutdown() {
    stop_ = true;
    wake();
    if (thread_.joinable() && !on_bus_thread()) thread_.join();
}

std::string Connection::unique_name() const { return unique_name_; }

void Connection::set_disconnect_handler(std::function<void()> handler) {
    run_sync([&] { disconnect_handler_ = std::move(handler); });
}

void Connection::wake() {
    if (wake_fd_ < 0) return;
    uint64_t one = 1;
    ssize_t n = ::write(wake_fd_, &one, sizeof one);
    (void)n;
}

void Connection::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        if (!finished_) {
            jobs_.push_back(std::move(job));
            job = nullptr;
        }
    }
    if (job) {
        job();  // the loop is gone: run inline (the bus is closed by now)
        return;
    }
    wake();
}

void Connection::run_jobs() {
    while (true) {
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            batch.swap(jobs_);
        }
        if (batch.empty()) return;
        for (auto& j : batch) j();
    }
}

uint64_t Connection::add_timer(std::chrono::milliseconds delay, std::function<void()> fn) {
    uint64_t id = next_id_++;
    auto due = std::chrono::steady_clock::now() + delay;
    post([this, id, due, fn = std::move(fn)]() mutable { timers_[id] = Timer{due, std::move(fn)}; });
    return id;
}

void Connection::cancel_timer(uint64_t id) {
    post([this, id] { timers_.erase(id); });
}

int Connection::run_timers() {
    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto next = timers_.end();
        for (auto it = timers_.begin(); it != timers_.end(); ++it)
            if (next == timers_.end() || it->second.due < next->second.due) next = it;
        if (next == timers_.end()) return -1;
        if (next->second.due > now) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next->second.due - now).count();
            return static_cast<int>(ms) + 1;
        }
        auto fn = std::move(next->second.fn);
        timers_.erase(next);
        if (fn) fn();
    }
}

void Connection::close_bus() {
    for (auto& [id, m] : matches_) sd_bus_slot_unref(m->slot);
    matches_.clear();
    for (auto& [path, n] : nodes_) sd_bus_slot_unref(n->slot);
    nodes_.clear();
    if (bus_) {
        // Destroys floating async-call slots: their PendingCalls report
        // "connection closed" to anyone waiting.
        sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
    }
    connected_ = false;
}

void Connection::run() {
    bool reported_disconnect = false;
    while (!stop_) {
        run_jobs();
        if (bus_ && connected_) {
            int r;
            while ((r = sd_bus_process(bus_, nullptr)) > 0) {
                if (stop_) break;
            }
            if (r < 0 || !sd_bus_is_open(bus_)) connected_ = false;
        }
        if (!connected_ && !reported_disconnect) {
            reported_disconnect = true;
            if (disconnect_handler_) disconnect_handler_();
        }
        run_jobs();
        int timeout = run_timers();
        if (stop_) break;

        pollfd fds[2];
        int nfds = 0;
        fds[nfds++] = pollfd{wake_fd_, POLLIN, 0};
        if (bus_ && connected_) {
            int events = sd_bus_get_events(bus_);
            int fd = sd_bus_get_fd(bus_);
            if (fd >= 0 && events >= 0) fds[nfds++] = pollfd{fd, static_cast<short>(events), 0};
            uint64_t until = 0;
            if (sd_bus_get_timeout(bus_, &until) >= 0 && until != UINT64_MAX) {
                uint64_t now = monotonic_usec();
                int bus_ms = until <= now ? 0 : static_cast<int>((until - now + 999) / 1000);
                if (timeout < 0 || bus_ms < timeout) timeout = bus_ms;
            }
        }
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            if (!jobs_.empty()) timeout = 0;
        }
        ::poll(fds, static_cast<nfds_t>(nfds), timeout);
        if (fds[0].revents & POLLIN) {
            uint64_t v;
            ssize_t n = ::read(wake_fd_, &v, sizeof v);
            (void)n;
        }
    }
    run_jobs();
    timers_.clear();
    close_bus();
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        finished_ = true;
    }
    run_jobs();
}

// ---------------------------------------------------------------- calls

Reply Connection::call_on_thread(const std::string& destination, const std::string& path,
                                 const std::string& interface, const std::string& member, const Args& args,
                                 int timeout_ms) {
    Reply out;
    out.error_name = kErrorFailed;
    if (!bus_ || !connected_) {
        out.error_name = "org.freedesktop.DBus.Error.Disconnected";
        out.error_message = "not connected";
        return out;
    }
    sd_bus_message* m = nullptr;
    int r = sd_bus_message_new_method_call(bus_, &m, destination.empty() ? nullptr : destination.c_str(),
                                           path.c_str(), interface.empty() ? nullptr : interface.c_str(),
                                           member.c_str());
    if (r >= 0) r = append_all(m, args);
    if (r < 0) {
        out.error_message = errno_text("build call", r);
        sd_bus_message_unref(m);
        return out;
    }
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    r = sd_bus_call(bus_, m, static_cast<uint64_t>(timeout_ms) * 1000u, &err, &reply);
    sd_bus_message_unref(m);
    if (r < 0) {
        out.error_name = sd_bus_error_is_set(&err) ? safe(err.name) : kErrorFailed;
        out.error_message = sd_bus_error_is_set(&err) ? safe(err.message) : errno_text("call", r);
        sd_bus_error_free(&err);
        return out;
    }
    sd_bus_error_free(&err);
    out = reply_from_message(reply);
    sd_bus_message_unref(reply);
    return out;
}

void Connection::call_async_on_thread(const std::string& destination, const std::string& path,
                                      const std::string& interface, const std::string& member, const Args& args,
                                      ReplyHandler handler, int timeout_ms) {
    auto* pending = new PendingCall{std::move(handler)};
    if (!bus_ || !connected_) {
        delete pending;  // reports Disconnected
        return;
    }
    sd_bus_message* m = nullptr;
    int r = sd_bus_message_new_method_call(bus_, &m, destination.empty() ? nullptr : destination.c_str(),
                                           path.c_str(), interface.empty() ? nullptr : interface.c_str(),
                                           member.c_str());
    if (r >= 0) r = append_all(m, args);
    sd_bus_slot* slot = nullptr;
    if (r >= 0) r = sd_bus_call_async(bus_, &slot, m, pending_reply, pending, static_cast<uint64_t>(timeout_ms) * 1000u);
    sd_bus_message_unref(m);
    if (r < 0) {
        Reply e;
        e.error_name = kErrorFailed;
        e.error_message = errno_text("call", r);
        pending->finish(std::move(e));
        delete pending;
        return;
    }
    sd_bus_slot_set_destroy_callback(slot, pending_destroy);
    sd_bus_slot_set_floating(slot, 1);
    sd_bus_slot_unref(slot);  // the bus keeps the floating slot
}

Reply Connection::call(const std::string& destination, const std::string& path, const std::string& interface,
                       const std::string& member, const Args& args, int timeout_ms) {
    if (on_bus_thread()) return call_on_thread(destination, path, interface, member, args, timeout_ms);
    auto promise = std::make_shared<std::promise<Reply>>();
    auto fut = promise->get_future();
    post([=, this] {
        call_async_on_thread(destination, path, interface, member, args,
                             [promise](Reply r) { promise->set_value(std::move(r)); }, timeout_ms);
    });
    return fut.get();
}

void Connection::call_async(const std::string& destination, const std::string& path, const std::string& interface,
                            const std::string& member, const Args& args, ReplyHandler handler, int timeout_ms) {
    if (on_bus_thread()) {
        call_async_on_thread(destination, path, interface, member, args, std::move(handler), timeout_ms);
        return;
    }
    post([=, this, handler = std::move(handler)]() mutable {
        call_async_on_thread(destination, path, interface, member, args, std::move(handler), timeout_ms);
    });
}

Reply Connection::get_property(const std::string& destination, const std::string& path,
                               const std::string& interface, const std::string& name) {
    Reply r = call(destination, path, "org.freedesktop.DBus.Properties", "Get", {Value::str(interface), Value::str(name)});
    if (r.ok && !r.values.empty()) {
        Value inner = r.values[0].unwrap();  // copy first: it lives inside the box the assignment frees
        r.values[0] = std::move(inner);
    }
    return r;
}

static std::map<std::string, Value> props_from_reply(const Reply& r) {
    std::map<std::string, Value> out;
    if (!r.ok || r.values.empty()) return out;
    for (auto& entry : r.values[0].items()) {
        auto& kv = entry.items();
        if (kv.size() == 2) out[kv[0].as_string()] = kv[1].unwrap();
    }
    return out;
}

std::map<std::string, Value> Connection::get_all_properties(const std::string& destination, const std::string& path,
                                                            const std::string& interface, std::string* error) {
    Reply r = call(destination, path, "org.freedesktop.DBus.Properties", "GetAll", {Value::str(interface)});
    if (!r.ok && error) *error = r.error();
    return props_from_reply(r);
}

void Connection::get_all_properties_async(
    const std::string& destination, const std::string& path, const std::string& interface,
    std::function<void(bool, std::map<std::string, Value>, std::string)> handler) {
    call_async(destination, path, "org.freedesktop.DBus.Properties", "GetAll", {Value::str(interface)},
               [handler = std::move(handler)](Reply r) {
                   if (handler) handler(r.ok, props_from_reply(r), r.error());
               });
}

// ---------------------------------------------------------------- signals

void Connection::handle_match(Match& match, sd_bus_message* m) {
    Message msg;
    msg.sender = safe(sd_bus_message_get_sender(m));
    msg.path = safe(sd_bus_message_get_path(m));
    msg.interface = safe(sd_bus_message_get_interface(m));
    msg.member = safe(sd_bus_message_get_member(m));
    std::string err;
    if (!read_all(m, msg.args, &err)) return;
    sd_bus_message_rewind(m, true);
    if (match.handler) match.handler(msg);
}

uint64_t Connection::add_match(const std::string& rule, SignalHandler handler) {
    return run_sync([&]() -> uint64_t {
        if (!bus_ || !connected_) return 0;
        auto m = std::make_unique<Match>();
        m->conn = this;
        m->handler = std::move(handler);
        int r = sd_bus_add_match(bus_, &m->slot, rule.c_str(), match_trampoline, m.get());
        if (r < 0) return 0;
        uint64_t id = next_id_++;
        matches_.emplace(id, std::move(m));
        return id;
    });
}

void Connection::remove_match(uint64_t id) {
    post([this, id] {
        auto it = matches_.find(id);
        if (it == matches_.end()) return;
        sd_bus_slot_unref(it->second->slot);
        matches_.erase(it);
    });
}

uint64_t Connection::watch_name_owner(
    const std::string& name,
    std::function<void(const std::string&, const std::string&, const std::string&)> handler) {
    std::string rule =
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
        "interface='org.freedesktop.DBus',member='NameOwnerChanged'";
    if (!name.empty()) rule += ",arg0='" + name + "'";
    return add_match(rule, [handler = std::move(handler)](const Message& m) {
        if (m.args.size() < 3) return;
        handler(m.args[0].as_string(), m.args[1].as_string(), m.args[2].as_string());
    });
}

// ---------------------------------------------------------------- names

NameRequest Connection::request_name(const std::string& name, uint32_t flags, std::string* error) {
    return run_sync([&]() -> NameRequest {
        if (!bus_ || !connected_) {
            if (error) *error = "not connected";
            return NameRequest::Error;
        }
        uint32_t f = 0;
        if (flags & name_flags::AllowReplacement) f |= 0x1;  // DBUS_NAME_FLAG_ALLOW_REPLACEMENT
        if (flags & name_flags::ReplaceExisting) f |= 0x2;   // DBUS_NAME_FLAG_REPLACE_EXISTING
        if (!(flags & name_flags::Queue)) f |= 0x4;          // DBUS_NAME_FLAG_DO_NOT_QUEUE
        // The raw RequestName call reports every outcome (sd_bus_request_name folds some together).
        Reply r = call_on_thread("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                 "RequestName", {Value::str(name), Value::u32(f)}, kDefaultTimeoutMs);
        if (!r.ok || r.values.empty()) {
            if (error) *error = r.error();
            return NameRequest::Error;
        }
        switch (r.values[0].as_uint()) {
            case 1: return NameRequest::PrimaryOwner;
            case 2: return NameRequest::InQueue;
            case 3: return NameRequest::Exists;
            case 4: return NameRequest::AlreadyOwner;
            default: return NameRequest::Error;
        }
    });
}

bool Connection::release_name(const std::string& name) {
    Reply r = call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName",
                   {Value::str(name)});
    return r.ok;
}

void Connection::set_name_handler(std::function<void(const std::string& name, bool acquired)> handler) {
    run_sync([&] { name_handler_ = std::move(handler); });
}

std::string Connection::get_name_owner(const std::string& name) {
    Reply r = call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
                   {Value::str(name)});
    return r.ok && !r.values.empty() ? r.values[0].as_string() : std::string();
}

std::vector<std::string> Connection::list_names() {
    Reply r = call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames");
    return r.ok && !r.values.empty() ? r.values[0].as_strings() : std::vector<std::string>();
}

// ---------------------------------------------------------------- export

bool Connection::export_interface(const std::string& path, std::shared_ptr<Interface> iface, std::string* error) {
    return run_sync([&]() -> bool {
        if (!bus_ || !connected_) {
            if (error) *error = "not connected";
            return false;
        }
        if (!sd_bus_object_path_is_valid(path.c_str())) {
            if (error) *error = "invalid object path " + path;
            return false;
        }
        auto& node = nodes_[path];
        if (!node) {
            node = std::make_unique<Node>();
            node->conn = this;
            node->path = path;
            int r = sd_bus_add_object(bus_, &node->slot, path.c_str(), node_trampoline, node.get());
            if (r < 0) {
                nodes_.erase(path);
                if (error) *error = errno_text("sd_bus_add_object", r);
                return false;
            }
        }
        for (auto& existing : node->interfaces)
            if (existing->name == iface->name) {
                existing = iface;
                return true;
            }
        node->interfaces.push_back(std::move(iface));
        return true;
    });
}

void Connection::unexport_interface(const std::string& path, const std::string& interface_name) {
    run_sync([&] {
        auto it = nodes_.find(path);
        if (it == nodes_.end()) return;
        auto& ifs = it->second->interfaces;
        for (auto i = ifs.begin(); i != ifs.end(); ++i)
            if ((*i)->name == interface_name) {
                ifs.erase(i);
                break;
            }
        if (ifs.empty()) {
            sd_bus_slot_unref(it->second->slot);
            nodes_.erase(it);
        }
    });
}

void Connection::unexport_path(const std::string& path) {
    run_sync([&] {
        auto it = nodes_.find(path);
        if (it == nodes_.end()) return;
        sd_bus_slot_unref(it->second->slot);
        nodes_.erase(it);
    });
}

bool Connection::emit_signal(const std::string& path, const std::string& interface, const std::string& member,
                             const Args& args, const std::string& destination) {
    return run_sync([&]() -> bool {
        if (!bus_ || !connected_) return false;
        sd_bus_message* m = nullptr;
        int r = sd_bus_message_new_signal(bus_, &m, path.c_str(), interface.c_str(), member.c_str());
        if (r >= 0 && !destination.empty()) r = sd_bus_message_set_destination(m, destination.c_str());
        if (r >= 0) r = append_all(m, args);
        if (r >= 0) r = sd_bus_send(bus_, m, nullptr);
        sd_bus_message_unref(m);
        return r >= 0;
    });
}

bool Connection::emit_properties_changed(const std::string& path, const std::string& interface,
                                         const std::vector<std::string>& names) {
    return run_sync([&]() -> bool {
        auto it = nodes_.find(path);
        if (it == nodes_.end()) return false;
        const Interface* iface = nullptr;
        for (auto& i : it->second->interfaces)
            if (i->name == interface) iface = i.get();
        if (!iface) return false;
        std::vector<std::pair<std::string, Value>> changed;
        std::vector<std::string> invalidated;
        for (auto& n : names) {
            auto* p = iface->find_property(n);
            if (!p || p->emits_changed == "false" || p->emits_changed == "const") continue;
            if (p->emits_changed == "invalidates" || !p->get)
                invalidated.push_back(n);
            else
                changed.emplace_back(n, p->get());
        }
        if (changed.empty() && invalidated.empty()) return true;
        return emit_signal(path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                           {Value::str(interface), Value::vardict(std::move(changed)), Value::strings(invalidated)});
    });
}

}  // namespace brosys::dbus
