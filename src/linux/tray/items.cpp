// LinuxTrayHost: StatusNotifierItem tracking (properties, New* signals),
// dbusmenu layouts, and the host's interaction calls.
#include "linux/tray/host.h"

#include <chrono>

namespace brosys::tray {

namespace {

using dbus::Value;

constexpr int kInteractionTimeoutMs = 5000;
constexpr int kFirstFetchRetries = 5;

const char* event_name(MenuEventType t) {
    switch (t) {
        case MenuEventType::Clicked: return "clicked";
        case MenuEventType::Hovered: return "hovered";
        case MenuEventType::Opened: return "opened";
        case MenuEventType::Closed: return "closed";
    }
    return "clicked";
}

dbus::Args get_layout_args() { return {Value::i32(0), Value::i32(-1), Value::strings({})}; }

}  // namespace

// ---------------------------------------------------------------- bus thread

LinuxTrayHost::ItemState* LinuxTrayHost::find_item(const std::string& id, uint64_t epoch) {
    auto it = items_.find(id);
    if (it == items_.end() || (epoch && it->second.epoch != epoch)) return nullptr;
    return &it->second;
}

void LinuxTrayHost::item_appeared(const std::string& id) {
    if (stopping_ || items_.count(id)) return;
    ItemState st;
    if (!split_item_id(id, &st.service, &st.path)) return;
    st.owner = st.service[0] == ':' ? st.service : conn_->get_name_owner(st.service);
    if (st.owner.empty()) return;  // already gone
    dbus::Reply pid = conn_->call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                  "GetConnectionUnixProcessID", {Value::str(st.owner)});
    if (pid.ok && pid.first()) st.pid = static_cast<uint32_t>(pid.first()->as_uint());
    st.epoch = next_epoch_++;
    const uint64_t epoch = st.epoch;
    st.item_match = conn_->add_match(
        "type='signal',sender='" + st.owner + "',path='" + st.path + "',interface='" + kItemIface + "'",
        [this, id, epoch](const dbus::Message& m) {
            // NewTitle, NewIcon, NewAttentionIcon, NewOverlayIcon, NewToolTip,
            // NewStatus, NewIconThemePath, NewMenu (+ vendor extensions): refetch.
            if (!stopping_ && m.member.rfind("New", 0) == 0 && find_item(id, epoch)) fetch_item(id);
        });
    {
        std::lock_guard<std::mutex> lock(mu_);
        items_.emplace(id, std::move(st));
    }
    fetch_item(id);
}

void LinuxTrayHost::item_vanished(const std::string& id) {
    auto it = items_.find(id);
    if (it == items_.end()) return;
    if (it->second.item_match) conn_->remove_match(it->second.item_match);
    if (it->second.menu_match) conn_->remove_match(it->second.menu_match);
    bool announced = it->second.announced;
    {
        std::lock_guard<std::mutex> lock(mu_);
        items_.erase(it);
    }
    if (announced) events_.push(TrayItemRemoved{id});
}

void LinuxTrayHost::clear_items() {
    std::vector<std::string> ids;
    for (auto& [id, st] : items_) ids.push_back(id);
    for (auto& id : ids) item_vanished(id);
}

void LinuxTrayHost::fetch_item(const std::string& id) {
    ItemState* st = find_item(id, 0);
    if (!st) return;
    if (st->fetching) {
        st->refetch = true;
        return;
    }
    st->fetching = true;
    const uint64_t epoch = st->epoch;
    conn_->get_all_properties_async(st->service, st->path, kItemIface,
                                    [this, id, epoch](bool ok, std::map<std::string, Value> props, std::string) {
                                        if (!stopping_) apply_item(id, epoch, ok, props);
                                    });
}

void LinuxTrayHost::apply_item(const std::string& id, uint64_t epoch, bool ok,
                               const std::map<std::string, Value>& props) {
    ItemState* st = find_item(id, epoch);
    if (!st) return;
    st->fetching = false;
    if (!ok) {
        if (!st->announced) {
            // Some items register a moment before their object answers.
            if (st->retries++ < kFirstFetchRetries) {
                conn_->add_timer(std::chrono::milliseconds(100 * st->retries), [this, id, epoch] {
                    if (!stopping_ && find_item(id, epoch)) fetch_item(id);
                });
            } else {
                item_vanished(id);
            }
            return;
        }
    } else {
        ItemSnapshot snap = item_from_properties(props);
        snap.item.id = id;
        snap.item.pid = st->pid;
        bool was_announced = st->announced;
        uint32_t bits = was_announced ? item_changes(st->snap, snap) : 0;
        if (!was_announced || bits) {
            std::lock_guard<std::mutex> lock(mu_);
            st->snap = snap;
            st->announced = true;
        }
        if (!was_announced)
            events_.push(TrayItemAdded{snap.item});
        else if (bits)
            events_.push(TrayItemChanged{snap.item, bits});
        st = find_item(id, epoch);
        if (!st) return;
        if (snap.menu_path != st->menu_path) set_menu_path(id, snap.menu_path);
        st = find_item(id, epoch);
        if (!st) return;
    }
    if (st->refetch) {
        st->refetch = false;
        fetch_item(id);
    }
}

void LinuxTrayHost::set_menu_path(const std::string& id, const std::string& path) {
    ItemState* st = find_item(id, 0);
    if (!st) return;
    if (st->menu_match) conn_->remove_match(st->menu_match);
    {
        std::lock_guard<std::mutex> lock(mu_);
        st->menu_path = path;
        st->menu_match = 0;
        st->menu.reset();
        st->menu_fetching = st->menu_refetch = false;
    }
    if (path.empty()) return;
    const uint64_t epoch = st->epoch;
    st->menu_match = conn_->add_match(
        "type='signal',sender='" + st->owner + "',path='" + path + "',interface='" + kMenuIface + "'",
        [this, id, epoch, path](const dbus::Message& m) {
            if (stopping_ || (m.member != "LayoutUpdated" && m.member != "ItemsPropertiesUpdated")) return;
            ItemState* s = find_item(id, epoch);
            if (s && s->menu_path == path) fetch_menu(id);
        });
    fetch_menu(id);
}

