#include "linux/disks/udisks_model.h"

#include <algorithm>

namespace brosys::udisks {

namespace {

const Props* iface(const Object& o, const char* name) {
    auto it = o.find(name);
    return it == o.end() ? nullptr : &it->second;
}

const dbus::Value* prop(const Props* p, const char* name) {
    if (!p) return nullptr;
    auto it = p->find(name);
    return it == p->end() ? nullptr : &it->second;
}

std::string str_prop(const Props* p, const char* name) {
    auto* v = prop(p, name);
    return v ? v->as_string() : std::string();
}

bool bool_prop(const Props* p, const char* name, bool fallback = false) {
    auto* v = prop(p, name);
    return v ? v->as_bool(fallback) : fallback;
}

uint64_t u64_prop(const Props* p, const char* name) {
    auto* v = prop(p, name);
    return v ? v->as_uint(0) : 0;
}

int64_t i64_prop(const Props* p, const char* name, int64_t fallback = 0) {
    auto* v = prop(p, name);
    return v ? v->as_int(fallback) : fallback;
}

std::string bytes_prop(const Props* p, const char* name) {
    auto* v = prop(p, name);
    return v ? bytestring(*v) : std::string();
}

// "/" from an object path that UDisks uses for "none".
std::string obj_prop(const Props* p, const char* name) {
    std::string s = str_prop(p, name);
    return s == "/" ? std::string() : s;
}

}  // namespace

std::string bytestring(const dbus::Value& vIn) {
    const dbus::Value& v = vIn.unwrap();
    if (const auto* b = v.as_bytes()) {
        std::string s(b->begin(), b->end());
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
    // An "ay" read as an array of bytes.
    std::string s;
    for (auto& item : v.items()) {
        const uint64_t c = item.as_uint(0);
        if (c == 0) break;
        s += static_cast<char>(c);
    }
    return s;
}

std::optional<DiskDrive> parse_drive(const std::string& path, const Object& object) {
    const Props* p = iface(object, kDrive);
    if (!p) return std::nullopt;
    DiskDrive d;
    d.id = path;
    d.vendor = str_prop(p, "Vendor");
    d.model = str_prop(p, "Model");
    d.serial = str_prop(p, "Serial");
    d.revision = str_prop(p, "Revision");
    d.wwn = str_prop(p, "WWN");
    d.size = u64_prop(p, "Size");
    d.connection_bus = str_prop(p, "ConnectionBus");
    d.media = str_prop(p, "Media");
    d.rotation_rate = static_cast<int32_t>(i64_prop(p, "RotationRate", -1));
    d.removable = bool_prop(p, "Removable");
    d.media_removable = bool_prop(p, "MediaRemovable");
    d.media_available = bool_prop(p, "MediaAvailable");
    d.ejectable = bool_prop(p, "Ejectable");
    d.can_power_off = bool_prop(p, "CanPowerOff");
    d.optical = bool_prop(p, "Optical");
    d.seat = str_prop(p, "Seat");
    d.sort_key = str_prop(p, "SortKey");
    if (const Props* n = iface(object, kNVMeController)) {
        d.kind = "nvme";
        d.smart_supported = true;
        (void)n;
    } else if (const Props* a = iface(object, kDriveAta)) {
        d.kind = "ata";
        d.smart_supported = bool_prop(a, "SmartSupported");
    }
    return d;
}

std::optional<DiskBlock> parse_block(const std::string& path, const Object& object) {
    const Props* p = iface(object, kBlock);
    if (!p) return std::nullopt;
    DiskBlock b;
    b.id = path;
    b.device = bytes_prop(p, "Device");
    b.preferred_device = bytes_prop(p, "PreferredDevice");
    if (b.preferred_device.empty()) b.preferred_device = b.device;
    b.drive = obj_prop(p, "Drive");
    b.size = u64_prop(p, "Size");
    b.read_only = bool_prop(p, "ReadOnly");
    b.id_usage = str_prop(p, "IdUsage");
    b.id_type = str_prop(p, "IdType");
    b.id_label = str_prop(p, "IdLabel");
    b.id_uuid = str_prop(p, "IdUUID");
    b.hint_system = bool_prop(p, "HintSystem");
    b.hint_ignore = bool_prop(p, "HintIgnore");
    b.hint_auto = bool_prop(p, "HintAuto");
    b.hint_name = str_prop(p, "HintName");
    b.hint_icon = str_prop(p, "HintIconName");
    b.crypto_backing_device = obj_prop(p, "CryptoBackingDevice");
    if (const Props* part = iface(object, kPartition)) {
        b.is_partition = true;
        b.partition_number = static_cast<uint32_t>(u64_prop(part, "Number"));
        b.partition_table = obj_prop(part, "Table");
        b.partition_type = str_prop(part, "Type");
        b.partition_name = str_prop(part, "Name");
        b.partition_uuid = str_prop(part, "UUID");
        b.partition_offset = u64_prop(part, "Offset");
    }
    if (const Props* table = iface(object, kPartitionTable)) {
        b.is_partition_table = true;
        b.partition_table_type = str_prop(table, "Type");
    }
    if (const Props* fs = iface(object, kFilesystem)) {
        b.is_filesystem = true;
        if (auto* mp = prop(fs, "MountPoints")) {
            for (auto& item : mp->unwrap().items()) {
                std::string m = bytestring(item);
                if (!m.empty()) b.mount_points.push_back(std::move(m));
            }
        }
    }
    if (const Props* loop = iface(object, kLoop)) {
        b.is_loop = true;
        b.loop_backing_file = bytes_prop(loop, "BackingFile");
    }
    return b;
}

std::vector<DiskDrive> build_drives(const Objects& objects) {
    std::vector<DiskDrive> out;
    for (auto& [path, o] : objects)
        if (auto d = parse_drive(path, o)) out.push_back(std::move(*d));
    return out;
}

std::vector<DiskBlock> build_blocks(const Objects& objects) {
    std::vector<DiskBlock> out;
    for (auto& [path, o] : objects)
        if (auto b = parse_block(path, o)) out.push_back(std::move(*b));
    return out;
}

DiskSmart parse_smart(const std::string& path, const Object& object, const dbus::Value* attributes) {
    DiskSmart s;
    s.drive = path;
    s.assessment = "unknown";
    if (const Props* n = iface(object, kNVMeController)) {
        s.kind = "nvme";
        s.supported = true;
        s.enabled = true;
        s.updated = u64_prop(n, "SmartUpdated");
        if (const uint64_t k = u64_prop(n, "SmartTemperature")) s.temperature_c = static_cast<double>(k) - 273.15;
        if (prop(n, "SmartPowerOnHours")) s.power_on_hours = u64_prop(n, "SmartPowerOnHours");
        if (auto* w = prop(n, "SmartCriticalWarning")) s.critical_warnings = w->unwrap().as_strings();
        s.selftest_status = str_prop(n, "SmartSelftestStatus");
        if (attributes) {
            const dbus::Value& a = attributes->unwrap();
            auto num = [&](const char* key) -> std::optional<uint64_t> {
                if (const dbus::Value* v = a.lookup(key)) return v->to_uint();
                return std::nullopt;
            };
            if (auto v = num("percent_used")) s.percent_used = static_cast<int32_t>(*v);
            if (auto v = num("avail_spare")) s.available_spare = static_cast<int32_t>(*v);
            if (auto v = num("spare_thresh")) s.spare_threshold = static_cast<int32_t>(*v);
            s.power_cycles = num("power_cycles");
            s.unsafe_shutdowns = num("unsafe_shutdowns");
            s.media_errors = num("media_errors");
            s.bytes_read = num("total_data_read");
            s.bytes_written = num("total_data_written");
            for (auto& entry : a.items()) {
                auto& kv = entry.items();
                if (kv.size() != 2) continue;
                const dbus::Value& v = kv[1].unwrap();
                if (v.is_array()) continue;
                auto u = v.to_uint();
                if (!u) continue;
                SmartAttribute attr;
                attr.name = kv[0].as_string();
                attr.raw = static_cast<int64_t>(*u);
                if (attr.name == "wctemp" || attr.name == "cctemp") attr.unit = "K";
                else if (attr.name == "total_data_read" || attr.name == "total_data_written") attr.unit = "bytes";
                else if (attr.name == "ctrl_busy_time" || attr.name == "warning_temp_time" ||
                         attr.name == "critical_temp_time") attr.unit = "min";
                else if (attr.name == "percent_used" || attr.name == "avail_spare" || attr.name == "spare_thresh")
                    attr.unit = "%";
                s.attributes.push_back(std::move(attr));
            }
        }
        const auto& w = s.critical_warnings;
        const bool hard = std::any_of(w.begin(), w.end(), [](const std::string& x) { return x != "temperature"; });
        s.failing = hard;
        if (hard) s.assessment = "failing";
        else if (!w.empty() || (s.percent_used && *s.percent_used >= 100) ||
                 (s.available_spare && s.spare_threshold && *s.available_spare < *s.spare_threshold) ||
                 (s.media_errors && *s.media_errors > 0))
            s.assessment = "warning";
        else if (s.updated) s.assessment = "good";
        return s;
    }
    if (const Props* a = iface(object, kDriveAta)) {
        s.kind = "ata";
        s.supported = bool_prop(a, "SmartSupported");
        s.enabled = bool_prop(a, "SmartEnabled");
        s.updated = u64_prop(a, "SmartUpdated");
        s.failing = bool_prop(a, "SmartFailing");
        if (auto* t = prop(a, "SmartTemperature")) {
            const double k = t->as_double(0);
            if (k > 0) s.temperature_c = k - 273.15;
        }
        if (const uint64_t secs = u64_prop(a, "SmartPowerOnSeconds")) s.power_on_hours = secs / 3600;
        if (auto* b = prop(a, "SmartNumBadSectors")) {
            const int64_t n = b->as_int(-1);
            if (n >= 0) s.bad_sectors = n;
        }
        s.selftest_status = str_prop(a, "SmartSelftestStatus");
        if (attributes) {
            for (auto& item : attributes->unwrap().items()) {
                auto& f = item.items();  // (ysqiiixia{sv})
                if (f.size() < 8) continue;
                SmartAttribute attr;
                attr.id = static_cast<int32_t>(f[0].as_int(0));
                attr.name = f[1].as_string();
                attr.value = static_cast<int32_t>(f[3].as_int(-1));
                attr.worst = static_cast<int32_t>(f[4].as_int(-1));
                attr.threshold = static_cast<int32_t>(f[5].as_int(-1));
                attr.raw = f[6].as_int(0);
                switch (f[7].as_int(0)) {
                    case 2: attr.unit = "ms"; break;
                    case 3: attr.unit = "sectors"; break;
                    case 4: attr.unit = "mK"; break;
                    default: break;
                }
                attr.failing = attr.value > 0 && attr.threshold > 0 && attr.value <= attr.threshold;
                if (attr.name == "power-cycle-count") s.power_cycles = static_cast<uint64_t>(attr.raw);
                if (attr.name == "unsafe-shutdown-count" || attr.name == "power-off-retract-count")
                    s.unsafe_shutdowns = static_cast<uint64_t>(attr.raw);
                s.attributes.push_back(std::move(attr));
            }
        }
        const int64_t failing_now = i64_prop(a, "SmartNumAttributesFailing", 0);
        const int64_t failed_past = i64_prop(a, "SmartNumAttributesFailedInThePast", 0);
        if (!s.supported || !s.enabled || !s.updated) s.assessment = "unknown";
        else if (s.failing) s.assessment = "failing";
        else if (failing_now > 0 || failed_past > 0 || (s.bad_sectors && *s.bad_sectors > 0)) s.assessment = "warning";
        else s.assessment = "good";
        return s;
    }
    return s;
}

void merge_interfaces(Objects& objects, const std::string& path, const dbus::Value& ifaces) {
    Object& o = objects[path];
    for (auto& entry : ifaces.unwrap().items()) {
        auto& kv = entry.items();
        if (kv.size() != 2) continue;
        Props& p = o[kv[0].as_string()];
        for (auto& pe : kv[1].items()) {
            auto& pkv = pe.items();
            if (pkv.size() == 2) p[pkv[0].as_string()] = pkv[1].unwrap();
        }
    }
}

void merge_managed_objects(Objects& objects, const dbus::Value& managed) {
    for (auto& entry : managed.unwrap().items()) {
        auto& kv = entry.items();
        if (kv.size() == 2) merge_interfaces(objects, kv[0].as_string(), kv[1]);
    }
}

void remove_interfaces(Objects& objects, const std::string& path, const std::vector<std::string>& ifaces) {
    auto it = objects.find(path);
    if (it == objects.end()) return;
    for (auto& i : ifaces) it->second.erase(i);
    if (it->second.empty()) objects.erase(it);
}

bool apply_properties_changed(Objects& objects, const std::string& path, const std::string& iface_name,
                              const dbus::Value& changed) {
    auto oit = objects.find(path);
    if (oit == objects.end()) return false;
    auto iit = oit->second.find(iface_name);
    if (iit == oit->second.end()) return false;
    bool any = false;
    Props& p = iit->second;
    for (auto& entry : changed.unwrap().items()) {
        auto& kv = entry.items();
        if (kv.size() != 2) continue;
        auto& slot = p[kv[0].as_string()];
        const dbus::Value& v = kv[1].unwrap();
        if (!(slot == v)) {
            slot = v;
            any = true;
        }
    }
    return any;
}

}  // namespace brosys::udisks
