// Linux AudioService: a PipeWire client on its own pw_thread_loop.
//
// The registry binds audio sink / source nodes, Audio/Device objects (for
// their active Routes) and the "default" metadata; their info / params /
// properties are mirrored into a pw::Graph on the loop thread. Each object
// is "settled" once a core sync issued after its info (and param
// subscription) completes, so devices appear complete, never half-read.
// Changes signal a loop event; its handler (once per loop iteration) builds
// an AudioState, diffs it against the last published one and queues the
// events. Mutations take the loop lock and set params the way wpctl does.
//
// When the server goes away every device is removed; a loop timer then
// reconnects (50 ms doubling to 2 s) to the same remote, and the registry
// fills the graph again, reported as ordinary additions.
#include "brosys/audio.h"
#include "linux/audio/pw_model.h"
#include "linux/audio/pw_pod.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>

namespace brosys {

namespace {

constexpr auto kInitialTimeout = std::chrono::milliseconds(5000);

class LinuxAudioService;

enum class Kind { Node, Device, Metadata };

// One bound registry object: its proxy, listeners and settle state.
struct Bound {
    LinuxAudioService* self = nullptr;
    uint32_t id = 0;
    Kind kind = Kind::Node;
    pw_proxy* proxy = nullptr;
    spa_hook object_hook{};
    bool subscribed = false;
    int sync_seq = -1;
    bool settled = false;
};

pw::Dict to_dict(const spa_dict* d) {
    pw::Dict out;
    if (!d) return out;
    for (uint32_t i = 0; i < d->n_items; ++i)
        if (d->items[i].key && d->items[i].value) out[d->items[i].key] = d->items[i].value;
    return out;
}

bool has_param(const spa_param_info* params, uint32_t n, uint32_t id) {
    for (uint32_t i = 0; i < n; ++i)
        if (params[i].id == id && (params[i].flags & SPA_PARAM_INFO_READ)) return true;
    return false;
}

class LinuxAudioService final : public AudioService {
public:
    ~LinuxAudioService() override { shutdown(); }

    bool start(const AudioConfig& config, std::string* error);

    AudioEventQueue& events() override { return queue_; }

    AudioState state() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return state_;
    }

    Result set_volume(const std::string& id, float volume) override;
    Result set_mute(const std::string& id, bool muted) override;
    Result set_default(const std::string& id) override;

    // ---- PipeWire callbacks (loop thread, loop lock held)
    void on_global(uint32_t id, const char* type, const spa_dict* props);
    void on_global_remove(uint32_t id);
    void on_node_info(Bound* b, const pw_node_info* info);
    void on_node_param(Bound* b, uint32_t param_id, uint32_t index, const spa_pod* param);
    void on_device_info(Bound* b, const pw_device_info* info);
    void on_device_param(Bound* b, uint32_t param_id, uint32_t index, const spa_pod* param);
    void on_metadata_property(uint32_t subject, const char* key, const char* value);
    void on_done(uint32_t id, int seq);
    void on_error(uint32_t id, int res, const char* message);
    void on_publish();
    void on_reconnect_timer();

private:
    void shutdown();
    // Loop lock held. connect_core: core + registry + listeners ("" or why not).
    std::string connect_core();
    void disconnect_core();
    void arm_reconnect(std::chrono::milliseconds delay);
    void bind(uint32_t id, Kind kind, const char* type, uint32_t version, const pw::Dict& props);
    void destroy_bound(Bound& b);
    void settle_later(Bound* b);
    void check_initial();
    void schedule_publish();
    // Loop lock held: the ready node named `id` and its binding.
    pw::Node* find_node(const std::string& id, Bound** bound);
    Bound* find_bound(uint32_t id, Kind kind);

    AudioConfig config_;
    AudioEventQueue queue_;
    mutable std::mutex mu_;
    AudioState state_;  // published snapshot, guarded by mu_

