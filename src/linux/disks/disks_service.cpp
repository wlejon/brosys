// Linux DiskService: UDisks2 over the system bus.
//
// The ObjectManager at /org/freedesktop/UDisks2 mirrors UDisks' object tree
// (drives, block devices and their interfaces); InterfacesAdded / Removed and
// PropertiesChanged keep the mirror, and each change is diffed per object into
// added / changed / removed / mount-changed events. Actions are UDisks method
// calls sent with interactive authorization allowed, so polkit may ask the
// user through the session's agent.
#include "brosys/disks.h"
#include "linux/dbus/connection.h"
#include "linux/disks/udisks_model.h"

#include <systemd/sd-bus.h>

#include <mutex>

namespace brosys {

namespace {

using dbus::Value;
using namespace udisks;

// A polkit prompt waits on the user.
constexpr int kActionTimeoutMs = 120000;

class LinuxDiskService final : public DiskService {
public:
    ~LinuxDiskService() override { conn_.reset(); }

    bool start(const DiskConfig& config, std::string* error) {
        conn_ = dbus::Connection::open(dbus::BusKind::System, config.system_bus_address, "brosys-disks", error);
        if (!conn_) return false;
        const bool up = conn_->run_sync([this] {
            allow_interactive();
            install_watches();
            reload(false);
            return udisks_up_;
        });
        if (!up) {
            if (error) *error = "UDisks2 is not running (org.freedesktop.UDisks2 has no owner)";
            return false;
        }
        conn_->set_disconnect_handler([this] {
            std::lock_guard<std::mutex> lock(mu_);
            objects_.clear();
            update_snapshots();
            udisks_up_ = false;
        });
        conn_->set_reconnect_handler([this] {
            allow_interactive();
            reload(true);
        });
        return true;
    }

    DiskEventQueue& events() override { return queue_; }

    std::vector<DiskDrive> drives() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return drives_;
    }

    std::vector<DiskBlock> blocks() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return blocks_;
    }

    std::optional<DiskDrive> drive(const std::string& id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& d : drives_)
            if (d.id == id) return d;
        return std::nullopt;
    }

    std::optional<DiskBlock> block(const std::string& id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        return find_block(id);
    }

    Result mount(const std::string& id, const MountOptions& options, std::string* mount_point) override {
        auto b = block(id);
        if (!b) return Result::failure("no block device " + id);
        if (!b->is_filesystem) return Result::failure(b->device + " has no mountable filesystem");
        std::vector<std::pair<std::string, Value>> opts{{"auth.no_user_interaction", Value::boolean(false)}};
        if (!options.fstype.empty()) opts.emplace_back("fstype", Value::str(options.fstype));
        if (!options.options.empty()) opts.emplace_back("options", Value::str(options.options));
        dbus::Reply r = conn_->call(kService, b->id, kFilesystem, "Mount", {Value::vardict(opts)}, kActionTimeoutMs);
        if (!r.ok) return Result::failure(r.error());
        if (mount_point) *mount_point = r.first() ? r.first()->as_string() : std::string();
        return Result::success();
    }

    Result unmount(const std::string& id, bool force) override {
        auto b = block(id);
        if (!b) return Result::failure("no block device " + id);
        if (!b->is_filesystem) return Result::failure(b->device + " has no filesystem");
        return unmount_block(b->id, force);
    }

    Result eject(const std::string& id) override { return drive_action(id, "Eject"); }
    Result power_off(const std::string& id) override { return drive_action(id, "PowerOff"); }

    Result smart(const std::string& id, DiskSmart& out) override {
        Object object;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = objects_.find(id);
            if (it == objects_.end() || !it->second.count(kDrive)) return Result::failure("no drive " + id);
            object = it->second;
        }
        const bool nvme = object.count(kNVMeController) > 0;
        const bool ata = object.count(kDriveAta) > 0;
        if (!nvme && !ata) return Result::failure("the drive has no SMART interface UDisks knows (" + id + ")");
        dbus::Reply r;
        if (nvme) {
            r = conn_->call(kService, id, kNVMeController, "SmartGetAttributes", {Value::vardict({})});
        } else {
            const Props& a = object.at(kDriveAta);
            auto sup = a.find("SmartSupported");
            auto en = a.find("SmartEnabled");
            if (sup != a.end() && sup->second.as_bool() && en != a.end() && en->second.as_bool())
                r = conn_->call(kService, id, kDriveAta, "SmartGetAttributes", {Value::vardict({})});
        }
        // No attribute table (never collected, not enabled): the properties alone.
        out = parse_smart(id, object, r.ok ? r.first() : nullptr);
        return Result::success();
    }

