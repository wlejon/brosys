// The journal and disk services' portable parts, and their create() where
// there is no backend (the systemd journal and UDisks2 are Linux's): nullptr
// with a reason, never an empty service that looks working.
#include "brosys/disks.h"
#include "brosys/journal.h"

#include <cctype>

namespace brosys {

int32_t parse_journal_priority(const std::string& name) {
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '7') return name[0] - '0';
    std::string n;
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* const kNames[][2] = {
        {"emerg", "panic"}, {"alert", "alert"}, {"crit", "critical"}, {"err", "error"},
        {"warning", "warn"}, {"notice", "notice"}, {"info", "informational"}, {"debug", "debug"},
    };
    for (int i = 0; i < 8; ++i)
        if (n == kNames[i][0] || n == kNames[i][1]) return i;
    return -1;
}

#if !defined(__linux__)

std::unique_ptr<JournalService> JournalService::create(const JournalConfig&, std::string* error) {
    if (error) *error = "the systemd journal exists only on Linux";
    return nullptr;
}

std::unique_ptr<DiskService> DiskService::create(const DiskConfig&, std::string* error) {
    if (error) *error = "disk management (UDisks2) is implemented only on Linux";
    return nullptr;
}

#endif

}  // namespace brosys