    // Loop-thread state (or loop lock held).
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    spa_hook core_hook_{};
    spa_hook registry_hook_{};
    spa_source* publish_event_ = nullptr;
    spa_source* reconnect_timer_ = nullptr;
    std::chrono::milliseconds reconnect_delay_{50};
    std::map<uint32_t, std::unique_ptr<Bound>> bound_;
    pw::Graph graph_;
    AudioState published_;
    int initial_seq_ = -1;
    bool registry_done_ = false;
    bool settled_ = false;      // the initial registry + every object settled
    bool initial_done_ = false;  // create() queued the initial events: diffs may follow
    bool connected_ = false;
    std::string fatal_;
};

// ---------------------------------------------------------------- C trampolines

void registry_global(void* data, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props) {
    static_cast<LinuxAudioService*>(data)->on_global(id, type, props);
}
void registry_global_remove(void* data, uint32_t id) { static_cast<LinuxAudioService*>(data)->on_global_remove(id); }
const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

void node_info(void* data, const pw_node_info* info) {
    auto* b = static_cast<Bound*>(data);
    b->self->on_node_info(b, info);
}
void node_param(void* data, int, uint32_t id, uint32_t index, uint32_t, const spa_pod* param) {
    auto* b = static_cast<Bound*>(data);
    b->self->on_node_param(b, id, index, param);
}
const pw_node_events kNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = node_info,
    .param = node_param,
};

void device_info(void* data, const pw_device_info* info) {
    auto* b = static_cast<Bound*>(data);
    b->self->on_device_info(b, info);
}
void device_param(void* data, int, uint32_t id, uint32_t index, uint32_t, const spa_pod* param) {
    auto* b = static_cast<Bound*>(data);
    b->self->on_device_param(b, id, index, param);
}
const pw_device_events kDeviceEvents = {
    .version = PW_VERSION_DEVICE_EVENTS,
    .info = device_info,
    .param = device_param,
};

int metadata_property(void* data, uint32_t subject, const char* key, const char*, const char* value) {
    static_cast<Bound*>(data)->self->on_metadata_property(subject, key, value);
    return 0;
}
const pw_metadata_events kMetadataEvents = {
    .version = PW_VERSION_METADATA_EVENTS,
    .property = metadata_property,
};

void core_done(void* data, uint32_t id, int seq) { static_cast<LinuxAudioService*>(data)->on_done(id, seq); }
void core_error(void* data, uint32_t id, int, int res, const char* message) {
    static_cast<LinuxAudioService*>(data)->on_error(id, res, message);
}
const pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .done = core_done,
    .error = core_error,
};

void publish_event(void* data, uint64_t) { static_cast<LinuxAudioService*>(data)->on_publish(); }
void reconnect_timer(void* data, uint64_t) { static_cast<LinuxAudioService*>(data)->on_reconnect_timer(); }

// ---------------------------------------------------------------- lifecycle

bool LinuxAudioService::start(const AudioConfig& config, std::string* error) {
    static std::once_flag init_once;
    std::call_once(init_once, [] { pw_init(nullptr, nullptr); });
    config_ = config;

    loop_ = pw_thread_loop_new("brosys-audio", nullptr);
    if (!loop_) {
        if (error) *error = std::string("pw_thread_loop_new: ") + std::strerror(errno);
        return false;
    }
    if (pw_thread_loop_start(loop_) < 0) {
        if (error) *error = "cannot start the PipeWire loop thread";
        return false;
    }
    pw_thread_loop_lock(loop_);
    // A control client: it needs the native protocol and the metadata
    // interface only. client.conf's other modules are switched off through
    // their conditions (no RTKit / D-Bus connection for real-time threads,
    // no node / device export).
    pw_properties* ctx_props = pw_properties_new("module.rt", "false", "module.client-node", "false",
                                                 "module.client-device", "false", "module.adapter", "false",
                                                 "module.session-manager", "false", nullptr);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), ctx_props, 0);
    if (!context_) {
        pw_thread_loop_unlock(loop_);
        if (error) *error = std::string("pw_context_new: ") + std::strerror(errno);
        return false;
    }
    publish_event_ = pw_loop_add_event(pw_thread_loop_get_loop(loop_), publish_event, this);
    reconnect_timer_ = pw_loop_add_timer(pw_thread_loop_get_loop(loop_), reconnect_timer, this);
    std::string why = connect_core();
    if (!why.empty()) {
        pw_thread_loop_unlock(loop_);
        if (error) *error = why;
        return false;
    }
    initial_seq_ = pw_core_sync(core_, PW_ID_CORE, 0);

    auto deadline = std::chrono::steady_clock::now() + kInitialTimeout;
    while (!settled_ && fatal_.empty() && std::chrono::steady_clock::now() < deadline)
        pw_thread_loop_timed_wait(loop_, 1);
    if (!fatal_.empty()) {
        pw_thread_loop_unlock(loop_);
        if (error) *error = "PipeWire: " + fatal_;
        return false;
    }
    initial_done_ = true;  // on a timeout, publish what has settled; the rest follows as events

    published_ = pw::build_state(graph_);
    for (auto& e : pw::initial_events(published_)) queue_.push(std::move(e));
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_ = published_;
    }
    pw_thread_loop_unlock(loop_);
    return true;
}

