// Small Win32 helpers shared by the Windows backends.
#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace brosys::win {

std::string to_utf8(std::wstring_view w);
std::wstring to_wide(std::string_view s);

// "<what> failed: <FormatMessage text> (0x%08X)".
std::string win32_error(const char* what, DWORD code);
std::string hresult_error(const char* what, HRESULT hr);

// COM apartment for the calling thread (MTA); balanced in the destructor.
class ComInit {
public:
    ComInit();
    ~ComInit();
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
    bool uninit_ = false;
};

}  // namespace brosys::win
