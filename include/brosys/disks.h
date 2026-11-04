// Disks: drives, their block devices and filesystems; mount, unmount,
// eject, power off; SMART health.
//
// Linux: UDisks2 over the system bus (org.freedesktop.UDisks2: Drive,
// Drive.Ata, NVMe.Controller, Block, Partition, PartitionTable, Filesystem,
// Loop; the ObjectManager at /org/freedesktop/UDisks2). Polkit decides each
// action for the calling user, interactively when an agent is running.
// Elsewhere create() returns nullptr with a reason.
#pragma once

#include "brosys/common.h"
#include "brosys/event_queue.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace brosys {

struct DiskDrive {
    std::string id;           // UDisks object path, "/org/freedesktop/UDisks2/drives/..."
    std::string vendor;
    std::string model;
    std::string serial;
    std::string revision;
    std::string wwn;
    uint64_t size = 0;        // bytes (0 when no media)
    std::string connection_bus;  // "usb", "sdio", "ieee1394", "" (internal)
    std::string media;           // "thumb", "flash_sd", "optical_cd", ... ("" unknown)
    int32_t rotation_rate = -1;  // rpm; 0 non-rotating (SSD), -1 unknown
    bool removable = false;        // the drive itself is removable (USB stick, card reader)
    bool media_removable = false;  // its media is (optical, card)
    bool media_available = false;
    bool ejectable = false;
    bool can_power_off = false;
    bool optical = false;
    std::string seat;
    std::string sort_key;
    std::string kind;   // "nvme", "ata" (the SMART interface it has), "" none
    bool smart_supported = false;  // SMART data can be read

    bool operator==(const DiskDrive&) const = default;
};

struct DiskBlock {
    std::string id;           // UDisks object path, "/org/freedesktop/UDisks2/block_devices/sda1"
    std::string device;       // "/dev/sda1"
    std::string preferred_device;
    std::string drive;        // its drive's id ("" none: loop, zram, dm)
    uint64_t size = 0;
    bool read_only = false;
    std::string id_usage;     // "filesystem", "crypto", "raid", "other", ""
    std::string id_type;      // "ext4", "vfat", "crypto_LUKS", "swap", ...
    std::string id_label;
    std::string id_uuid;
    bool hint_system = false;   // part of the system (needs more authorization to touch)
    bool hint_ignore = false;   // not shown to users
    bool hint_auto = false;     // automount candidate
    std::string hint_name;
    std::string hint_icon;
    // Partition (is_partition)
    bool is_partition = false;
    uint32_t partition_number = 0;
    std::string partition_table;  // the partitioned block's id
    std::string partition_type;
    std::string partition_name;
    std::string partition_uuid;
    uint64_t partition_offset = 0;
    // Partition table (is_partition_table): "gpt", "dos"
    bool is_partition_table = false;
    std::string partition_table_type;
    // Filesystem (is_filesystem): mountable here
    bool is_filesystem = false;
    std::vector<std::string> mount_points;
    // Loop device
    bool is_loop = false;
    std::string loop_backing_file;
    // Encrypted: the cleartext device's backing block ("" none)
    std::string crypto_backing_device;

    bool operator==(const DiskBlock&) const = default;
};

struct SmartAttribute {
    int32_t id = 0;            // ATA attribute id; 0 for NVMe values
    std::string name;          // "power-on-hours", "percent_used", ...
    int32_t value = -1;        // normalized (ATA; -1 none)
    int32_t worst = -1;
    int32_t threshold = -1;
    int64_t raw = 0;           // pretty value: ATA in `unit`; NVMe the value itself
    std::string unit;          // "", "ms", "sectors", "mK"
    bool failing = false;      // value <= threshold

    bool operator==(const SmartAttribute&) const = default;
};

struct DiskSmart {
    std::string drive;
    std::string kind;          // "nvme" or "ata"
    bool supported = false;
    bool enabled = false;
    uint64_t updated = 0;      // seconds since the epoch the data was read by UDisks (0 never)
    bool failing = false;      // the drive predicts its own failure / reports a critical warning
    std::string assessment;    // "good", "warning", "failing", "unknown"
    std::optional<double> temperature_c;
    std::optional<uint64_t> power_on_hours;
    std::optional<int32_t> percent_used;     // NVMe endurance used (can exceed 100)
    std::optional<int32_t> available_spare;  // NVMe, percent
    std::optional<int32_t> spare_threshold;  // NVMe, percent
    std::optional<int64_t> bad_sectors;      // ATA
    std::optional<uint64_t> power_cycles;
    std::optional<uint64_t> unsafe_shutdowns;
    std::optional<uint64_t> media_errors;
    std::optional<uint64_t> bytes_read;      // NVMe
    std::optional<uint64_t> bytes_written;   // NVMe
    std::vector<std::string> critical_warnings;  // NVMe: "spare", "temperature", "degraded", "readonly", ...
    std::string selftest_status;
    std::vector<SmartAttribute> attributes;

    bool operator==(const DiskSmart&) const = default;
};

// ---------------------------------------------------------------- events

struct DriveAdded { DiskDrive drive; };
struct DriveChanged { DiskDrive drive; };
struct DriveRemoved { std::string id; };
struct BlockAdded { DiskBlock block; };
struct BlockChanged { DiskBlock block; };
struct BlockRemoved { std::string id; };
// A filesystem's mount points changed (a mount or unmount, ours or anyone's).
struct BlockMountChanged {
    DiskBlock block;
    std::vector<std::string> previous;  // its mount points before
};
// UDisks went away (false) or came back (true); the snapshot is reloaded.
struct DisksAvailability { bool available = false; };

using DiskEvent = std::variant<DriveAdded, DriveChanged, DriveRemoved, BlockAdded, BlockChanged, BlockRemoved,
                               BlockMountChanged, DisksAvailability>;
using DiskEventQueue = MessageQueue<DiskEvent>;

struct DiskConfig {
    // System-bus address override; empty = the default system bus.
    std::string system_bus_address;
};

struct MountOptions {
    std::string fstype;    // "" auto
    std::string options;   // "ro,noexec"; "" UDisks' defaults
};

class DiskService {
public:
    static std::unique_ptr<DiskService> create(const DiskConfig& config, std::string* error);
    virtual ~DiskService() = default;

    virtual DiskEventQueue& events() = 0;

    // The latest snapshot (sorted by id).
    virtual std::vector<DiskDrive> drives() const = 0;
    virtual std::vector<DiskBlock> blocks() const = 0;
    // By id, or a block by device path ("/dev/sdb1") too.
    virtual std::optional<DiskDrive> drive(const std::string& id) const = 0;
    virtual std::optional<DiskBlock> block(const std::string& id_or_device) const = 0;

    // Blocking until UDisks answers (a polkit prompt can take a while), so
    // call these off a UI thread. mount's result is the mount point.
    virtual Result mount(const std::string& block, const MountOptions& options, std::string* mount_point) = 0;
    virtual Result unmount(const std::string& block, bool force) = 0;
    // Unmounts what is mounted on the drive's filesystems first (UDisks
    // refuses a busy drive): eject the media, or power the drive off.
    virtual Result eject(const std::string& drive) = 0;
    virtual Result power_off(const std::string& drive) = 0;
    // Read-only: UDisks' last reading plus the attribute table.
    virtual Result smart(const std::string& drive, DiskSmart& out) = 0;
};

}  // namespace brosys
