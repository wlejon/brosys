// The systemd journal: structured log records, queried and followed.
//
// Linux: libsystemd's sd_journal (the system's journal files: the system
// journal and the user's own, as the user may read them). Elsewhere create()
// returns nullptr with a reason.
//
// query(), boots() and field_values() block on disk reads (a query over a
// large journal can take a while), so a host calls them off its UI thread.
// A follow runs on a thread of its own and posts batches of new records to
// events().
#pragma once

#include "brosys/common.h"
#include "brosys/event_queue.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace brosys {

struct JournalEntry {
    std::string cursor;          // opaque position: before / after a query or follow
    uint64_t realtime_us = 0;    // wall clock, microseconds since the epoch
    uint64_t monotonic_us = 0;   // since its boot
    std::string boot_id;         // 32 hex digits
    int32_t priority = -1;       // syslog 0 (emerg) .. 7 (debug); -1 when the record has none
    std::string message;
    std::string unit;            // _SYSTEMD_UNIT, else _SYSTEMD_USER_UNIT, else UNIT / USER_UNIT
    std::string identifier;      // SYSLOG_IDENTIFIER, else _COMM
    int32_t pid = 0;
    std::string hostname;
    std::string transport;       // _TRANSPORT: "journal", "syslog", "kernel", "stdout", ...
    // Every field of the record ("FIELD", "value"), when JournalQuery::all_fields.
    // Values are UTF-8 (invalid bytes replaced with U+FFFD).
    std::vector<std::pair<std::string, std::string>> fields;
};

struct JournalQuery {
    // Records of any of these units (system or user units; "sshd.service").
    std::vector<std::string> units;
    // Only records with PRIORITY <= this (0..7); -1 keeps every priority.
    int32_t max_priority = -1;
    // "" every boot; "current" or "0" this boot; "-1", "-2", ... the boots
    // before it (as journalctl -b counts them); else a 32-hex-digit boot id.
    std::string boot;
    // Wall-clock bounds in microseconds since the epoch (0: unbounded).
    uint64_t since_us = 0;
    uint64_t until_us = 0;
    // FIELD=value matches: values of one field are alternatives, different
    // fields must all hold ("_COMM" = "sshd", "_PID" = "42").
    std::vector<std::pair<std::string, std::string>> matches;
    // Case-insensitive substring the MESSAGE must contain ("" none).
    std::string text;
    // Paging: only records older than this cursor ("" from the newest).
    std::string before;
    // Follow only: start after this cursor ("" from the end, new records only).
    std::string after;
    // At most this many records per page.
    size_t limit = 200;
    // Fill JournalEntry::fields with every field.
    bool all_fields = false;
};

struct JournalPage {
    std::vector<JournalEntry> entries;  // oldest first
    // The cursor to pass as JournalQuery::before for the next older page
    // (the oldest record this page examined); "" when nothing was examined.
    std::string before;
    // Older records may match (false: the journal or `since` was reached).
    bool more = false;
};

struct JournalBoot {
    std::string id;
    int32_t offset = 0;        // 0 the newest boot, -1 the one before, ...
    uint64_t first_us = 0;     // its first and last records' wall clock
    uint64_t last_us = 0;
};

// New records of a follow, or why the follow stopped (`error` set, `ended`).
struct JournalFollowBatch {
    uint64_t follow_id = 0;
    std::vector<JournalEntry> entries;
    std::string error;
    bool ended = false;
};

using JournalEventQueue = MessageQueue<JournalFollowBatch>;

struct JournalConfig {
    // Read the journal files in this directory instead of the system's.
    std::string directory;
};

class JournalService {
public:
    static std::unique_ptr<JournalService> create(const JournalConfig& config, std::string* error);
    virtual ~JournalService() = default;

    // Blocking (disk reads); safe from any thread, concurrently.
    virtual Result query(const JournalQuery& query, JournalPage& out) = 0;
    virtual std::vector<JournalBoot> boots(std::string* error) = 0;
    // The distinct values of `field` ("_SYSTEMD_UNIT"), sorted.
    virtual std::vector<std::string> field_values(const std::string& field, std::string* error) = 0;

    // Starts following records that match `query` (its units, priority,
    // boot, matches and text; after = where to start). Batches arrive on
    // events() with the returned id. 0 when it could not start (*error).
    virtual uint64_t follow(const JournalQuery& query, std::string* error) = 0;
    virtual void unfollow(uint64_t id) = 0;
    virtual JournalEventQueue& events() = 0;
};

// Syslog priority from a name ("emerg", "alert", "crit", "err"/"error",
// "warning"/"warn", "notice", "info", "debug") or a digit; -1 unknown.
int32_t parse_journal_priority(const std::string& name);

}  // namespace brosys
