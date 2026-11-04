// DiskService against a scripted UDisks2 on a private bus: the snapshot
// (drives, partitions, filesystems, loop devices), mount / unmount / eject /
// power off and the events they cause, SMART for NVMe and ATA drives, a
// refused action's error, devices appearing, and UDisks going away. Nothing on
// the machine's real buses or disks is touched.
#include "check.h"
#include "brosys/disks.h"
#include "linux/desktop/event_log.h"
#include "linux/fakes/fake_udisks.h"
#include "linux/support/private_bus.h"

#include <algorithm>
#include <cmath>

using namespace brosys;
using namespace std::chrono_literals;
using Log = bstest::EventLog<DiskEvent>;
using V = dbus::Value;
using Fake = bstest::FakeUDisks;

namespace {

const std::string kUsb = "/org/freedesktop/UDisks2/drives/Kingston_DataTraveler_1234";
const std::string kNvme = "/org/freedesktop/UDisks2/drives/Lexar_SSD_9999";
const std::string kAta = "/org/freedesktop/UDisks2/drives/WDC_WD40_5555";
const std::string kSdb = "/org/freedesktop/UDisks2/block_devices/sdb";
const std::string kSdb1 = "/org/freedesktop/UDisks2/block_devices/sdb1";
const std::string kNvmeP1 = "/org/freedesktop/UDisks2/block_devices/nvme0n1p1";
const std::string kLoop0 = "/org/freedesktop/UDisks2/block_devices/loop0";

Fake::Props drive_props(const std::string& model, bool removable, const std::string& bus, uint64_t size) {
    return {
        {"Vendor", V::str("")}, {"Model", V::str(model)}, {"Serial", V::str("SER-" + model)},
        {"Revision", V::str("1.0")}, {"WWN", V::str("")}, {"Size", V::u64(size)},
        {"ConnectionBus", V::str(bus)}, {"Media", V::str(removable ? "thumb" : "")},
        {"RotationRate", V::i32(removable ? -1 : 0)}, {"Removable", V::boolean(removable)},
        {"MediaRemovable", V::boolean(false)}, {"MediaAvailable", V::boolean(true)},
        {"Ejectable", V::boolean(removable)}, {"CanPowerOff", V::boolean(removable)},
        {"Optical", V::boolean(false)}, {"Seat", V::str("seat0")}, {"SortKey", V::str("01hotplug/" + model)},
    };
}

Fake::Props block_props(const std::string& dev, const std::string& drive, uint64_t size, const std::string& usage,
                        const std::string& type, const std::string& label, bool system) {
    return {
        {"Device", Fake::bytestring(dev)}, {"PreferredDevice", Fake::bytestring(dev)},
        {"Drive", V::obj(drive.empty() ? "/" : drive)}, {"Size", V::u64(size)}, {"ReadOnly", V::boolean(false)},
        {"IdUsage", V::str(usage)}, {"IdType", V::str(type)}, {"IdLabel", V::str(label)},
        {"IdUUID", V::str(label.empty() ? "" : "uuid-" + label)}, {"HintSystem", V::boolean(system)},
        {"HintIgnore", V::boolean(false)}, {"HintAuto", V::boolean(!system)}, {"HintName", V::str("")},
        {"HintIconName", V::str("")}, {"CryptoBackingDevice", V::obj("/")},
    };
}

void populate(Fake& fake) {
    fake.add_object(kUsb, {{"org.freedesktop.UDisks2.Drive", drive_props("DataTraveler", true, "usb", 16000000000ull)}});
    fake.add_object(kNvme, {
        {"org.freedesktop.UDisks2.Drive", drive_props("Lexar SSD", false, "", 2000000000000ull)},
        {"org.freedesktop.UDisks2.NVMe.Controller", {
            {"SmartUpdated", V::u64(1793783075)}, {"SmartPowerOnHours", V::u64(28)},
            {"SmartTemperature", V::u16(305)}, {"SmartCriticalWarning", V::strings({})},
            {"SmartSelftestStatus", V::str("success")}, {"State", V::str("live")},
        }},
    });
    fake.add_object(kAta, {
        {"org.freedesktop.UDisks2.Drive", drive_props("WD40", false, "", 4000000000000ull)},
        {"org.freedesktop.UDisks2.Drive.Ata", {
            {"SmartSupported", V::boolean(true)}, {"SmartEnabled", V::boolean(true)},
            {"SmartUpdated", V::u64(1793780000)}, {"SmartFailing", V::boolean(false)},
            {"SmartPowerOnSeconds", V::u64(3600ull * 1234)}, {"SmartTemperature", V::dbl(311.15)},
            {"SmartNumAttributesFailing", V::i32(0)}, {"SmartNumAttributesFailedInThePast", V::i32(0)},
            {"SmartNumBadSectors", V::i64(8)}, {"SmartSelftestStatus", V::str("success")},
        }},
    });
    fake.add_object(kSdb, {
        {"org.freedesktop.UDisks2.Block", block_props("/dev/sdb", kUsb, 16000000000ull, "", "", "", false)},
        {"org.freedesktop.UDisks2.PartitionTable", {{"Type", V::str("gpt")}}},
    });
    fake.add_object(kSdb1, {
        {"org.freedesktop.UDisks2.Block", block_props("/dev/sdb1", kUsb, 15999000000ull, "filesystem", "vfat", "STICK", false)},
        {"org.freedesktop.UDisks2.Partition", {
            {"Number", V::u32(1)}, {"Table", V::obj(kSdb)}, {"Type", V::str("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")},
            {"Name", V::str("data")}, {"UUID", V::str("part-uuid")}, {"Offset", V::u64(1048576)},
        }},
        {"org.freedesktop.UDisks2.Filesystem", {{"MountPoints", Fake::bytestrings({})}}},
    });
    fake.add_object(kNvmeP1, {
        {"org.freedesktop.UDisks2.Block", block_props("/dev/nvme0n1p1", kNvme, 1000000000ull, "filesystem", "ext4", "root", true)},
        {"org.freedesktop.UDisks2.Filesystem", {{"MountPoints", Fake::bytestrings({"/", "/tmp/x"})}}},
    });
    fake.add_object(kLoop0, {
        {"org.freedesktop.UDisks2.Block", block_props("/dev/loop0", "", 1048576, "filesystem", "ext4", "img", false)},
        {"org.freedesktop.UDisks2.Loop", {{"BackingFile", Fake::bytestring("/tmp/disk.img")}}},
        {"org.freedesktop.UDisks2.Filesystem", {{"MountPoints", Fake::bytestrings({})}}},
    });

    // NVMe: as halo's Lexar answers. ATA: a few attributes, one failing.
    fake.set_smart_attributes(kNvme, V::vardict({
        {"avail_spare", V::byte(100)}, {"spare_thresh", V::byte(10)}, {"percent_used", V::byte(1)},
        {"total_data_read", V::u64(4796592640000ull)}, {"total_data_written", V::u64(11743680512000ull)},
        {"power_cycles", V::u64(9)}, {"unsafe_shutdowns", V::u64(0)}, {"media_errors", V::u64(0)},
        {"temp_sensors", V::array("q", {V::u16(305), V::u16(296)})}, {"wctemp", V::u16(363)},
    }));
    auto ata_attr = [](uint8_t id, const char* name, int32_t value, int32_t worst, int32_t thresh, int64_t pretty,
                       int32_t unit) {
        return V::structure({V::byte(id), V::str(name), V::u16(0), V::i32(value), V::i32(worst), V::i32(thresh),
                             V::i64(pretty), V::i32(unit), V::vardict({})});
    };
    fake.set_smart_attributes(kAta, V::array("(ysqiiixia{sv})", {
        ata_attr(5, "reallocated-sector-count", 100, 100, 5, 8, 3),
        ata_attr(9, "power-on-hours", 98, 98, 0, 1234ll * 3600 * 1000, 2),
        ata_attr(12, "power-cycle-count", 100, 100, 0, 77, 1),
        ata_attr(194, "temperature-celsius-2", 3, 3, 10, 311150, 4),
    }));
}

template <class T>
bool has(const std::vector<T>& v, const std::string& id) {
    return std::any_of(v.begin(), v.end(), [&](const T& x) { return x.id == id; });
}

void test_snapshot(DiskService& s) {
    auto drives = s.drives();
    CHECK_EQ(drives.size(), size_t(3));
    auto usb = s.drive(kUsb);
    REQUIRE(usb);
    CHECK_EQ(usb->model, std::string("DataTraveler"));
    CHECK_EQ(usb->connection_bus, std::string("usb"));
    CHECK(usb->removable && usb->ejectable && usb->can_power_off && usb->media_available);
    CHECK_EQ(usb->size, uint64_t(16000000000ull));
    CHECK_EQ(usb->kind, std::string(""));
    CHECK(!usb->smart_supported);
    auto nvme = s.drive(kNvme);
    REQUIRE(nvme);
    CHECK_EQ(nvme->kind, std::string("nvme"));
    CHECK(nvme->smart_supported);
    CHECK_EQ(nvme->rotation_rate, 0);
    CHECK_EQ(s.drive(kAta)->kind, std::string("ata"));

    CHECK_EQ(s.blocks().size(), size_t(4));
    auto sdb1 = s.block("/dev/sdb1");
    REQUIRE(sdb1);
    CHECK_EQ(sdb1->id, kSdb1);
    CHECK_EQ(sdb1->drive, kUsb);
    CHECK_EQ(sdb1->id_type, std::string("vfat"));
    CHECK_EQ(sdb1->id_label, std::string("STICK"));
    CHECK(sdb1->is_partition && sdb1->is_filesystem && !sdb1->is_partition_table);
    CHECK_EQ(sdb1->partition_number, uint32_t(1));
    CHECK_EQ(sdb1->partition_table, kSdb);
    CHECK_EQ(sdb1->partition_offset, uint64_t(1048576));
    CHECK(sdb1->mount_points.empty());
    auto sdb = s.block(kSdb);
    REQUIRE(sdb);
    CHECK(sdb->is_partition_table && !sdb->is_filesystem);
    CHECK_EQ(sdb->partition_table_type, std::string("gpt"));
    auto root = s.block("/dev/nvme0n1p1");
    REQUIRE(root);
    CHECK(root->hint_system);
    CHECK((root->mount_points == std::vector<std::string>{"/", "/tmp/x"}));
    auto loop = s.block(kLoop0);
    REQUIRE(loop);
    CHECK(loop->is_loop);
    CHECK_EQ(loop->loop_backing_file, std::string("/tmp/disk.img"));
    CHECK(loop->drive.empty());
    CHECK(loop->crypto_backing_device.empty());
    CHECK(!s.block("/dev/nope"));
}

void test_mount_cycle(DiskService& s, Fake& fake, Log& log) {
    std::string where;
    Result r = s.mount("/dev/sdb1", {"vfat", "ro"}, &where);
    REQUIRE(r.ok);
    CHECK_EQ(where, std::string("/run/media/fake/STICK"));
    auto ev = log.wait<BlockMountChanged>([](const BlockMountChanged& e) { return e.block.id == kSdb1; });
    REQUIRE(ev);
    CHECK(ev->previous.empty());
    CHECK((ev->block.mount_points == std::vector<std::string>{"/run/media/fake/STICK"}));
    CHECK(log.wait<BlockChanged>([](const BlockChanged& e) { return e.block.id == kSdb1; }).has_value());
    CHECK_EQ(s.block(kSdb1)->mount_points.size(), size_t(1));

    // Twice: UDisks' own refusal, by name.
    r = s.mount(kSdb1, {}, &where);
    CHECK(!r.ok);
    CHECK(r.error.find("AlreadyMounted") != std::string::npos);

    r = s.unmount(kSdb1, true);
    REQUIRE(r.ok);
    ev = log.wait<BlockMountChanged>([](const BlockMountChanged& e) { return e.block.id == kSdb1; });
    REQUIRE(ev);
    CHECK(ev->block.mount_points.empty());
    CHECK((ev->previous == std::vector<std::string>{"/run/media/fake/STICK"}));
    r = s.unmount(kSdb1, false);
    CHECK(!r.ok && r.error.find("NotMounted") != std::string::npos);

    // Not a filesystem.
    CHECK(!s.mount(kSdb, {}, &where).ok);
    CHECK(!s.unmount("/dev/nope", false).ok);

    // A refusal from polkit carries its error name.
    fake.fail_next("Mount", "org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain", "Not authorized to perform operation");
    r = s.mount(kSdb1, {}, &where);
    CHECK(!r.ok);
    CHECK(r.error.find("NotAuthorizedCanObtain") != std::string::npos);

    auto calls = fake.calls();
    CHECK(std::find(calls.begin(), calls.end(), "Mount(" + kSdb1 + ",vfat,ro)") != calls.end());
    CHECK(std::find(calls.begin(), calls.end(), "Unmount(" + kSdb1 + ",force)") != calls.end());
}

void test_eject_and_power_off(DiskService& s, Fake& fake, Log& log) {
    std::string where;
    REQUIRE(s.mount(kSdb1, {}, &where).ok);
    REQUIRE(log.wait<BlockMountChanged>([](const BlockMountChanged& e) { return !e.block.mount_points.empty(); }));

    // Eject unmounts what is mounted first.
    const size_t before = fake.calls().size();
    REQUIRE(s.eject(kUsb).ok);
    auto calls = fake.calls();
    REQUIRE(calls.size() == before + 2);
    CHECK_EQ(calls[before], "Unmount(" + kSdb1 + ")");
    CHECK_EQ(calls[before + 1], "Eject(" + kUsb + ")");
    auto changed = log.wait<DriveChanged>([](const DriveChanged& e) { return e.drive.id == kUsb; });
    REQUIRE(changed);
    CHECK(!changed->drive.media_available);
    CHECK(!s.eject("/org/freedesktop/UDisks2/drives/none").ok);

    // A device plugged in.
    const std::string kSdc = "/org/freedesktop/UDisks2/block_devices/sdc";
    fake.add_object(kSdc, {{"org.freedesktop.UDisks2.Block",
                            block_props("/dev/sdc", kUsb, 1000, "filesystem", "exfat", "CARD", false)},
                           {"org.freedesktop.UDisks2.Filesystem", {{"MountPoints", Fake::bytestrings({})}}}});
    auto added = log.wait<BlockAdded>([&](const BlockAdded& e) { return e.block.id == kSdc; });
    REQUIRE(added);
    CHECK_EQ(added->block.id_type, std::string("exfat"));
    CHECK(s.block("/dev/sdc").has_value());

    // Power off: the drive and its devices go.
    REQUIRE(s.power_off(kUsb).ok);
    CHECK(log.wait<DriveRemoved>([](const DriveRemoved& e) { return e.id == kUsb; }).has_value());
    CHECK(log.wait<BlockRemoved>([](const BlockRemoved& e) { return e.id == kSdb1; }).has_value());
    CHECK(log.wait<BlockRemoved>([&](const BlockRemoved& e) { return e.id == kSdc; }).has_value());
    CHECK(!has(s.drives(), kUsb));
    CHECK(!s.block(kSdb1));
    CHECK(s.block(kNvmeP1).has_value());
}

void test_smart(DiskService& s, Fake& fake) {
    DiskSmart n;
    REQUIRE(s.smart(kNvme, n).ok);
    CHECK_EQ(n.kind, std::string("nvme"));
    CHECK(n.supported && n.enabled && !n.failing);
    CHECK_EQ(n.assessment, std::string("good"));
    REQUIRE(n.temperature_c);
    CHECK(std::fabs(*n.temperature_c - 31.85) < 0.01);
    CHECK_EQ(n.power_on_hours.value_or(0), uint64_t(28));
    CHECK_EQ(n.percent_used.value_or(-1), 1);
    CHECK_EQ(n.available_spare.value_or(-1), 100);
    CHECK_EQ(n.spare_threshold.value_or(-1), 10);
    CHECK_EQ(n.power_cycles.value_or(0), uint64_t(9));
    CHECK_EQ(n.bytes_written.value_or(0), uint64_t(11743680512000ull));
    CHECK_EQ(n.updated, uint64_t(1793783075));
    CHECK(!n.attributes.empty());
    bool wc = false;
    for (auto& a : n.attributes) wc = wc || (a.name == "wctemp" && a.raw == 363 && a.unit == "K");
    CHECK(wc);

    // A critical warning makes it failing; temperature alone a warning.
    fake.set_property(kNvme, "org.freedesktop.UDisks2.NVMe.Controller", "SmartCriticalWarning", V::strings({"temperature"}));
    REQUIRE(bstest::wait_until([&] {
        DiskSmart x;
        return s.smart(kNvme, x).ok && x.assessment == "warning";
    }, 3s));
    fake.set_property(kNvme, "org.freedesktop.UDisks2.NVMe.Controller", "SmartCriticalWarning",
                      V::strings({"temperature", "degraded"}));
    REQUIRE(bstest::wait_until([&] {
        DiskSmart x;
        return s.smart(kNvme, x).ok && x.assessment == "failing" && x.failing && x.critical_warnings.size() == 2;
    }, 3s));

    DiskSmart a;
    REQUIRE(s.smart(kAta, a).ok);
    CHECK_EQ(a.kind, std::string("ata"));
    CHECK_EQ(a.assessment, std::string("warning"));  // 8 bad sectors
    CHECK_EQ(a.bad_sectors.value_or(-1), int64_t(8));
    CHECK_EQ(a.power_on_hours.value_or(0), uint64_t(1234));
    REQUIRE(a.temperature_c);
    CHECK(std::fabs(*a.temperature_c - 38.0) < 0.01);
    CHECK_EQ(a.power_cycles.value_or(0), uint64_t(77));
    REQUIRE(a.attributes.size() == 4);
    CHECK_EQ(a.attributes[0].id, 5);
    CHECK_EQ(a.attributes[0].name, std::string("reallocated-sector-count"));
    CHECK_EQ(a.attributes[0].unit, std::string("sectors"));
    CHECK_EQ(a.attributes[0].threshold, 5);
    CHECK(!a.attributes[0].failing);
    CHECK_EQ(a.attributes[1].unit, std::string("ms"));
    CHECK(a.attributes[3].failing);  // 3 <= 10
    CHECK_EQ(a.attributes[3].unit, std::string("mK"));

    fake.set_property(kAta, "org.freedesktop.UDisks2.Drive.Ata", "SmartFailing", V::boolean(true));
    REQUIRE(bstest::wait_until([&] {
        DiskSmart x;
        return s.smart(kAta, x).ok && x.failing && x.assessment == "failing";
    }, 3s));

    DiskSmart none;
    CHECK(!s.smart("/org/freedesktop/UDisks2/drives/none", none).ok);
}

void test_udisks_goes_away(const std::string& address) {
    auto fake = std::make_unique<Fake>(address);
    REQUIRE(fake->ok());
    populate(*fake);
    DiskConfig cfg;
    cfg.system_bus_address = address;
    std::string err;
    auto s = DiskService::create(cfg, &err);
    REQUIRE(s);
    Log log(s->events());
    CHECK_EQ(s->drives().size(), size_t(3));
    fake.reset();
    auto gone = log.wait<DisksAvailability>([](const DisksAvailability& e) { return !e.available; });
    CHECK(gone.has_value());
    CHECK(s->drives().empty());
    CHECK(s->blocks().empty());

    fake = std::make_unique<Fake>(address);
    REQUIRE(fake->ok());
    populate(*fake);
    auto back = log.wait<DisksAvailability>([](const DisksAvailability& e) { return e.available; });
    CHECK(back.has_value());
    CHECK(bstest::wait_until([&] { return s->drives().size() == 3; }, 3s));
}

}  // namespace

int main() {
    bstest::PrivateBus bus;
    if (!bus.ok()) bstest::skip("test_disks_fake", "no dbus-daemon: " + bus.error());

    {
        std::string err;
        DiskConfig cfg;
        cfg.system_bus_address = bus.address();
        CHECK(DiskService::create(cfg, &err) == nullptr);  // nobody owns org.freedesktop.UDisks2
        CHECK(!err.empty());
    }

    {
        Fake fake(bus.address());
        if (!fake.ok()) {
            std::fprintf(stderr, "fake UDisks2: %s\n", fake.error().c_str());
            return 1;
        }
        populate(fake);
        DiskConfig cfg;
        cfg.system_bus_address = bus.address();
        std::string err;
        auto s = DiskService::create(cfg, &err);
        if (!s) {
            std::fprintf(stderr, "create: %s\n", err.c_str());
            return 1;
        }
        Log log(s->events());
        test_snapshot(*s);
        test_mount_cycle(*s, fake, log);
        test_smart(*s, fake);
        test_eject_and_power_off(*s, fake, log);
    }
    test_udisks_goes_away(bus.address());
    return bstest::finish("test_disks_fake");
}
