// Linux JournalService: libsystemd's sd_journal.
//
// An sd_journal handle is used by one thread at a time, so every query opens
// its own (opening maps the files' headers: a few ms) and every follow owns
// one on its thread. A fresh handle per query also sees files rotated since
// the last one.
#include "brosys/journal.h"

#include <systemd/sd-id128.h>
#include <systemd/sd-journal.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace brosys {

namespace {

// Records scanned per page before it returns what it has (a rare `text`
// over a big journal would otherwise read all of it in one call).
constexpr size_t kScanCap = 200000;
// Records per follow batch.
constexpr size_t kFollowBatch = 500;
// How long a follow waits for the journal to change before checking its
// stop flag.
constexpr uint64_t kFollowWaitUs = 200000;

std::string errno_text(const char* what, int r) {
    return std::string(what) + ": " + std::strerror(r < 0 ? -r : r);
}

// Replaces bytes that are not well-formed UTF-8 with U+FFFD.
std::string sanitize_utf8(const char* s, size_t n) {
    std::string out;
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        uint32_t min = 0;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) { len = 2; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { len = 3; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { len = 4; min = 0x10000; }
        bool ok = len > 0 && i + len <= n;
        uint32_t cp = len == 1 ? c : (len == 2 ? (c & 0x1F) : (len == 3 ? (c & 0x0F) : (c & 0x07)));
        for (size_t k = 1; ok && k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) ok = false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok && len > 1 && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
        if (ok) {
            out.append(s + i, len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

bool contains_ci(const std::string& hay, const std::string& needle_lower) {
    if (needle_lower.empty()) return true;
    if (hay.size() < needle_lower.size()) return false;
    auto it = std::search(hay.begin(), hay.end(), needle_lower.begin(), needle_lower.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) == b;
                          });
    return it != hay.end();
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

class Journal {
public:
    ~Journal() {
        if (j_) sd_journal_close(j_);
    }
    int open(const JournalConfig& config) {
        if (!config.directory.empty()) return sd_journal_open_directory(&j_, config.directory.c_str(), 0);
        return sd_journal_open(&j_, SD_JOURNAL_LOCAL_ONLY);
    }
    sd_journal* get() const { return j_; }

    // The field's value ("" when absent).
    std::string field(const char* name) const {
        const void* data = nullptr;
        size_t len = 0;
        if (sd_journal_get_data(j_, name, &data, &len) < 0 || !data) return {};
        const size_t nl = std::strlen(name);
        if (len <= nl + 1) return {};
        return sanitize_utf8(static_cast<const char*>(data) + nl + 1, len - nl - 1);
    }

    std::string cursor() const {
        char* c = nullptr;
        if (sd_journal_get_cursor(j_, &c) < 0 || !c) return {};
        std::string out(c);
        std::free(c);
        return out;
    }

    uint64_t realtime() const {
        uint64_t t = 0;
        sd_journal_get_realtime_usec(j_, &t);
        return t;
    }

    JournalEntry entry(bool all_fields) const {
        JournalEntry e;
        e.cursor = cursor();
        e.realtime_us = realtime();
        sd_id128_t boot;
        uint64_t mono = 0;
        if (sd_journal_get_monotonic_usec(j_, &mono, &boot) >= 0) {
            e.monotonic_us = mono;
            char s[SD_ID128_STRING_MAX];
            e.boot_id = sd_id128_to_string(boot, s);
        }
        const std::string prio = field("PRIORITY");
        if (prio.size() == 1 && prio[0] >= '0' && prio[0] <= '7') e.priority = prio[0] - '0';
        e.message = field("MESSAGE");
        e.unit = field("_SYSTEMD_UNIT");
        if (e.unit.empty()) e.unit = field("_SYSTEMD_USER_UNIT");
        if (e.unit.empty()) e.unit = field("UNIT");
        if (e.unit.empty()) e.unit = field("USER_UNIT");
        e.identifier = field("SYSLOG_IDENTIFIER");
        if (e.identifier.empty()) e.identifier = field("_COMM");
        e.pid = std::atoi(field("_PID").c_str());
        e.hostname = field("_HOSTNAME");
        e.transport = field("_TRANSPORT");
        if (all_fields) {
            const void* data = nullptr;
            size_t len = 0;
            sd_journal_restart_data(j_);
            while (sd_journal_enumerate_data(j_, &data, &len) > 0) {
                const char* d = static_cast<const char*>(data);
                const void* eq = std::memchr(d, '=', len);
                if (!eq) continue;
                const size_t k = static_cast<size_t>(static_cast<const char*>(eq) - d);
                e.fields.emplace_back(std::string(d, k), sanitize_utf8(d + k + 1, len - k - 1));
            }
        }
        return e;
    }

private:
    sd_journal* j_ = nullptr;
};

int add_match(sd_journal* j, const std::string& field, const std::string& value) {
    const std::string m = field + "=" + value;
    return sd_journal_add_match(j, m.data(), m.size());
}

bool valid_field_name(const std::string& f) {
    if (f.empty() || f.size() > 64) return false;
    for (char c : f) {
        if (!(std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    }
    return !std::isdigit(static_cast<unsigned char>(f[0]));
}

bool is_boot_id(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::vector<JournalBoot> list_boots(Journal& jr, std::string* error) {
    std::vector<JournalBoot> out;
    sd_journal* j = jr.get();
    int r = sd_journal_query_unique(j, "_BOOT_ID");
    if (r < 0) {
        if (error) *error = errno_text("sd_journal_query_unique", r);
        return out;
    }
    std::vector<std::string> ids;
    const void* data = nullptr;
    size_t len = 0;
    SD_JOURNAL_FOREACH_UNIQUE(j, data, len) {
        const char* d = static_cast<const char*>(data);
        if (len > 9) ids.emplace_back(d + 9, len - 9);  // "_BOOT_ID="
    }
    for (const std::string& id : ids) {
        if (!is_boot_id(id)) continue;
        sd_journal_flush_matches(j);
        add_match(j, "_BOOT_ID", id);
        JournalBoot b;
        b.id = id;
        if (sd_journal_seek_head(j) >= 0 && sd_journal_next(j) > 0) b.first_us = jr.realtime();
        if (sd_journal_seek_tail(j) >= 0 && sd_journal_previous(j) > 0) b.last_us = jr.realtime();
        if (b.first_us || b.last_us) out.push_back(std::move(b));
    }
    sd_journal_flush_matches(j);
    std::sort(out.begin(), out.end(), [](const JournalBoot& a, const JournalBoot& b) { return a.first_us < b.first_us; });
    for (size_t i = 0; i < out.size(); ++i) out[i].offset = -static_cast<int32_t>(out.size() - 1 - i);
    return out;
}

// Installs the query's matches. Returns "" or why the query is invalid.
std::string apply_matches(Journal& jr, const JournalQuery& q) {
    sd_journal* j = jr.get();
    sd_journal_flush_matches(j);

    // The boot first: resolving an offset lists the boots, which uses matches.
    std::string boot_id;
    if (!q.boot.empty()) {
        if (q.boot == "current" || q.boot == "0") {
            sd_id128_t id;
            int r = sd_id128_get_boot(&id);
            if (r < 0) return errno_text("sd_id128_get_boot", r);
            char s[SD_ID128_STRING_MAX];
            boot_id = sd_id128_to_string(id, s);
        } else if (is_boot_id(q.boot)) {
            boot_id = lower(q.boot);
        } else {
            char* end = nullptr;
            const long off = std::strtol(q.boot.c_str(), &end, 10);
            if (!end || *end || off > 0) return "unknown boot \"" + q.boot + "\"";
            std::string err;
            auto boots = list_boots(jr, &err);
            if (!err.empty()) return err;
            for (auto& b : boots)
                if (b.offset == off) boot_id = b.id;
            if (boot_id.empty()) return "no boot " + q.boot + " in the journal";
        }
    }

    if (!q.units.empty()) {
        for (const std::string& u : q.units) {
            add_match(j, "_SYSTEMD_UNIT", u);
            sd_journal_add_disjunction(j);
            add_match(j, "_SYSTEMD_USER_UNIT", u);
            sd_journal_add_disjunction(j);
            add_match(j, "_PID", "1");
            add_match(j, "UNIT", u);
            sd_journal_add_disjunction(j);
            add_match(j, "USER_UNIT", u);
            sd_journal_add_disjunction(j);
        }
        sd_journal_add_conjunction(j);
    }
    if (q.max_priority >= 0 && q.max_priority < 7) {
        for (int p = 0; p <= q.max_priority; ++p) add_match(j, "PRIORITY", std::to_string(p));
        sd_journal_add_conjunction(j);
    }
    if (!boot_id.empty()) {
        add_match(j, "_BOOT_ID", boot_id);
        sd_journal_add_conjunction(j);
    }
    if (!q.matches.empty()) {
        for (auto& [field, value] : q.matches) {
            if (!valid_field_name(field)) return "invalid journal field name \"" + field + "\"";
            add_match(j, field, value);
        }
        sd_journal_add_conjunction(j);
    }
    return {};
}

class LinuxJournalService final : public JournalService {
public:
    explicit LinuxJournalService(JournalConfig config) : config_(std::move(config)) {}

    ~LinuxJournalService() override {
        std::map<uint64_t, std::unique_ptr<Follow>> follows;
        {
            std::lock_guard<std::mutex> lock(mu_);
            follows.swap(follows_);
        }
        for (auto& [id, f] : follows) f->stop.store(true);
        for (auto& [id, f] : follows)
            if (f->thread.joinable()) f->thread.join();
    }

    bool probe(std::string* error) {
        Journal jr;
        int r = jr.open(config_);
        if (r < 0) {
            if (error) *error = errno_text("sd_journal_open", r);
            return false;
        }
        return true;
    }

    Result query(const JournalQuery& q, JournalPage& out) override {
        out = JournalPage{};
        Journal jr;
        int r = jr.open(config_);
        if (r < 0) return Result::failure(errno_text("sd_journal_open", r));
        std::string why = apply_matches(jr, q);
        if (!why.empty()) return Result::failure(why);
        sd_journal* j = jr.get();

        // Position on the newest candidate.
        bool skip_cursor = false;
        if (!q.before.empty()) {
            r = sd_journal_seek_cursor(j, q.before.c_str());
            skip_cursor = true;
        } else if (q.until_us) {
            r = sd_journal_seek_realtime_usec(j, q.until_us);
        } else {
            r = sd_journal_seek_tail(j);
        }
        if (r < 0) return Result::failure(errno_text("sd_journal_seek", r));

        const std::string needle = lower(q.text);
        const size_t limit = q.limit ? q.limit : 200;
        size_t scanned = 0;
        std::vector<JournalEntry> newest_first;
        while (true) {
            r = sd_journal_previous(j);
            if (r < 0) return Result::failure(errno_text("sd_journal_previous", r));
            if (r == 0) break;  // the head of the journal
            if (skip_cursor) {
                skip_cursor = false;
                if (sd_journal_test_cursor(j, q.before.c_str()) > 0) continue;
            }
            const uint64_t t = jr.realtime();
            if (q.until_us && t > q.until_us) continue;
            if (q.since_us && t < q.since_us) break;
            if (newest_first.size() >= limit || scanned >= kScanCap) {
                out.more = true;
                break;
            }
            ++scanned;
            out.before = jr.cursor();
            if (!needle.empty() && !contains_ci(jr.field("MESSAGE"), needle)) continue;
            newest_first.push_back(jr.entry(q.all_fields));
        }
        out.entries.assign(std::make_move_iterator(newest_first.rbegin()),
                           std::make_move_iterator(newest_first.rend()));
        return Result::success();
    }

    std::vector<JournalBoot> boots(std::string* error) override {
        Journal jr;
        int r = jr.open(config_);
        if (r < 0) {
            if (error) *error = errno_text("sd_journal_open", r);
            return {};
        }
        return list_boots(jr, error);
    }

    std::vector<std::string> field_values(const std::string& field, std::string* error) override {
        if (!valid_field_name(field)) {
            if (error) *error = "invalid journal field name \"" + field + "\"";
            return {};
        }
        Journal jr;
        int r = jr.open(config_);
        if (r < 0) {
            if (error) *error = errno_text("sd_journal_open", r);
            return {};
        }
        r = sd_journal_query_unique(jr.get(), field.c_str());
        if (r < 0) {
            if (error) *error = errno_text("sd_journal_query_unique", r);
            return {};
        }
        std::set<std::string> values;
        const void* data = nullptr;
        size_t len = 0;
        const size_t skip = field.size() + 1;
        SD_JOURNAL_FOREACH_UNIQUE(jr.get(), data, len) {
            if (len >= skip) values.insert(sanitize_utf8(static_cast<const char*>(data) + skip, len - skip));
        }
        return {values.begin(), values.end()};
    }

    uint64_t follow(const JournalQuery& q, std::string* error) override {
        auto f = std::make_unique<Follow>();
        f->query = q;
        if (int r = f->journal.open(config_); r < 0) {
            if (error) *error = errno_text("sd_journal_open", r);
            return 0;
        }
        sd_journal* j = f->journal.get();
        // A follow from the end starts after the newest record of the whole
        // journal, read before the matches apply: seeking the tail of a
        // filtered view that has no record yet leaves the reader at the
        // "tail" location, after which no later record ever arrives.
        std::string after = q.after;
        int r = 0;
        if (after.empty()) {
            r = sd_journal_seek_tail(j);
            if (r >= 0 && sd_journal_previous(j) > 0) after = f->journal.cursor();
        }
        if (std::string why = apply_matches(f->journal, q); !why.empty()) {
            if (error) *error = why;
            return 0;
        }
        // Position just before the first record to deliver.
        if (r >= 0 && !after.empty()) {
            r = sd_journal_seek_cursor(j, after.c_str());
            if (r >= 0 && sd_journal_next(j) > 0 && sd_journal_test_cursor(j, after.c_str()) <= 0) {
                // The cursor's record is gone (or filtered out): we stand on
                // the first one after it, which is due too.
                sd_journal_previous(j);
            }
        } else if (r >= 0) {
            r = sd_journal_seek_head(j);  // an empty journal: from its first record
        }
        if (r < 0) {
            if (error) *error = errno_text("sd_journal_seek", r);
            return 0;
        }
        Follow* raw = f.get();
        uint64_t id;
        {
            std::lock_guard<std::mutex> lock(mu_);
            id = next_id_++;
            raw->id = id;
            follows_[id] = std::move(f);
        }
        raw->thread = std::thread([this, raw] { run_follow(*raw); });
        return id;
    }

    void unfollow(uint64_t id) override {
        std::unique_ptr<Follow> f;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = follows_.find(id);
            if (it == follows_.end()) return;
            f = std::move(it->second);
            follows_.erase(it);
        }
        f->stop.store(true);
        if (f->thread.joinable()) f->thread.join();
    }

    JournalEventQueue& events() override { return queue_; }

private:
    struct Follow {
        uint64_t id = 0;
        JournalQuery query;
        Journal journal;
        std::atomic<bool> stop{false};
        std::thread thread;
    };

    void run_follow(Follow& f) {
        sd_journal* j = f.journal.get();
        const std::string needle = lower(f.query.text);
        while (!f.stop.load()) {
            JournalFollowBatch batch;
            batch.follow_id = f.id;
            int r = 0;
            while (batch.entries.size() < kFollowBatch && (r = sd_journal_next(j)) > 0) {
                const uint64_t t = f.journal.realtime();
                if (f.query.since_us && t < f.query.since_us) continue;
                if (!needle.empty() && !contains_ci(f.journal.field("MESSAGE"), needle)) continue;
                batch.entries.push_back(f.journal.entry(f.query.all_fields));
            }
            if (r < 0) {
                batch.error = errno_text("sd_journal_next", r);
                batch.ended = true;
                queue_.push(std::move(batch));
                return;
            }
            const bool full = batch.entries.size() >= kFollowBatch;
            if (!batch.entries.empty()) queue_.push(std::move(batch));
            if (full) continue;
            r = sd_journal_wait(j, kFollowWaitUs);
            if (r < 0) {
                JournalFollowBatch err;
                err.follow_id = f.id;
                err.error = errno_text("sd_journal_wait", r);
                err.ended = true;
                queue_.push(std::move(err));
                return;
            }
        }
    }

    JournalConfig config_;
    JournalEventQueue queue_;
    std::mutex mu_;
    uint64_t next_id_ = 1;
    std::map<uint64_t, std::unique_ptr<Follow>> follows_;
};

}  // namespace

std::unique_ptr<JournalService> JournalService::create(const JournalConfig& config, std::string* error) {
    auto s = std::make_unique<LinuxJournalService>(config);
    if (!s->probe(error)) return nullptr;
    return s;
}

}  // namespace brosys
