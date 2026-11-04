// DiskService against the machine's real UDisks2.
//
// Read-only by default: every block device UDisks reports exists under
// /sys/class/block, mount points agree with /proc/self/mountinfo, and each
// drive with SMART answers smart() with plausible values (UDisks' cached
// reading: nothing is sent to the drive).
//
// With BROSYS_TEST_MUTATE=1 (or as root, which polkit allows everything) it
// also mounts and unmounts a filesystem: on a loop device over a scratch
// file it creates in /tmp and removes again (LoopSetup, Mount, Unmount,
// Loop.Delete). No real disk is touched either way.
#include "check.h"
#include "brosys/disks.h"
#include "linux/dbus/connection.h"
#include "linux/desktop/event_log.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

using namespace brosys;
using namespace std::chrono_literals;
using Log = bstest::EventLog<DiskEvent>;

namespace {

bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

// mountinfo's octal escapes (\040 for a space) undone.
std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            const std::string oct = s.substr(i + 1, 3);
            if (oct.find_first_not_of("01234567") == std::string::npos) {
                out += static_cast<char>(std::stoi(oct, nullptr, 8));
                i += 3;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::set<std::string> mount_points() {
    std::set<std::string> out;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string id, parent, dev, root, mp;
        ls >> id >> parent >> dev >> root >> mp;
        out.insert(unescape(mp));
    }
    return out;
}

void test_read_only(DiskService& s) {
    auto blocks = s.blocks();
    auto drives = s.drives();
    std::printf("UDisks: %zu drives, %zu block devices\n", drives.size(), blocks.size());
    CHECK(!blocks.empty());
    const auto mounted = mount_points();
    for (auto& b : blocks) {
        CHECK(b.device.rfind("/dev/", 0) == 0);
        const std::string name = b.device.substr(5);
        if (!exists("/sys/class/block/" + name)) bstest::fail(__FILE__, __LINE__, "no /sys/class/block/" + name);
        if (!b.drive.empty() && !s.drive(b.drive)) bstest::fail(__FILE__, __LINE__, b.id + "'s drive is unknown");
        for (auto& mp : b.mount_points)
            if (!mounted.count(mp)) bstest::fail(__FILE__, __LINE__, b.device + " at " + mp + " is not in mountinfo");
        if (b.is_partition) CHECK(s.block(b.partition_table).has_value());
    }
    for (auto& d : drives) {
        std::printf("  %s: %s %s, %llu bytes, kind '%s'\n", d.id.c_str(), d.vendor.c_str(), d.model.c_str(),
                    static_cast<unsigned long long>(d.size), d.kind.c_str());
        if (!d.smart_supported) continue;
        DiskSmart smart;
        Result r = s.smart(d.id, smart);
        CHECK(r.ok);
        if (!r.ok) {
            std::fprintf(stderr, "smart(%s): %s\n", d.id.c_str(), r.error.c_str());
            continue;
        }
        std::printf("    SMART %s: %s, %.1f C, %llu h, %d%% used, %zu attributes\n", smart.kind.c_str(),
                    smart.assessment.c_str(), smart.temperature_c.value_or(-1),
                    static_cast<unsigned long long>(smart.power_on_hours.value_or(0)), smart.percent_used.value_or(-1),
                    smart.attributes.size());
        CHECK_EQ(smart.kind, d.kind);
        CHECK(smart.assessment == "good" || smart.assessment == "warning" || smart.assessment == "failing" ||
              smart.assessment == "unknown");
        if (smart.temperature_c) CHECK(*smart.temperature_c > -20 && *smart.temperature_c < 120);
        if (smart.kind == "nvme") {
            CHECK(smart.percent_used.has_value());
            CHECK(smart.available_spare.has_value());
            CHECK(!smart.attributes.empty());
        }
    }
}

// A loop device over a fresh ext4 image: mount, unmount, delete.
void test_loop_mount(DiskService& s) {
    // A kernel without a loop driver (none built in, its module missing, as
    // after a kernel upgrade without a reboot) has nothing to set up.
    if (const int ctl = ::open("/dev/loop-control", O_RDWR | O_CLOEXEC); ctl < 0) {
        std::printf("no loop driver (/dev/loop-control: %s): loop mount not exercised\n", std::strerror(errno));
        return;
    } else {
        ::close(ctl);
    }
    char path[] = "/tmp/brosys-disks-XXXXXX";
    const int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);
    const std::string image = path;
    REQUIRE(::ftruncate(fd, 32 * 1024 * 1024) == 0);
    ::close(fd);
    const std::string mkfs = "mkfs.ext4 -q -F -L brosystest '" + image + "' >/dev/null 2>&1";
    if (std::system(mkfs.c_str()) != 0) {
        ::unlink(path);
        std::printf("mkfs.ext4 unavailable: loop mount not exercised\n");
        return;
    }