std::string LinuxAudioService::connect_core() {
    pw_properties* props = pw_properties_new(PW_KEY_APP_NAME, "brosys", nullptr);
    if (!config_.pipewire_remote.empty()) pw_properties_set(props, PW_KEY_REMOTE_NAME, config_.pipewire_remote.c_str());
    core_ = pw_context_connect(context_, props, 0);
    if (!core_) {
        int e = errno;
        return "cannot connect to PipeWire" +
               (config_.pipewire_remote.empty() ? std::string() : " remote '" + config_.pipewire_remote + "'") + ": " +
               std::strerror(e);
    }
    connected_ = true;
    pw_core_add_listener(core_, &core_hook_, &kCoreEvents, this);
    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(registry_, &registry_hook_, &kRegistryEvents, this);
    return {};
}

void LinuxAudioService::disconnect_core() {
    for (auto& [id, b] : bound_) destroy_bound(*b);
    bound_.clear();
    if (registry_) {
        spa_hook_remove(&registry_hook_);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
        registry_ = nullptr;
    }
    if (core_) {
        spa_hook_remove(&core_hook_);
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    connected_ = false;
}

void LinuxAudioService::arm_reconnect(std::chrono::milliseconds delay) {
    if (!reconnect_timer_) return;
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(delay.count() / 1000);
    ts.tv_nsec = static_cast<long>((delay.count() % 1000) * 1000000);
    pw_loop_update_timer(pw_thread_loop_get_loop(loop_), reconnect_timer_, &ts, nullptr, false);
}

// Loop thread: the old connection is torn down (outside its own callbacks)
// and a new one tried; the registry repopulates the graph from scratch.
void LinuxAudioService::on_reconnect_timer() {
    if (connected_) return;
    disconnect_core();
    if (!connect_core().empty()) {
        reconnect_delay_ = std::min(reconnect_delay_ * 2, std::chrono::milliseconds(2000));
        arm_reconnect(reconnect_delay_);
        return;
    }
    reconnect_delay_ = std::chrono::milliseconds(50);
    pw_core_sync(core_, PW_ID_CORE, 0);
}

void LinuxAudioService::shutdown() {
    if (!loop_) return;
    pw_thread_loop_lock(loop_);
    disconnect_core();
    if (reconnect_timer_) {
        pw_loop_destroy_source(pw_thread_loop_get_loop(loop_), reconnect_timer_);
        reconnect_timer_ = nullptr;
    }
    if (publish_event_) {
        pw_loop_destroy_source(pw_thread_loop_get_loop(loop_), publish_event_);
        publish_event_ = nullptr;
    }
    pw_thread_loop_unlock(loop_);
    pw_thread_loop_stop(loop_);
    if (context_) pw_context_destroy(context_);
    context_ = nullptr;
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
}

// ---------------------------------------------------------------- registry

void LinuxAudioService::on_global(uint32_t id, const char* type, const spa_dict* props) {
    if (!type) return;
    pw::Dict d = to_dict(props);
    std::string t = type;
    if (t == PW_TYPE_INTERFACE_Node) {
        auto it = d.find(PW_KEY_MEDIA_CLASS);
        if (it == d.end() || !pw::direction_of(it->second, d)) return;
        graph_.nodes[id] = pw::Node{id, d};
        bind(id, Kind::Node, type, PW_VERSION_NODE, d);
    } else if (t == PW_TYPE_INTERFACE_Device) {
        auto it = d.find(PW_KEY_MEDIA_CLASS);
        if (it == d.end() || it->second != "Audio/Device") return;
        graph_.devices[id] = pw::Device{id, d};
        bind(id, Kind::Device, type, PW_VERSION_DEVICE, d);
    } else if (t == PW_TYPE_INTERFACE_Metadata) {
        auto it = d.find(PW_KEY_METADATA_NAME);
        if (it == d.end() || it->second != "default") return;
        bind(id, Kind::Metadata, type, PW_VERSION_METADATA, d);
    }
}

void LinuxAudioService::bind(uint32_t id, Kind kind, const char* type, uint32_t version, const pw::Dict&) {
    auto proxy = static_cast<pw_proxy*>(pw_registry_bind(registry_, id, type, version, 0));
    if (!proxy) return;
    auto b = std::make_unique<Bound>();
    b->self = this;
    b->id = id;
    b->kind = kind;
    b->proxy = proxy;
    switch (kind) {
        case Kind::Node:
            pw_node_add_listener(reinterpret_cast<pw_node*>(proxy), &b->object_hook, &kNodeEvents, b.get());
            break;
        case Kind::Device:
            pw_device_add_listener(reinterpret_cast<pw_device*>(proxy), &b->object_hook, &kDeviceEvents, b.get());
            break;
        case Kind::Metadata:
            pw_metadata_add_listener(reinterpret_cast<pw_metadata*>(proxy), &b->object_hook, &kMetadataEvents,
                                     b.get());
            settle_later(b.get());  // its properties arrive right after the bind
            break;
    }
    auto& slot = bound_[id];
    if (slot) destroy_bound(*slot);
    slot = std::move(b);
}

void LinuxAudioService::destroy_bound(Bound& b) {
    if (!b.proxy) return;
    spa_hook_remove(&b.object_hook);
    pw_proxy_destroy(b.proxy);
    b.proxy = nullptr;
}

void LinuxAudioService::on_global_remove(uint32_t id) {
    auto it = bound_.find(id);
    if (it == bound_.end()) return;
    if (it->second->kind == Kind::Metadata) {
        graph_.default_sink.clear();
        graph_.default_source.clear();
    }
    destroy_bound(*it->second);
    bound_.erase(it);
    graph_.nodes.erase(id);
    graph_.devices.erase(id);
    check_initial();
    schedule_publish();
}

// Marks `b` settled once everything requested so far has been answered.
void LinuxAudioService::settle_later(Bound* b) {
    if (!core_) return;
    b->sync_seq = pw_core_sync(core_, PW_ID_CORE, 0);
}

// ---------------------------------------------------------------- objects

void LinuxAudioService::on_node_info(Bound* b, const pw_node_info* info) {
    auto it = graph_.nodes.find(b->id);
    if (it == graph_.nodes.end()) return;
    if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) it->second.props = to_dict(info->props);
    if (!b->subscribed && has_param(info->params, info->n_params, SPA_PARAM_Props)) {
        uint32_t ids[] = {SPA_PARAM_Props};
        pw_node_subscribe_params(reinterpret_cast<pw_node*>(b->proxy), ids, 1);
        b->subscribed = true;
    }
    if (!b->settled && b->sync_seq < 0) settle_later(b);
    schedule_publish();
}

