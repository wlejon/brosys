// NetworkService change events from a real adapter that the test itself
// creates and removes: a Microsoft KM-TEST Loopback Adapter (hardware id
// *MSLOOP, the inbox %windir%\INF\netloop.inf) is installed through SetupAPI
// on a fresh device node, which this test alone owns. Its arrival, its
// address, and its removal must reach the service as NetworkChanged events,
// cross-checked with WMI (MSFT_NetAdapter / MSFT_NetIPAddress). Nothing
// else is configured; the device node is removed even when a check fails.
//
// Installing a device needs an elevated process: unelevated, the test skips.
#include "check.h"
#include "win/event_log.h"
#include "win/support/oracle.h"

#include "brosys/network.h"

#include <windows.h>
#include <devguid.h>
#include <iphlpapi.h>
#include <newdev.h>
#include <objbase.h>
#include <setupapi.h>

#include <set>

using namespace brosys;
using namespace std::chrono_literals;

namespace {

constexpr const char* kName = "test_win_network_adapter";
constexpr const char* kDescription = "Microsoft KM-TEST Loopback Adapter";

bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    bool yes = GetTokenInformation(token, TokenElevation, &e, sizeof e, &size) && e.TokenIsElevated;
    CloseHandle(token);
    return yes;
}

std::string last_error(const char* what) { return std::string(what) + " failed: " + std::to_string(GetLastError()); }

// One KM-TEST loopback device node, installed with the inbox driver only on
// this node (DiInstallDevice, not a hardware-id-wide driver update, so an
// existing loopback adapter of the user's is never touched).
class TestAdapter {
public:
    bool create(std::string* error) {
        wchar_t inf[MAX_PATH];
        if (!GetWindowsDirectoryW(inf, MAX_PATH)) return fail(error, "GetWindowsDirectory");
        wcscat_s(inf, L"\\INF\\netloop.inf");
        set_ = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, nullptr);
        if (set_ == INVALID_HANDLE_VALUE) return fail(error, "SetupDiCreateDeviceInfoList");
        info_.cbSize = sizeof info_;
        if (!SetupDiCreateDeviceInfoW(set_, L"Net", &GUID_DEVCLASS_NET, nullptr, nullptr, DICD_GENERATE_ID, &info_))
            return fail(error, "SetupDiCreateDeviceInfo");
        static const wchar_t kHwid[] = L"*MSLOOP\0";  // REG_MULTI_SZ: the literal adds the second NUL
        if (!SetupDiSetDeviceRegistryPropertyW(set_, &info_, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(kHwid),
                                               sizeof kHwid))
            return fail(error, "SetupDiSetDeviceRegistryProperty");
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set_, &info_)) return fail(error, "DIF_REGISTERDEVICE");
        registered_ = true;
        SP_DEVINSTALL_PARAMS_W params{};
        params.cbSize = sizeof params;
        if (!SetupDiGetDeviceInstallParamsW(set_, &info_, &params)) return fail(error, "SetupDiGetDeviceInstallParams");
        params.Flags |= DI_ENUMSINGLEINF;
        wcscpy_s(params.DriverPath, inf);
        if (!SetupDiSetDeviceInstallParamsW(set_, &info_, &params)) return fail(error, "SetupDiSetDeviceInstallParams");
        if (!SetupDiBuildDriverInfoList(set_, &info_, SPDIT_COMPATDRIVER)) return fail(error, "SetupDiBuildDriverInfoList");
        SP_DRVINFO_DATA_W drv{};
        drv.cbSize = sizeof drv;
        if (!SetupDiEnumDriverInfoW(set_, &info_, SPDIT_COMPATDRIVER, 0, &drv)) return fail(error, "SetupDiEnumDriverInfo");
        if (!SetupDiSetSelectedDriverW(set_, &info_, &drv)) return fail(error, "SetupDiSetSelectedDriver");
        BOOL reboot = FALSE;
        if (!DiInstallDevice(nullptr, set_, &info_, &drv, 0, &reboot)) return fail(error, "DiInstallDevice");
        return true;
    }

    bool remove(std::string* error = nullptr) {
        if (!registered_) return true;
        BOOL reboot = FALSE;
        if (!DiUninstallDevice(nullptr, set_, &info_, 0, &reboot) &&
            !SetupDiCallClassInstaller(DIF_REMOVE, set_, &info_))
            return fail(error, "DiUninstallDevice / DIF_REMOVE");
        registered_ = false;
        return true;
    }

    ~TestAdapter() {
        remove();
        if (set_ != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set_);
    }

private:
    static bool fail(std::string* error, const char* what) {
        if (error) *error = last_error(what);
        return false;
    }

    HDEVINFO set_ = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA info_{};
    bool registered_ = false;
};