    std::string err;
    auto conn = dbus::Connection::open(dbus::BusKind::System, "", "brosys-disks-test", &err);
    REQUIRE(conn);
    const int rw = ::open(path, O_RDWR | O_CLOEXEC);
    REQUIRE(rw >= 0);
    dbus::Reply setup = conn->call("org.freedesktop.UDisks2", "/org/freedesktop/UDisks2/Manager",
                                   "org.freedesktop.UDisks2.Manager", "LoopSetup",
                                   {dbus::Value::fd(dbus::UnixFd::adopt(rw)), dbus::Value::vardict({})});
    if (!setup.ok) {
        ::unlink(path);
        bstest::fail(__FILE__, __LINE__, "LoopSetup: " + setup.error());
        return;
    }
    const std::string loop = setup.first()->as_string();
    std::printf("loop device %s over %s\n", loop.c_str(), image.c_str());
    auto cleanup = [&] {
        conn->call("org.freedesktop.UDisks2", loop, "org.freedesktop.UDisks2.Loop", "Delete",
                   {dbus::Value::vardict({})});
        ::unlink(path);
    };

    Log log(s.events());
    // UDisks probes the new device before it is a filesystem.
    if (!bstest::wait_until([&] {
            auto b = s.block(loop);
            return b && b->is_filesystem;
        }, 10s, 50ms)) {
        bstest::fail(__FILE__, __LINE__, "the loop device never showed a filesystem");
        cleanup();
        return;
    }
    auto b = s.block(loop);
    CHECK(b->is_loop);
    CHECK_EQ(b->loop_backing_file, image);
    CHECK_EQ(b->id_type, std::string("ext4"));
    CHECK_EQ(b->id_label, std::string("brosystest"));

    std::string where;
    Result r = s.mount(loop, {"", "ro"}, &where);
    CHECK(r.ok);
    if (r.ok) {
        std::printf("mounted at %s\n", where.c_str());
        CHECK(exists(where));
        CHECK(mount_points().count(where) == 1);
        CHECK(log.wait<BlockMountChanged>([&](const BlockMountChanged& e) {
            return e.block.id == loop && !e.block.mount_points.empty();
        }, 5s).has_value());
        r = s.unmount(b->device, false);  // by device path
        CHECK(r.ok);
        CHECK(log.wait<BlockMountChanged>([&](const BlockMountChanged& e) {
            return e.block.id == loop && e.block.mount_points.empty();
        }, 5s).has_value());
        CHECK(mount_points().count(where) == 0);
    } else {
        std::fprintf(stderr, "mount: %s\n", r.error.c_str());
    }
    cleanup();
    CHECK(log.wait<BlockRemoved>([&](const BlockRemoved& e) { return e.id == loop; }, 5s).has_value() ||
          !s.block(loop) || !s.block(loop)->is_filesystem);
}

}  // namespace

int main() {
    std::string err;
    auto s = DiskService::create({}, &err);
    if (!s) bstest::skip("test_disks_system", "no UDisks2 on the system bus: " + err);
    test_read_only(*s);
    if (bstest::mutate_opted_in() || ::geteuid() == 0) test_loop_mount(*s);
    else std::printf("Note: the loop-device mount was not exercised; set BROSYS_TEST_MUTATE=1 or run as root\n");
    return bstest::finish("test_disks_system");
}
