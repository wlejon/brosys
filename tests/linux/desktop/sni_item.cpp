// A real StatusNotifierItem client process for the tray tests: exports
// org.kde.StatusNotifierItem (or, with --flavor freedesktop,
// org.freedesktop.StatusNotifierItem, registering with
// org.freedesktop.StatusNotifierWatcher) plus a com.canonical.dbusmenu
// menu, registers with whichever process owns the watcher name (again
// whenever that owner changes, and after the bus daemon restarts, as real
// items do), changes itself on command through org.brosys.TestItem at
// /control, and prints every interaction it receives:
//
//   READY <unique name> <item id>      REGISTERED <watcher> | REGISTER-FAILED <error>
//   ACTIVATE x y   SECONDARY x y   CONTEXT x y   SCROLL delta orientation
//   EVENT id type <sig>:<data> <timestamp>   ABOUTTOSHOW id   RECONNECTED <unique name> <item id>
//
// --broken registers without exporting the item object (it never answers);
// --watcher picks the watcher flavour independently of --flavor (after it).
//
// usage: brosys_sni_item --bus ADDRESS [--mode path|name] [--flavor kde|freedesktop]
//                        [--watcher kde|freedesktop] [--broken 1] [--id ID] [--title TITLE]
#include "linux/dbus/connection.h"

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unistd.h>

using namespace brosys::dbus;

