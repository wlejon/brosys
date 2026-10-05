// Independent sources of truth for the Windows tests: WMI queries and the
// output of the OS's own command-line tools.
#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace bstest::win {

using WmiRow = std::map<std::string, std::string>;  // property -> value as text ("" for NULL)

// Runs a WQL query in `ns` (e.g. L"ROOT\\CIMV2"). Arrays are joined with ",".
// Returns false (and *error) when WMI is unavailable.
bool wmi_query(const wchar_t* ns, const wchar_t* wql, std::vector<WmiRow>& rows, std::string* error);

struct ToolResult {
    int exit_code = -1;
    std::string out;  // stdout + stderr, OEM code page as returned
};

// Runs a command line (no shell) and captures its output; killed after `timeout`.
ToolResult run_tool(const std::wstring& command_line, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));

std::string lower(std::string s);

}  // namespace bstest::win