void LinuxTrayHost::fetch_menu(const std::string& id) {
    ItemState* st = find_item(id, 0);
    if (!st || st->menu_path.empty()) return;
    if (st->menu_fetching) {
        st->menu_refetch = true;
        return;
    }
    st->menu_fetching = true;
    const uint64_t epoch = st->epoch;
    const std::string path = st->menu_path;
    conn_->call_async(st->service, path, kMenuIface, "GetLayout", get_layout_args(),
                      [this, id, epoch, path](dbus::Reply r) {
                          if (stopping_) return;
                          ItemState* s = find_item(id, epoch);
                          if (!s || s->menu_path != path) return;
                          s->menu_fetching = false;
                          store_menu(id, epoch, path, r);
                          s = find_item(id, epoch);
                          if (s && s->menu_refetch) {
                              s->menu_refetch = false;
                              fetch_menu(id);
                          }
                      });
}

void LinuxTrayHost::store_menu(const std::string& id, uint64_t epoch, const std::string& menu_path,
                               const dbus::Reply& r) {
    ItemState* st = find_item(id, epoch);
    if (!st || st->menu_path != menu_path || !r.ok || r.values.size() < 2) return;
    auto root = menu_from_layout(r.values[1]);
    if (!root || (st->menu && *st->menu == *root)) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        st->menu = *root;
    }
    events_.push(TrayMenuChanged{id, std::move(*root)});
}

// ---------------------------------------------------------------- host thread

std::vector<TrayItem> LinuxTrayHost::items() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<TrayItem> out;
    for (auto& [id, st] : items_)
        if (st.announced) out.push_back(st.snap.item);
    return out;
}

std::optional<MenuItem> LinuxTrayHost::menu(const std::string& item_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = items_.find(item_id);
    if (it == items_.end() || !it->second.announced) return std::nullopt;
    return it->second.menu;
}

bool LinuxTrayHost::locate(const std::string& id, Target* t, std::string* why) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = items_.find(id);
    if (it == items_.end() || !it->second.announced) {
        *why = "no tray item " + id;
        return false;
    }
    t->service = it->second.service;
    t->path = it->second.path;
    t->menu_path = it->second.menu_path;
    t->epoch = it->second.epoch;
    return true;
}

Result LinuxTrayHost::call_item(const std::string& id, const char* method, const dbus::Args& args) {
    Target t;
    std::string why;
    if (!locate(id, &t, &why)) return Result::failure(why);
    dbus::Reply r = conn_->call(t.service, t.path, kItemIface, method, args, kInteractionTimeoutMs);
    return r.ok ? Result::success() : Result::failure(r.error());
}

Result LinuxTrayHost::activate(const std::string& item_id, int32_t x, int32_t y) {
    return call_item(item_id, "Activate", {Value::i32(x), Value::i32(y)});
}

Result LinuxTrayHost::secondary_activate(const std::string& item_id, int32_t x, int32_t y) {
    return call_item(item_id, "SecondaryActivate", {Value::i32(x), Value::i32(y)});
}

Result LinuxTrayHost::context_menu(const std::string& item_id, int32_t x, int32_t y) {
    return call_item(item_id, "ContextMenu", {Value::i32(x), Value::i32(y)});
}

Result LinuxTrayHost::scroll(const std::string& item_id, int32_t delta, ScrollOrientation orientation) {
    return call_item(item_id, "Scroll",
                     {Value::i32(delta), Value::str(orientation == ScrollOrientation::Horizontal ? "horizontal" : "vertical")});
}

Result LinuxTrayHost::menu_about_to_show(const std::string& item_id, int32_t menu_item_id) {
    Target t;
    std::string why;
    if (!locate(item_id, &t, &why)) return Result::failure(why);
    if (t.menu_path.empty()) return Result::failure("tray item " + item_id + " has no menu");
    dbus::Reply r = conn_->call(t.service, t.menu_path, kMenuIface, "AboutToShow", {Value::i32(menu_item_id)},
                                kInteractionTimeoutMs);
    if (!r.ok) return Result::failure(r.error());
    if (r.first() && r.first()->as_bool(false)) {
        // The item says the layout needs updating: refresh before returning,
        // so menu() is current (TrayMenuChanged follows when it differs).
        dbus::Reply layout = conn_->call(t.service, t.menu_path, kMenuIface, "GetLayout", get_layout_args(),
                                         kInteractionTimeoutMs);
        if (!layout.ok) return Result::failure(layout.error());
        conn_->run_sync([&] { store_menu(item_id, t.epoch, t.menu_path, layout); });
    }
    return Result::success();
}

Result LinuxTrayHost::menu_event(const std::string& item_id, int32_t menu_item_id, MenuEventType type) {
    Target t;
    std::string why;
    if (!locate(item_id, &t, &why)) return Result::failure(why);
    if (t.menu_path.empty()) return Result::failure("tray item " + item_id + " has no menu");
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    dbus::Reply r = conn_->call(t.service, t.menu_path, kMenuIface, "Event",
                                {Value::i32(menu_item_id), Value::str(event_name(type)), Value::variant(Value::i32(0)),
                                 Value::u32(static_cast<uint32_t>(ms))},
                                kInteractionTimeoutMs);
    return r.ok ? Result::success() : Result::failure(r.error());
}

}  // namespace brosys::tray