namespace {

const char* kItemIface = "org.kde.StatusNotifierItem";
constexpr const char* kMenuIface = "com.canonical.dbusmenu";
constexpr const char* kMenuPath = "/MenuBar";
const char* kWatcher = "org.kde.StatusNotifierWatcher";  // the bus name and the interface

void say(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

struct MenuNode {
    std::vector<std::pair<std::string, Value>> props;
    std::vector<int32_t> children;
};

struct Item {
    Connection* conn = nullptr;
    std::string item_path, register_arg;
    // Bus-thread state.
    std::string id = "brosys-test-item", title = "Test Item", status = "Active", icon = "mail-unread";
    std::string attention = "mail-attention", overlay, tip_title = "Tip title", tip_body = "Tip <b>body</b>";
    std::map<int32_t, MenuNode> menu;
    uint32_t revision = 1;
    int32_t next_menu_id = 100;
    std::string dirty_label;  // non-empty: the next AboutToShow relabels item 1 and asks for a refresh

    std::mutex quit_mu;
    std::condition_variable quit_cv;
    bool quit = false;

    void build_menu() {
        menu[0] = {{{"children-display", Value::str("submenu")}}, {1, 2, 3, 4, 6, 7}};
        menu[1] = {{{"label", Value::str("_Open")},
                    {"icon-name", Value::str("document-open")},
                    {"shortcut", Value::array("as", {Value::strings({"Control", "o"})})}},
                   {}};
        menu[2] = {{{"type", Value::str("separator")}}, {}};
        menu[3] = {{{"label", Value::str("Check")}, {"toggle-type", Value::str("checkmark")}, {"toggle-state", Value::i32(1)}},
                   {}};
        menu[4] = {{{"label", Value::str("More")}, {"children-display", Value::str("submenu")}}, {5}};
        menu[5] = {{{"label", Value::str("Deep")}, {"enabled", Value::boolean(false)}}, {}};
        menu[6] = {{{"label", Value::str("Hidden")}, {"visible", Value::boolean(false)}}, {}};
        menu[7] = {{{"label", Value::str("Icon")}, {"icon-data", Value::bytes({0x89, 'P', 'N', 'G', 13, 10, 26, 10})}}, {}};
    }

    Value layout(int32_t id, int32_t depth) {
        auto& n = menu[id];
        Value::Items kids;
        if (depth != 0)
            for (int32_t c : n.children) kids.push_back(Value::variant(layout(c, depth < 0 ? -1 : depth - 1)));
        return Value::structure({Value::i32(id), Value::vardict(n.props), Value::array("v", std::move(kids))});
    }

    void set_prop(int32_t id, const std::string& key, Value v) {
        for (auto& [k, val] : menu[id].props)
            if (k == key) {
                val = std::move(v);
                return;
            }
        menu[id].props.emplace_back(key, std::move(v));
    }

    void emit(const char* member, Args args = Args()) { conn->emit_signal(item_path, kItemIface, member, args); }

    void register_with(const std::string& owner) {
        conn->call_async(owner, "/StatusNotifierWatcher", kWatcher, "RegisterStatusNotifierItem",
                         {Value::str(register_arg)}, [owner](Reply r) {
                             say(r.ok ? "REGISTERED " + owner : "REGISTER-FAILED " + r.error());
                         });
    }

    std::shared_ptr<Interface> item_interface();
    std::shared_ptr<Interface> menu_interface();
    std::shared_ptr<Interface> control_interface();
};

Value pixmaps() {
    // One 2x1 ARGB32 pixmap: opaque red, half-transparent blue.
    return Value::array("(iiay)", {Value::structure({Value::i32(2), Value::i32(1),
                                                     Value::bytes({0xFF, 0xFF, 0, 0, 0x80, 0, 0, 0xFF})})});
}

std::shared_ptr<Interface> Item::item_interface() {
    auto i = std::make_shared<Interface>();
    i->name = kItemIface;
    auto ro = [&](const char* name, const char* sig, std::function<Value()> get) {
        i->properties.push_back({name, sig, std::move(get), nullptr, "true"});
    };
    ro("Category", "s", [] { return Value::str("Communications"); });
    ro("Id", "s", [this] { return Value::str(id); });
    ro("Title", "s", [this] { return Value::str(title); });
    ro("Status", "s", [this] { return Value::str(status); });
    ro("WindowId", "i", [] { return Value::i32(42); });
    ro("IconName", "s", [this] { return Value::str(icon); });
    ro("IconPixmap", "a(iiay)", [] { return pixmaps(); });
    ro("OverlayIconName", "s", [this] { return Value::str(overlay); });
    ro("OverlayIconPixmap", "a(iiay)", [] { return Value::array("(iiay)", {}); });
    ro("AttentionIconName", "s", [this] { return Value::str(attention); });
    ro("AttentionIconPixmap", "a(iiay)", [] { return Value::array("(iiay)", {}); });
    ro("AttentionMovieName", "s", [] { return Value::str(""); });
    ro("ToolTip", "(sa(iiay)ss)", [this] {
        return Value::structure({Value::str("tip-icon"), Value::array("(iiay)", {}), Value::str(tip_title), Value::str(tip_body)});
    });
    ro("ItemIsMenu", "b", [] { return Value::boolean(false); });
    ro("Menu", "o", [] { return Value::obj(kMenuPath); });
    ro("IconThemePath", "s", [] { return Value::str("/tmp/brosys-icons"); });
    auto click = [&](const char* name, const char* tag) {
        i->methods.push_back({name, "ii", "", {"x", "y"}, {}, [tag](const MethodCall& c) {
                                  say(std::string(tag) + " " + std::to_string(c.args[0].as_int()) + " " +
                                      std::to_string(c.args[1].as_int()));
                                  return MethodResult::ok();
                              }});
    };
    click("Activate", "ACTIVATE");
    click("SecondaryActivate", "SECONDARY");
    click("ContextMenu", "CONTEXT");
    i->methods.push_back({"Scroll", "is", "", {"delta", "orientation"}, {}, [](const MethodCall& c) {
                              say("SCROLL " + std::to_string(c.args[0].as_int()) + " " + c.args[1].as_string());
                              return MethodResult::ok();
                          }});
    for (const char* s : {"NewTitle", "NewIcon", "NewAttentionIcon", "NewOverlayIcon", "NewToolTip"})
        i->signals.push_back({s, "", {}});
    i->signals.push_back({"NewStatus", "s", {"status"}});
    i->signals.push_back({"NewIconThemePath", "s", {"icon_theme_path"}});
    return i;
}

std::shared_ptr<Interface> Item::menu_interface() {
    auto i = std::make_shared<Interface>();
    i->name = kMenuIface;
    i->methods.push_back({"GetLayout", "iias", "u(ia{sv}av)", {"parentId", "recursionDepth", "propertyNames"},
                          {"revision", "layout"}, [this](const MethodCall& c) {
                              int32_t parent = static_cast<int32_t>(c.args[0].as_int());
                              if (!menu.count(parent)) return MethodResult::error(kErrorInvalidArgs, "no such item");
                              return MethodResult::ok({Value::u32(revision),
                                                       layout(parent, static_cast<int32_t>(c.args[1].as_int()))});
                          }});
    i->methods.push_back({"GetGroupProperties", "aias", "a(ia{sv})", {}, {}, [this](const MethodCall& c) {
                              Value::Items out;
                              for (auto& idv : c.args[0].items()) {
                                  int32_t id = static_cast<int32_t>(idv.as_int());
                                  if (menu.count(id))
                                      out.push_back(Value::structure({Value::i32(id), Value::vardict(menu[id].props)}));
                              }
                              return MethodResult::ok({Value::array("(ia{sv})", std::move(out))});
                          }});
    i->methods.push_back({"GetProperty", "is", "v", {}, {}, [this](const MethodCall& c) {
                              int32_t id = static_cast<int32_t>(c.args[0].as_int());
                              for (auto& [k, v] : menu[id].props)
                                  if (k == c.args[1].as_string()) return MethodResult::ok({Value::variant(v)});
                              return MethodResult::error(kErrorInvalidArgs, "no such property");
                          }});
    i->methods.push_back({"Event", "isvu", "", {"id", "eventId", "data", "timestamp"}, {}, [](const MethodCall& c) {
                              Value d = c.args[2].unwrap();
                              std::string data = d.sig + ":";
                              if (d.sig == "s") data += d.as_string();
                              else if (d.sig == "b") data += d.as_bool() ? "true" : "false";
                              else data += std::to_string(d.as_int());
                              say("EVENT " + std::to_string(c.args[0].as_int()) + " " + c.args[1].as_string() + " " +
                                  data + " " + std::to_string(c.args[3].as_uint()));
                              return MethodResult::ok();
                          }});
    i->methods.push_back({"EventGroup", "a(isvu)", "ai", {}, {}, [](const MethodCall& c) {
                              for (auto& e : c.args[0].items())
                                  say("EVENT " + std::to_string(e.items()[0].as_int()) + " " + e.items()[1].as_string());
                              return MethodResult::ok({Value::array("i", {})});
                          }});
    i->methods.push_back({"AboutToShow", "i", "b", {"id"}, {"needUpdate"}, [this](const MethodCall& c) {
                              say("ABOUTTOSHOW " + std::to_string(c.args[0].as_int()));
                              if (dirty_label.empty()) return MethodResult::ok({Value::boolean(false)});
                              set_prop(1, "label", Value::str(dirty_label));
                              dirty_label.clear();
                              ++revision;
                              return MethodResult::ok({Value::boolean(true)});
                          }});
    i->methods.push_back({"AboutToShowGroup", "ai", "aiai", {}, {}, [](const MethodCall&) {
                              return MethodResult::ok({Value::array("i", {}), Value::array("i", {})});
                          }});
    i->properties.push_back({"Version", "u", [] { return Value::u32(3); }, nullptr, "true"});
    i->properties.push_back({"TextDirection", "s", [] { return Value::str("ltr"); }, nullptr, "true"});
    i->properties.push_back({"Status", "s", [] { return Value::str("normal"); }, nullptr, "true"});
    i->properties.push_back({"IconThemePath", "as", [] { return Value::strings({}); }, nullptr, "true"});
    i->signals.push_back({"ItemsPropertiesUpdated", "a(ia{sv})a(ias)", {"updatedProps", "removedProps"}});
    i->signals.push_back({"LayoutUpdated", "ui", {"revision", "parent"}});
    i->signals.push_back({"ItemActivationRequested", "iu", {"id", "timestamp"}});
    return i;
}

std::shared_ptr<Interface> Item::control_interface() {
    auto i = std::make_shared<Interface>();
    i->name = "org.brosys.TestItem";
    auto setter = [&](const char* name, std::string Item::*field, const char* signal) {
        i->methods.push_back({name, "s", "", {}, {}, [this, field, signal](const MethodCall& c) {
                                  this->*field = c.args[0].as_string();
                                  if (std::strcmp(signal, "NewStatus") == 0)
                                      emit(signal, {Value::str(status)});
                                  else
                                      emit(signal);
                                  return MethodResult::ok();
                              }});
    };
    setter("SetTitle", &Item::title, "NewTitle");
    setter("SetStatus", &Item::status, "NewStatus");
    setter("SetIcon", &Item::icon, "NewIcon");
    setter("SetAttention", &Item::attention, "NewAttentionIcon");
    setter("SetOverlay", &Item::overlay, "NewOverlayIcon");
    i->methods.push_back({"SetToolTip", "ss", "", {}, {}, [this](const MethodCall& c) {
                              tip_title = c.args[0].as_string();
                              tip_body = c.args[1].as_string();
                              emit("NewToolTip");
                              return MethodResult::ok();
                          }});
    i->methods.push_back({"Burst", "u", "", {}, {}, [this](const MethodCall& c) {
                              uint64_t n = c.args[0].as_uint();
                              for (uint64_t k = 0; k < n; ++k) {
                                  title = "burst-" + std::to_string(k);
                                  emit("NewTitle");
                              }
                              return MethodResult::ok();
                          }});
    i->methods.push_back({"RenameMenuItem", "is", "", {}, {}, [this](const MethodCall& c) {
                              int32_t id = static_cast<int32_t>(c.args[0].as_int());
                              set_prop(id, "label", Value::str(c.args[1].as_string()));
                              Value upd = Value::array("(ia{sv})", {Value::structure(
                                  {Value::i32(id), Value::vardict({{"label", Value::str(c.args[1].as_string())}})})});
                              conn->emit_signal(kMenuPath, kMenuIface, "ItemsPropertiesUpdated",
                                                {upd, Value::array("(ias)", {})});
                              return MethodResult::ok();
                          }});
    i->methods.push_back({"AddMenuItem", "s", "i", {}, {}, [this](const MethodCall& c) {
                              int32_t id = next_menu_id++;
                              menu[id] = {{{"label", Value::str(c.args[0].as_string())}}, {}};
                              menu[0].children.push_back(id);
                              ++revision;
                              conn->emit_signal(kMenuPath, kMenuIface, "LayoutUpdated", {Value::u32(revision), Value::i32(0)});
                              return MethodResult::ok({Value::i32(id)});
                          }});
    i->methods.push_back({"DirtyOnShow", "s", "", {}, {}, [this](const MethodCall& c) {
                              dirty_label = c.args[0].as_string();
                              return MethodResult::ok();
                          }});
    i->methods.push_back({"Quit", "", "", {}, {}, [this](const MethodCall&) {
                              std::lock_guard<std::mutex> lock(quit_mu);
                              quit = true;
                              quit_cv.notify_all();
                              return MethodResult::ok();
                          }});
    return i;
}

}  // namespace

int main(int argc, char** argv) {
    std::string address, mode = "path";
    bool broken = false;
    Item item;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--bus") address = v;
        else if (k == "--mode") mode = v;
        else if (k == "--id") item.id = v;
        else if (k == "--title") item.title = v;
        else if (k == "--broken") broken = v == "1";
        else if (k == "--flavor" && v == "freedesktop") {
            kItemIface = "org.freedesktop.StatusNotifierItem";
            kWatcher = "org.freedesktop.StatusNotifierWatcher";
        } else if (k == "--watcher") {  // register through this watcher flavour, whatever the item's own
            kWatcher = v == "freedesktop" ? "org.freedesktop.StatusNotifierWatcher" : "org.kde.StatusNotifierWatcher";
        }
    }
    std::string err;
    auto conn = Connection::open(BusKind::Session, address, "sni-item", &err);
    if (!conn) {
        std::fprintf(stderr, "sni_item: %s\n", err.c_str());
        return 2;
    }
    item.conn = conn.get();
    item.build_menu();
    std::string item_id;
    if (mode == "name") {
        std::string name = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
        if (conn->request_name(name, 0, &err) != NameRequest::PrimaryOwner) {
            std::fprintf(stderr, "sni_item: cannot own %s: %s\n", name.c_str(), err.c_str());
            return 2;
        }
        item.item_path = "/StatusNotifierItem";
        item.register_arg = name;
        item_id = name + item.item_path;
    } else {
        item.item_path = "/org/brosys/Item";
        item.register_arg = item.item_path;  // resolved against our unique name by the watcher
        item_id = conn->unique_name() + item.item_path;
    }
    if ((!broken && !conn->export_interface(item.item_path, item.item_interface(), &err)) ||
        !conn->export_interface(kMenuPath, item.menu_interface(), &err) ||
        !conn->export_interface("/control", item.control_interface(), &err)) {
        std::fprintf(stderr, "sni_item: export: %s\n", err.c_str());
        return 2;
    }
    conn->watch_name_owner(kWatcher, [&](const std::string&, const std::string&, const std::string& now) {
        if (!now.empty()) item.register_with(now);
    });
    // The bus daemon restarted: objects and matches are back by themselves;
    // the item's own name and its registration are its business.
    conn->set_reconnect_handler([&] {
        std::string e;
        std::string id = item.register_arg[0] == '/' ? conn->unique_name() + item.item_path : item_id;
        if (mode == "name") conn->request_name(item.register_arg, 0, &e);
        say("RECONNECTED " + conn->unique_name() + " " + id);
        std::string owner = conn->get_name_owner(kWatcher);
        if (!owner.empty()) item.register_with(owner);
    });
    say("READY " + conn->unique_name() + " " + item_id);
    std::string owner = conn->get_name_owner(kWatcher);
    if (!owner.empty()) conn->run_sync([&] { item.register_with(owner); });

    std::unique_lock<std::mutex> lock(item.quit_mu);
    item.quit_cv.wait(lock, [&] { return item.quit; });
    lock.unlock();
    conn.reset();
    return 0;
}