private:
    void allow_interactive() {
        if (sd_bus* bus = conn_->raw()) sd_bus_set_allow_interactive_authorization(bus, 1);
    }

    // mu_ held.
    std::optional<DiskBlock> find_block(const std::string& id) const {
        for (auto& b : blocks_)
            if (b.id == id || b.device == id || b.preferred_device == id) return b;
        return std::nullopt;
    }

    Result unmount_block(const std::string& path, bool force) {
        std::vector<std::pair<std::string, Value>> opts{{"auth.no_user_interaction", Value::boolean(false)}};
        if (force) opts.emplace_back("force", Value::boolean(true));
        dbus::Reply r = conn_->call(kService, path, kFilesystem, "Unmount", {Value::vardict(opts)}, kActionTimeoutMs);
        return r.ok ? Result::success() : Result::failure(r.error());
    }

    // Unmounts the drive's mounted filesystems (UDisks refuses a busy
    // drive), then Eject / PowerOff.
    Result drive_action(const std::string& id, const char* method) {
        std::vector<std::string> mounted;
        {
            std::lock_guard<std::mutex> lock(mu_);
            bool found = false;
            for (auto& d : drives_) found = found || d.id == id;
            if (!found) return Result::failure("no drive " + id);
            for (auto& b : blocks_)
                if (b.drive == id && b.is_filesystem && !b.mount_points.empty()) mounted.push_back(b.id);
        }
        for (auto& path : mounted) {
            Result r = unmount_block(path, false);
            if (!r.ok) return r;
        }
        dbus::Reply r = conn_->call(kService, id, kDrive, method,
                                    {Value::vardict({{"auth.no_user_interaction", Value::boolean(false)}})},
                                    kActionTimeoutMs);
        return r.ok ? Result::success() : Result::failure(r.error());
    }

    void install_watches() {
        conn_->watch_name_owner(kService, [this](const std::string&, const std::string&, const std::string& owner) {
            if (owner.empty()) {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    objects_.clear();
                    update_snapshots();
                    udisks_up_ = false;
                }
                queue_.push(DisksAvailability{false});
            } else {
                reload(true);
            }
        });
        const std::string om = std::string("type='signal',sender='") + kService +
                               "',interface='org.freedesktop.DBus.ObjectManager',member=";
        conn_->add_match(om + "'InterfacesAdded'", [this](const dbus::Message& m) {
            if (m.args.size() < 2) return;
            const std::string path = m.args[0].as_string();
            changed(path, [&] { merge_interfaces(objects_, path, m.args[1]); });
        });
        conn_->add_match(om + "'InterfacesRemoved'", [this](const dbus::Message& m) {
            if (m.args.size() < 2) return;
            const std::string path = m.args[0].as_string();
            const std::vector<std::string> ifaces = m.args[1].as_strings();
            changed(path, [&] { remove_interfaces(objects_, path, ifaces); });
        });
        conn_->add_match(std::string("type='signal',sender='") + kService +
                             "',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                         [this](const dbus::Message& m) {
                             if (m.args.size() < 2) return;
                             const std::string iface = m.args[0].as_string();
                             changed(m.path, [&] { apply_properties_changed(objects_, m.path, iface, m.args[1]); });
                         });
    }

    // Applies `mutate` to the mirror and queues what it changed at `path`.
    template <class F>
    void changed(const std::string& path, F&& mutate) {
        std::optional<DiskDrive> d0, d1;
        std::optional<DiskBlock> b0, b1;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto before = objects_.find(path);
            if (before != objects_.end()) {
                d0 = parse_drive(path, before->second);
                b0 = parse_block(path, before->second);
            }
            mutate();
            auto after = objects_.find(path);
            if (after != objects_.end()) {
                d1 = parse_drive(path, after->second);
                b1 = parse_block(path, after->second);
            }
            if (d0 != d1 || b0 != b1) update_snapshots();
        }
        if (d0 && !d1) queue_.push(DriveRemoved{path});
        else if (!d0 && d1) queue_.push(DriveAdded{*d1});
        else if (d0 && d1 && *d0 != *d1) queue_.push(DriveChanged{*d1});
        if (b0 && !b1) {
            queue_.push(BlockRemoved{path});
        } else if (!b0 && b1) {
            queue_.push(BlockAdded{*b1});
        } else if (b0 && b1 && *b0 != *b1) {
            if (b0->mount_points != b1->mount_points) queue_.push(BlockMountChanged{*b1, b0->mount_points});
            queue_.push(BlockChanged{*b1});
        }
    }

    void reload(bool announce) {
        dbus::Reply r = conn_->call(kService, kRoot, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        {
            std::lock_guard<std::mutex> lock(mu_);
            objects_.clear();
            udisks_up_ = r.ok;
            if (r.ok && !r.values.empty()) merge_managed_objects(objects_, r.values[0]);
            update_snapshots();
        }
        if (announce) queue_.push(DisksAvailability{r.ok});
    }

    void update_snapshots() {  // mu_ held
        drives_ = build_drives(objects_);
        blocks_ = build_blocks(objects_);
    }

    DiskEventQueue queue_;
    mutable std::mutex mu_;
    Objects objects_;
    std::vector<DiskDrive> drives_;
    std::vector<DiskBlock> blocks_;
    bool udisks_up_ = false;
    std::unique_ptr<dbus::Connection> conn_;  // last: destroyed (and its thread joined) first
};

}  // namespace

std::unique_ptr<DiskService> DiskService::create(const DiskConfig& config, std::string* error) {
    auto s = std::make_unique<LinuxDiskService>();
    if (!s->start(config, error)) return nullptr;
    return s;
}

}  // namespace brosys
