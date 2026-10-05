#include "win/util.h"

#include <objbase.h>

#include <cstdio>

namespace brosys::win {

std::string to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

static std::string format_message(DWORD code) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string text;
    if (n && buf) {
        text = to_utf8(std::wstring_view(buf, n));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.'))
            text.pop_back();
    }
    if (buf) LocalFree(buf);
    return text;
}

std::string win32_error(const char* what, DWORD code) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08lX", static_cast<unsigned long>(code));
    return std::string(what) + " failed: " + format_message(code) + " (" + hex + ")";
}

std::string hresult_error(const char* what, HRESULT hr) { return win32_error(what, static_cast<DWORD>(hr)); }

ComInit::ComInit() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uninit_ = SUCCEEDED(hr);
    ok_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

ComInit::~ComInit() {
    if (uninit_) CoUninitialize();
}

}  // namespace brosys::win
