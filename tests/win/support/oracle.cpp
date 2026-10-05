#include "win/support/oracle.h"

#include <windows.h>
#include <comdef.h>
#include <wbemidl.h>

#include <algorithm>
#include <cctype>

#pragma comment(lib, "wbemuuid.lib")

namespace bstest::win {

namespace {

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string variant_text(VARIANT& v) {
    if (v.vt == VT_NULL || v.vt == VT_EMPTY) return {};
    if (v.vt & VT_ARRAY) {
        SAFEARRAY* sa = v.parray;
        LONG lo = 0, hi = -1;
        SafeArrayGetLBound(sa, 1, &lo);
        SafeArrayGetUBound(sa, 1, &hi);
        VARTYPE vt = static_cast<VARTYPE>(v.vt & ~VT_ARRAY);
        std::string out;
        for (LONG i = lo; i <= hi; ++i) {
            VARIANT e;
            VariantInit(&e);
            e.vt = vt;
            if (vt == VT_BSTR) {
                BSTR b = nullptr;
                SafeArrayGetElement(sa, &i, &b);
                e.bstrVal = b;
            } else {
                SafeArrayGetElement(sa, &i, &e.lVal);
            }
            if (!out.empty()) out += ",";
            out += variant_text(e);
            VariantClear(&e);
        }
        return out;
    }
    VARIANT s;
    VariantInit(&s);
    std::string out;
    if (SUCCEEDED(VariantChangeType(&s, &v, VARIANT_ALPHABOOL, VT_BSTR))) out = narrow(s.bstrVal);
    VariantClear(&s);
    return out;
}

}  // namespace

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool wmi_query(const wchar_t* ns, const wchar_t* wql, std::vector<WmiRow>& rows, std::string* error) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninit = SUCCEEDED(init);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                         EOAC_NONE, nullptr);
    bool ok = false;
    IWbemLocator* loc = nullptr;
    IWbemServices* svc = nullptr;
    IEnumWbemClassObject* en = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                  reinterpret_cast<void**>(&loc));
    if (SUCCEEDED(hr)) hr = loc->ConnectServer(_bstr_t(ns), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    if (SUCCEEDED(hr))
        hr = CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                               RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (SUCCEEDED(hr))
        hr = svc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(wql), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                            nullptr, &en);
    if (SUCCEEDED(hr)) {
        ok = true;
        while (true) {
            IWbemClassObject* obj = nullptr;
            ULONG n = 0;
            if (en->Next(WBEM_INFINITE, 1, &obj, &n) != WBEM_S_NO_ERROR || n == 0) break;
            WmiRow row;
            obj->BeginEnumeration(WBEM_FLAG_NONSYSTEM_ONLY);
            BSTR name = nullptr;
            VARIANT val;
            VariantInit(&val);
            while (obj->Next(0, &name, &val, nullptr, nullptr) == WBEM_S_NO_ERROR) {
                row[narrow(name)] = variant_text(val);
                SysFreeString(name);
                VariantClear(&val);
            }
            obj->EndEnumeration();
            obj->Release();
            rows.push_back(std::move(row));
        }
    } else if (error) {
        char buf[32];
        snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
        *error = std::string("WMI query failed: ") + buf;
    }
    if (en) en->Release();
    if (svc) svc->Release();
    if (loc) loc->Release();
    if (uninit) CoUninitialize();
    return ok;
}

ToolResult run_tool(const std::wstring& command_line, std::chrono::milliseconds timeout) {
    ToolResult res;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return res;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_line;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return res;
    }
    CloseHandle(wr);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[4096];
    while (true) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // writer closed
        if (avail) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, sizeof buf, &got, nullptr) || !got) break;
            res.out.append(buf, got);
            continue;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        Sleep(10);
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code)) res.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return res;
}

}  // namespace bstest::win