void LinuxAudioService::on_node_param(Bound* b, uint32_t param_id, uint32_t, const spa_pod* param) {
    if (param_id != SPA_PARAM_Props) return;
    auto it = graph_.nodes.find(b->id);
    if (it == graph_.nodes.end()) return;
    pw::PropsValues v;
    if (!pw::parse_props(param, &v)) return;
    pw::Node& n = it->second;
    // Partial Props objects carry only some keys: keep what they leave out.
    if (v.has_volume) {
        n.has_volume = true;
        n.volumes = std::move(v.volumes);
    }
    if (v.has_mute) {
        n.has_mute = true;
        n.mute = v.mute;
    }
    schedule_publish();
}

void LinuxAudioService::on_device_info(Bound* b, const pw_device_info* info) {
    auto it = graph_.devices.find(b->id);
    if (it == graph_.devices.end()) return;
    if (info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS) it->second.props = to_dict(info->props);
    if (!b->subscribed && has_param(info->params, info->n_params, SPA_PARAM_Route)) {
        uint32_t ids[] = {SPA_PARAM_Route};
        pw_device_subscribe_params(reinterpret_cast<pw_device*>(b->proxy), ids, 1);
        b->subscribed = true;
    }
    if (!b->settled && b->sync_seq < 0) settle_later(b);
    schedule_publish();
}

void LinuxAudioService::on_device_param(Bound* b, uint32_t param_id, uint32_t index, const spa_pod* param) {
    if (param_id != SPA_PARAM_Route) return;
    auto it = graph_.devices.find(b->id);
    if (it == graph_.devices.end()) return;
    pw::Route r;
    if (!pw::parse_route(param, &r)) return;
    auto& routes = it->second.routes;
    if (index == 0) routes.clear();  // a new enumeration of the active routes starts
    auto same = std::find_if(routes.begin(), routes.end(),
                             [&](const pw::Route& x) { return x.index == r.index && x.device == r.device; });
    if (same != routes.end())
        *same = std::move(r);
    else
        routes.push_back(std::move(r));
    schedule_publish();
}