ULONG if_index(const std::string& luid_text) {
    NET_LUID luid{};
    luid.Value = std::stoull(luid_text);
    NET_IFINDEX idx = 0;
    ConvertInterfaceLuidToIndex(&luid, &idx);
    return idx;
}

std::vector<bstest::win::WmiRow> wmi(const wchar_t* q) {
    std::vector<bstest::win::WmiRow> rows;
    std::string err;
    if (!bstest::win::wmi_query(L"ROOT\\StandardCimv2", q, rows, &err)) std::printf("note: %s\n", err.c_str());
    return rows;
}

const NetDevice* new_loopback(const NetworkState& s, const std::set<std::string>& before) {
    for (auto& d : s.devices)
        if (!before.count(d.id) && d.description.rfind(kDescription, 0) == 0) return &d;
    return nullptr;
}

void run_test() {
    std::string err;
    auto svc = NetworkService::create(NetworkConfig{}, &err);
    REQUIRE(svc != nullptr);
    bstest::EventLog<NetworkEvent> log(svc->events());
    std::set<std::string> before;
    for (auto& d : svc->state().devices) before.insert(d.id);
    size_t m = log.mark();

    TestAdapter adapter;
    if (!adapter.create(&err)) {
        bstest::fail(__FILE__, __LINE__, "creating the test adapter: " + err);
        return;
    }
    // ---- arrival
    std::string id;
    auto ev = log.wait<NetworkChanged>(
        [&](const NetworkChanged& c) {
            const NetDevice* d = new_loopback(c.state, before);
            if (d) id = d->id;
            return d != nullptr;
        },
        m, 60s);
    REQUIRE(ev.has_value());
    std::printf("test adapter arrived: id %s ifindex %lu '%s'\n", id.c_str(), if_index(id),
                new_loopback(ev->state, before)->interface_name.c_str());
    ULONG idx = if_index(id);
    CHECK(idx != 0);
    auto rows = wmi(L"SELECT InterfaceIndex, InterfaceDescription FROM MSFT_NetAdapter");
    bool in_wmi = false;
    for (auto& r : rows) in_wmi |= std::stoul(r["InterfaceIndex"]) == idx && r["InterfaceDescription"].rfind(kDescription, 0) == 0;
    CHECK(in_wmi);

    // ---- its (link-local) IPv4 address arrives as a change too, as WMI lists it
    auto with_addr = log.wait<NetworkChanged>(
        [&](const NetworkChanged& c) {
            for (auto& d : c.state.devices)
                if (d.id == id && !d.ipv4.addresses.empty()) return true;
            return false;
        },
        m, 60s);
    if (with_addr) {
        std::set<std::string> ours;
        for (auto& d : with_addr->state.devices)
            if (d.id == id) ours.insert(d.ipv4.addresses.begin(), d.ipv4.addresses.end());
        std::wstring q = L"SELECT IPAddress, PrefixLength FROM MSFT_NetIPAddress WHERE AddressFamily = 2 AND InterfaceIndex = " +
                         std::to_wstring(idx);
        std::set<std::string> theirs;
        // WMI may still show the address it is about to replace: compare once both settle.
        CHECK(bstest::wait_until([&] {
            theirs.clear();
            for (auto& r : wmi(q.c_str())) theirs.insert(r["IPAddress"] + "/" + r["PrefixLength"]);
            ours.clear();
            for (auto& d : svc->state().devices)
                if (d.id == id) ours.insert(d.ipv4.addresses.begin(), d.ipv4.addresses.end());
            return !ours.empty() && ours == theirs;
        }, 30000ms));
    } else {
        std::printf("note: the test adapter got no IPv4 address within 60 s (address change not exercised)\n");
    }

    // ---- removal
    m = log.mark();
    REQUIRE(adapter.remove(&err));
    auto gone = log.wait<NetworkChanged>(
        [&](const NetworkChanged& c) {
            for (auto& d : c.state.devices)
                if (d.id == id) return false;
            return true;
        },
        m, 60s);
    CHECK(gone.has_value());
    rows = wmi(L"SELECT InterfaceIndex, InterfaceDescription FROM MSFT_NetAdapter");
    for (auto& r : rows) CHECK(!(std::stoul(r["InterfaceIndex"]) == idx && r["InterfaceDescription"].rfind(kDescription, 0) == 0));
    std::set<std::string> after;
    for (auto& d : svc->state().devices) after.insert(d.id);
    CHECK(after == before);  // back to where it started
}

}  // namespace

int main() {
    if (!bstest::mutate_opted_in())
        bstest::skip(kName, "installs and removes a network device on this machine; set BROSYS_TEST_MUTATE=1 to run it");
    if (!elevated())
        bstest::skip(kName, "installing a device (the KM-TEST loopback adapter) needs an elevated process");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    run_test();
    if (SUCCEEDED(hr)) CoUninitialize();
    return bstest::finish(kName);
}
