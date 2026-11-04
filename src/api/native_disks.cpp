// bro.sys.disks: drives, block devices and filesystems through UDisks2;
// mount / unmount / eject / power off and SMART as promises (docs/sys-api.js
// in bro).
#include "api.h"
#include "arg_reader.h"
#include "async_job.h"
#include "object_builder.h"

#include <memory>
#include <string>
#include <vector>

namespace brosys::api {

namespace {

Value buildStrings(const std::vector<std::string>& v) {
    ArrayBuilder arr(v.size());
    for (size_t i = 0; i < v.size(); ++i) arr.set(static_cast<uint32_t>(i), v[i]);
    return arr.build();
}

Value buildDrive(const DiskDrive& d) {
    ObjectBuilder b;
    b.set("id", d.id);
    b.set("vendor", d.vendor);
    b.set("model", d.model);
    b.set("serial", d.serial);
    b.set("revision", d.revision);
    b.set("wwn", d.wwn);
    b.set("size", static_cast<double>(d.size));
    b.set("connectionBus", d.connection_bus);
    b.set("media", d.media);
    b.set("rotationRate", d.rotation_rate);
    b.set("removable", d.removable);
    b.set("mediaRemovable", d.media_removable);
    b.set("mediaAvailable", d.media_available);
    b.set("ejectable", d.ejectable);
    b.set("canPowerOff", d.can_power_off);
    b.set("optical", d.optical);
    b.set("seat", d.seat);
    b.set("sortKey", d.sort_key);
    if (d.kind.empty()) b.setNull("smartKind"); else b.set("smartKind", d.kind);
    b.set("smartSupported", d.smart_supported);
    return b.build();
}

Value buildBlock(const DiskBlock& k) {
    ObjectBuilder b;
    b.set("id", k.id);
    b.set("device", k.device);
    b.set("preferredDevice", k.preferred_device);
    if (k.drive.empty()) b.setNull("drive"); else b.set("drive", k.drive);
    b.set("size", static_cast<double>(k.size));
    b.set("readOnly", k.read_only);
    b.set("usage", k.id_usage);
    b.set("type", k.id_type);
    b.set("label", k.id_label);
    b.set("uuid", k.id_uuid);
    b.set("hintSystem", k.hint_system);
    b.set("hintIgnore", k.hint_ignore);
    b.set("hintAuto", k.hint_auto);
    b.set("hintName", k.hint_name);
    b.set("hintIcon", k.hint_icon);
    if (k.is_partition) {
        ObjectBuilder p;
        p.set("number", static_cast<double>(k.partition_number));
        p.set("table", k.partition_table);
        p.set("type", k.partition_type);
        p.set("name", k.partition_name);
        p.set("uuid", k.partition_uuid);
        p.set("offset", static_cast<double>(k.partition_offset));
        b.set("partition", p.build());
    } else {
        b.setNull("partition");
    }
    if (k.is_partition_table) b.set("partitionTable", k.partition_table_type); else b.setNull("partitionTable");
    b.set("isFilesystem", k.is_filesystem);
    {
        ev::Persistent mp(buildStrings(k.mount_points));
        b.set("mountPoints", mp.get());
    }
    b.set("mounted", !k.mount_points.empty());
    if (k.is_loop) b.set("loopBackingFile", k.loop_backing_file); else b.setNull("loopBackingFile");
    if (k.crypto_backing_device.empty()) b.setNull("cryptoBackingDevice");
    else b.set("cryptoBackingDevice", k.crypto_backing_device);
    return b.build();
}

template <class T>
void setOpt(ObjectBuilder& b, const char* key, const std::optional<T>& v) {
    if (v) b.set(key, static_cast<double>(*v)); else b.setNull(key);
}

Value buildSmart(const DiskSmart& s) {
    ObjectBuilder b;
    b.set("drive", s.drive);
    b.set("kind", s.kind);
    b.set("supported", s.supported);
    b.set("enabled", s.enabled);
    if (s.updated) b.set("updated", static_cast<double>(s.updated) * 1000.0); else b.setNull("updated");
    b.set("failing", s.failing);
    b.set("assessment", s.assessment);
    setOpt(b, "temperatureC", s.temperature_c);
    setOpt(b, "powerOnHours", s.power_on_hours);
    setOpt(b, "percentUsed", s.percent_used);
    setOpt(b, "availableSpare", s.available_spare);
    setOpt(b, "spareThreshold", s.spare_threshold);
    setOpt(b, "badSectors", s.bad_sectors);
    setOpt(b, "powerCycles", s.power_cycles);
    setOpt(b, "unsafeShutdowns", s.unsafe_shutdowns);
    setOpt(b, "mediaErrors", s.media_errors);
    setOpt(b, "bytesRead", s.bytes_read);
    setOpt(b, "bytesWritten", s.bytes_written);
    {
        ev::Persistent w(buildStrings(s.critical_warnings));
        b.set("criticalWarnings", w.get());
    }
    b.set("selftestStatus", s.selftest_status);
    ArrayBuilder attrs(s.attributes.size());
    for (size_t i = 0; i < s.attributes.size(); ++i) {
        const SmartAttribute& a = s.attributes[i];
        ObjectBuilder ab;
        ab.set("id", a.id);
        ab.set("name", a.name);
        if (a.value >= 0) ab.set("value", a.value); else ab.setNull("value");
        if (a.worst >= 0) ab.set("worst", a.worst); else ab.setNull("worst");
        if (a.threshold >= 0) ab.set("threshold", a.threshold); else ab.setNull("threshold");
        ab.set("raw", static_cast<double>(a.raw));
        ab.set("unit", a.unit);
        ab.set("failing", a.failing);
        ev::Persistent v(ab.build());
        attrs.set(static_cast<uint32_t>(i), v.get());
    }
    b.set("attributes", attrs.build());
    return b.build();
}

template <class T, class F>
Value buildList(const std::vector<T>& items, F build) {
    ArrayBuilder arr(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        ev::Persistent v(build(items[i]));
        arr.set(static_cast<uint32_t>(i), v.get());
    }
    return arr.build();
}

std::string g_reason;

DiskService* service() {
    std::string err;
    DiskService* svc = getDiskService(&err);
    if (!svc) g_reason = err.empty() ? "disk management is unavailable" : err;
    return svc;
}

Value rejected(const std::string& why) {
    return runAsync([why] { return why; }, nullptr);
}

using Action = std::function<Result(DiskService&)>;

// The action off the page's thread; resolves to true.
Value actionPromise(Action action) {
    DiskService* svc = service();
    if (!svc) return rejected(g_reason);
    return runAsync(
        [svc, action] {
            Result r = action(*svc);
            return r.ok ? std::string() : r.error;
        },
        [] { return ev::fromBool(true); });
}

Value optionString(Value opts, const char* key) {
    if (!ev::isObject(opts)) return ev::undefined();
    return ev::getProperty(opts, key);
}

}  // namespace

void installDisks(ObjectBuilder& sys) {
    ObjectBuilder disks;
#if !defined(__linux__)
    disks.set("available", false);
    disks.set("reason", std::string("disk management (UDisks2) is implemented only on Linux"));
#endif

    disks.def("getState", 0, [](Value, std::span<const Value>) {
        DiskService* svc = service();
        ObjectBuilder b;
        b.set("available", svc != nullptr);
        if (svc) b.setNull("reason"); else b.set("reason", g_reason);
        ev::Persistent drives(buildList(svc ? svc->drives() : std::vector<DiskDrive>{}, buildDrive));
        b.set("drives", drives.get());
        ev::Persistent blocks(buildList(svc ? svc->blocks() : std::vector<DiskBlock>{}, buildBlock));
        b.set("blocks", blocks.get());
        return b.build();
    });
    disks.def("getDrives", 0, [](Value, std::span<const Value>) {
        DiskService* svc = service();
        return buildList(svc ? svc->drives() : std::vector<DiskDrive>{}, buildDrive);
    });
    disks.def("getBlocks", 0, [](Value, std::span<const Value>) {
        DiskService* svc = service();
        return buildList(svc ? svc->blocks() : std::vector<DiskBlock>{}, buildBlock);
    });
    disks.def("getDrive", 1, [](Value, std::span<const Value> args) {
        DiskService* svc = service();
        auto d = svc ? svc->drive(strAt(args, 0)) : std::nullopt;
        return d ? buildDrive(*d) : ev::null();
    });
    disks.def("getBlock", 1, [](Value, std::span<const Value> args) {
        DiskService* svc = service();
        auto k = svc ? svc->block(strAt(args, 0)) : std::nullopt;
        return k ? buildBlock(*k) : ev::null();
    });

    // mount(block, { fstype, options }) -> Promise<mountPoint>
    disks.def("mount", 2, [](Value, std::span<const Value> args) {
        const std::string id = strAt(args, 0);
        if (id.empty()) return rejected("disks.mount needs a block device (id or /dev path)");
        ev::Persistent opts(argAt(args, 1));
        MountOptions mo;
        mo.fstype = strVal(optionString(opts.get(), "fstype"));
        mo.options = strVal(optionString(opts.get(), "options"));
        DiskService* svc = service();
        if (!svc) return rejected(g_reason);
        auto where = std::make_shared<std::string>();
        return runAsync(
            [svc, id, mo, where] {
                Result r = svc->mount(id, mo, where.get());
                return r.ok ? std::string() : r.error;
            },
            [where] { return ev::fromUtf8(*where); });
    });
    // unmount(block, { force }) -> Promise<true>
    disks.def("unmount", 2, [](Value, std::span<const Value> args) {
        const std::string id = strAt(args, 0);
        if (id.empty()) return rejected("disks.unmount needs a block device (id or /dev path)");
        ev::Persistent opts(argAt(args, 1));
        const bool force = boolVal(optionString(opts.get(), "force"));
        return actionPromise([id, force](DiskService& s) { return s.unmount(id, force); });
    });
    disks.def("eject", 1, [](Value, std::span<const Value> args) {
        const std::string id = strAt(args, 0);
        if (id.empty()) return rejected("disks.eject needs a drive id");
        return actionPromise([id](DiskService& s) { return s.eject(id); });
    });
    disks.def("powerOff", 1, [](Value, std::span<const Value> args) {
        const std::string id = strAt(args, 0);
        if (id.empty()) return rejected("disks.powerOff needs a drive id");
        return actionPromise([id](DiskService& s) { return s.power_off(id); });
    });
    // smart(drive) -> Promise<Smart>
    disks.def("smart", 1, [](Value, std::span<const Value> args) {
        const std::string id = strAt(args, 0);
        if (id.empty()) return rejected("disks.smart needs a drive id");
        DiskService* svc = service();
        if (!svc) return rejected(g_reason);
        auto out = std::make_shared<DiskSmart>();
        return runAsync(
            [svc, id, out] {
                Result r = svc->smart(id, *out);
                return r.ok ? std::string() : r.error;
            },
            [out] { return buildSmart(*out); });
    });

    disks.def("on", 2, [](Value, std::span<const Value> args) {
        ev::Persistent cb(argAt(args, 1));
        const std::string name = strAt(args, 0);
        service();  // events flow once the service runs
        addEventListener("disks", name, cb.get());
        return ev::fromBool(true);
    });
    disks.def("off", 2, [](Value, std::span<const Value> args) {
        ev::Persistent cb(argAt(args, 1));
        removeEventListener("disks", strAt(args, 0), cb.get());
        return ev::fromBool(true);
    });

    sys.set("disks", disks.build());
}

void tickDisks() {
    DiskService* svc = getDiskService();
    if (!svc) return;
    for (const auto& item : svc->events().drain()) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            const char* type = nullptr;
            ObjectBuilder change;
            if constexpr (std::is_same_v<T, DriveAdded> || std::is_same_v<T, DriveChanged>) {
                type = std::is_same_v<T, DriveAdded> ? "driveAdded" : "driveChanged";
                ev::Persistent p(buildDrive(e.drive));
                dispatchEvent("disks", type, p.get());
                change.set("drive", p.get());
            } else if constexpr (std::is_same_v<T, BlockAdded> || std::is_same_v<T, BlockChanged>) {
                type = std::is_same_v<T, BlockAdded> ? "blockAdded" : "blockChanged";
                ev::Persistent p(buildBlock(e.block));
                dispatchEvent("disks", type, p.get());
                change.set("block", p.get());
            } else if constexpr (std::is_same_v<T, DriveRemoved> || std::is_same_v<T, BlockRemoved>) {
                type = std::is_same_v<T, DriveRemoved> ? "driveRemoved" : "blockRemoved";
                ObjectBuilder p;
                p.set("id", e.id);
                ev::Persistent pv(p.build());
                dispatchEvent("disks", type, pv.get());
                change.set("id", e.id);
            } else if constexpr (std::is_same_v<T, BlockMountChanged>) {
                type = e.block.mount_points.empty() ? "unmounted" : (e.previous.empty() ? "mounted" : "mountChanged");
                ObjectBuilder p;
                ev::Persistent blk(buildBlock(e.block));
                p.set("block", blk.get());
                ev::Persistent mp(buildStrings(e.block.mount_points));
                p.set("mountPoints", mp.get());
                ev::Persistent prev(buildStrings(e.previous));
                p.set("previous", prev.get());
                ev::Persistent pv(p.build());
                dispatchEvent("disks", type, pv.get());
                change.set("block", blk.get());
            } else if constexpr (std::is_same_v<T, DisksAvailability>) {
                type = "availability";
                ObjectBuilder p;
                p.set("available", e.available);
                ev::Persistent pv(p.build());
                dispatchEvent("disks", type, pv.get());
                change.set("available", e.available);
            }
            change.set("type", std::string(type));
            ev::Persistent cv(change.build());
            dispatchEvent("disks", "change", cv.get());
        }, item);
    }
}

void shutdownDisks() {
}

}  // namespace brosys::api