void LinuxAudioService::on_metadata_property(uint32_t subject, const char* key, const char* value) {
    if (subject != PW_ID_CORE) return;
    if (!key) {  // everything cleared
        graph_.default_sink.clear();
        graph_.default_source.clear();
    } else if (std::strcmp(key, "default.audio.sink") == 0) {
        graph_.default_sink = value ? pw::parse_default_name(value) : std::string();
    } else if (std::strcmp(key, "default.audio.source") == 0) {
        graph_.default_source = value ? pw::parse_default_name(value) : std::string();
    } else {
        return;
    }
    schedule_publish();
}

void LinuxAudioService::on_done(uint32_t id, int seq) {
    if (id != PW_ID_CORE) return;
    if (seq == initial_seq_) registry_done_ = true;
    for (auto& [bid, b] : bound_) {
        if (b->settled || b->sync_seq != seq) continue;
        b->settled = true;
        if (auto n = graph_.nodes.find(bid); n != graph_.nodes.end()) n->second.settled = true;
        if (auto d = graph_.devices.find(bid); d != graph_.devices.end()) d->second.settled = true;
    }
    check_initial();
    schedule_publish();
}

void LinuxAudioService::on_error(uint32_t id, int res, const char* message) {
    if (id != PW_ID_CORE) return;  // per-object errors (a refused set_param) are not fatal
    if (res != -EPIPE) return;
    std::string why = message ? message : spa_strerror(res);
    connected_ = false;
    if (!initial_done_) {
        fatal_ = why.empty() ? "disconnected" : why;
        pw_thread_loop_signal(loop_, false);
        return;
    }
    // The server went away: everything is gone until it is back. The proxies
    // are released from the reconnect timer, not inside the core's own callback.
    graph_ = pw::Graph();
    schedule_publish();
    reconnect_delay_ = std::chrono::milliseconds(50);
    arm_reconnect(reconnect_delay_);
}

void LinuxAudioService::check_initial() {
    if (settled_ || !registry_done_) return;
    for (auto& [id, b] : bound_)
        if (!b->settled) return;
    settled_ = true;
    pw_thread_loop_signal(loop_, false);
}

void LinuxAudioService::schedule_publish() {
    if (publish_event_ && loop_) pw_loop_signal_event(pw_thread_loop_get_loop(loop_), publish_event_);
}

void LinuxAudioService::on_publish() {
    if (!initial_done_) return;
    AudioState now = pw::build_state(graph_);
    if (now == published_) return;
    auto events = pw::diff(published_, now);
    published_ = now;
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_ = std::move(now);
    }
    for (auto& e : events) queue_.push(std::move(e));
}

// ---------------------------------------------------------------- mutations

pw::Node* LinuxAudioService::find_node(const std::string& id, Bound** bound) {
    for (auto& [nid, n] : graph_.nodes) {
        auto name = n.props.find(PW_KEY_NODE_NAME);
        if (name == n.props.end() || name->second != id || !pw::node_ready(graph_, n)) continue;
        auto b = bound_.find(nid);
        if (b == bound_.end() || !b->second->proxy) continue;
        if (bound) *bound = b->second.get();
        return &n;
    }
    return nullptr;
}

Bound* LinuxAudioService::find_bound(uint32_t id, Kind kind) {
    auto it = bound_.find(id);
    return it != bound_.end() && it->second->kind == kind && it->second->proxy ? it->second.get() : nullptr;
}

namespace {

struct LoopLock {
    explicit LoopLock(pw_thread_loop* l) : loop(l) { pw_thread_loop_lock(loop); }
    ~LoopLock() { pw_thread_loop_unlock(loop); }
    pw_thread_loop* loop;
};

// The device binding behind a route-backed node.
uint32_t device_id_of(const pw::Node& n) {
    auto it = n.props.find(PW_KEY_DEVICE_ID);
    if (it == n.props.end()) return SPA_ID_INVALID;
    try {
        return static_cast<uint32_t>(std::stoul(it->second));
    } catch (...) {
        return SPA_ID_INVALID;
    }
}

}  // namespace

Result LinuxAudioService::set_volume(const std::string& id, float volume) {
    LoopLock lock(loop_);
    if (!connected_) return Result::failure("PipeWire connection lost");
    Bound* nb = nullptr;
    pw::Node* n = find_node(id, &nb);
    if (!n) return Result::failure("no such audio device: " + id);
    float ui = std::clamp(volume, 0.0f, std::max(0.0f, config_.max_volume));
    float linear = pw::ui_to_linear(ui);
    uint8_t buf[4096];
    spa_pod_builder b;
    spa_pod_builder_init(&b, buf, sizeof buf);

    const pw::Route* route = pw::route_for_node(graph_, *n);
    Bound* db = route ? find_bound(device_id_of(*n), Kind::Device) : nullptr;
    if (route && db && (route->has_volume || !n->has_volume)) {
        size_t channels = route->volumes.empty() ? n->volumes.size() : route->volumes.size();
        if (channels == 0) return Result::failure("the device route has no volume control");
        std::vector<float> vols(channels, linear);
        const spa_pod* pod = pw::build_route(&b, *route, &vols, std::nullopt);
        int r = pw_device_set_param(reinterpret_cast<pw_device*>(db->proxy), SPA_PARAM_Route, 0, pod);
        return r < 0 ? Result::failure(std::string("set route volume: ") + spa_strerror(r)) : Result::success();
    }
    if (!n->has_volume || n->volumes.empty()) return Result::failure("the device has no volume control: " + id);
    std::vector<float> vols(n->volumes.size(), linear);
    const spa_pod* pod = pw::build_props(&b, &vols, std::nullopt);
    int r = pw_node_set_param(reinterpret_cast<pw_node*>(nb->proxy), SPA_PARAM_Props, 0, pod);
    return r < 0 ? Result::failure(std::string("set node volume: ") + spa_strerror(r)) : Result::success();
}

Result LinuxAudioService::set_mute(const std::string& id, bool muted) {
    LoopLock lock(loop_);
    if (!connected_) return Result::failure("PipeWire connection lost");
    Bound* nb = nullptr;
    pw::Node* n = find_node(id, &nb);
    if (!n) return Result::failure("no such audio device: " + id);
    uint8_t buf[1024];
    spa_pod_builder b;
    spa_pod_builder_init(&b, buf, sizeof buf);

    const pw::Route* route = pw::route_for_node(graph_, *n);
    Bound* db = route ? find_bound(device_id_of(*n), Kind::Device) : nullptr;
    if (route && db && (route->has_mute || !n->has_mute)) {
        const spa_pod* pod = pw::build_route(&b, *route, nullptr, muted);
        int r = pw_device_set_param(reinterpret_cast<pw_device*>(db->proxy), SPA_PARAM_Route, 0, pod);
        return r < 0 ? Result::failure(std::string("set route mute: ") + spa_strerror(r)) : Result::success();
    }
    if (!n->has_mute) return Result::failure("the device has no mute control: " + id);
    const spa_pod* pod = pw::build_props(&b, nullptr, muted);
    int r = pw_node_set_param(reinterpret_cast<pw_node*>(nb->proxy), SPA_PARAM_Props, 0, pod);
    return r < 0 ? Result::failure(std::string("set node mute: ") + spa_strerror(r)) : Result::success();
}

Result LinuxAudioService::set_default(const std::string& id) {
    LoopLock lock(loop_);
    if (!connected_) return Result::failure("PipeWire connection lost");
    pw::Node* n = find_node(id, nullptr);
    if (!n) return Result::failure("no such audio device: " + id);
    auto cls = n->props.find(PW_KEY_MEDIA_CLASS);
    auto dir = pw::direction_of(cls == n->props.end() ? std::string() : cls->second, n->props);
    if (!dir) return Result::failure("not an audio device: " + id);
    Bound* meta = nullptr;
    for (auto& [bid, b] : bound_)
        if (b->kind == Kind::Metadata && b->proxy) meta = b.get();
    if (!meta) return Result::failure("no \"default\" metadata (is a session manager such as WirePlumber running?)");
    const char* key = *dir == AudioDirection::Output ? "default.configured.audio.sink" : "default.configured.audio.source";
    std::string json = pw::default_json(id);
    int r = pw_metadata_set_property(reinterpret_cast<pw_metadata*>(meta->proxy), PW_ID_CORE, key, "Spa:String:JSON",
                                     json.c_str());
    return r < 0 ? Result::failure(std::string("set default: ") + spa_strerror(r)) : Result::success();
}

}  // namespace

std::unique_ptr<AudioService> AudioService::create(const AudioConfig& config, std::string* error) {
    auto s = std::make_unique<LinuxAudioService>();
    if (!s->start(config, error)) return nullptr;
    return s;
}

}  // namespace brosys
