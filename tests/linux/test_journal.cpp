// JournalService against the machine's real journal.
//
// The test writes its own records (sd_journal_send, tagged with a field
// unique to this run, BROSYS_TEST=<tag>) and reads them back: matches,
// units, priorities, text, boots, since/until, backward paging by cursor,
// all fields, and a live follow from a cursor and from the end. Writing a
// few log lines is all it does to the machine. Without a readable journal
// (no journald, a container) it skips.
#include "check.h"
#include "brosys/journal.h"

#include <systemd/sd-journal.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace brosys;
using namespace std::chrono_literals;

namespace {

std::string g_tag;
constexpr int kCount = 30;

void send(int i, int priority) {
    sd_journal_send("MESSAGE=brosys journal test %s #%d", g_tag.c_str(), i, "PRIORITY=%d", priority,
                    "SYSLOG_IDENTIFIER=brosys-test", "BROSYS_TEST=%s", g_tag.c_str(), "BROSYS_SEQ=%d", i,
                    nullptr);
}

JournalQuery tagged() {
    JournalQuery q;
    q.matches.emplace_back("BROSYS_TEST", g_tag);
    q.limit = 1000;
    return q;
}

size_t count(JournalService& j, JournalQuery q) {
    JournalPage page;
    Result r = j.query(q, page);
    if (!r.ok) std::fprintf(stderr, "query failed: %s\n", r.error.c_str());
    return r.ok ? page.entries.size() : 0;
}

void test_query(JournalService& j) {
    // journald writes asynchronously.
    REQUIRE(bstest::wait_until([&] { return count(j, tagged()) == kCount; }, 10s, 50ms));

    JournalPage page;
    REQUIRE(j.query(tagged(), page).ok);
    REQUIRE(page.entries.size() == static_cast<size_t>(kCount));
    CHECK(!page.more);
    for (int i = 0; i < kCount; ++i) {
        const JournalEntry& e = page.entries[i];
        CHECK_EQ(e.message, "brosys journal test " + g_tag + " #" + std::to_string(i));
        CHECK_EQ(e.priority, i % 2 ? 6 : 3);
        CHECK_EQ(e.identifier, std::string("brosys-test"));
        CHECK(!e.cursor.empty());
        CHECK_EQ(e.boot_id.size(), size_t(32));
        CHECK(e.realtime_us > 0);
        CHECK(e.fields.empty());
        if (i) CHECK(e.realtime_us >= page.entries[i - 1].realtime_us);
    }
    CHECK_EQ(page.entries[0].transport, std::string("journal"));

    // Priority: err (3) and above.
    JournalQuery q = tagged();
    q.max_priority = 3;
    CHECK_EQ(count(j, q), size_t(kCount / 2));
    q.max_priority = 2;
    CHECK_EQ(count(j, q), size_t(0));

    // Text, case-insensitive: "#1" is #1 and #10..#19.
    q = tagged();
    q.text = "TEST " + g_tag + " #1";
    for (char& c : q.text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    CHECK_EQ(count(j, q), size_t(11));

    // Alternatives of one field, and a second field that must also hold.
    q = tagged();
    q.matches.emplace_back("BROSYS_SEQ", "4");
    q.matches.emplace_back("BROSYS_SEQ", "5");
    CHECK_EQ(count(j, q), size_t(2));
    q.matches.emplace_back("PRIORITY", "6");
    CHECK_EQ(count(j, q), size_t(1));

    // The unit the records came from.
    if (!page.entries[0].unit.empty()) {
        q = tagged();
        q.units = {page.entries[0].unit};
        CHECK_EQ(count(j, q), size_t(kCount));
        q.units = {"brosys-no-such-unit.service"};
        CHECK_EQ(count(j, q), size_t(0));
    }

    // This boot, by name, by offset and by id.
    q = tagged();
    q.boot = "current";
    CHECK_EQ(count(j, q), size_t(kCount));
    q.boot = "0";
    CHECK_EQ(count(j, q), size_t(kCount));
    q.boot = page.entries[0].boot_id;
    CHECK_EQ(count(j, q), size_t(kCount));
    q.boot = "nonsense";
    JournalPage bad;
    CHECK(!j.query(q, bad).ok);

    // since / until.
    q = tagged();
    q.since_us = page.entries[15].realtime_us;
    const size_t since = count(j, q);
    CHECK(since >= 15 && since <= size_t(kCount));  // records written in the same microsecond tie
    q = tagged();
    q.until_us = page.entries[14].realtime_us;
    const size_t until = count(j, q);
    CHECK(until >= 15 && until <= size_t(kCount));

    // All fields.
    q = tagged();
    q.all_fields = true;
    q.limit = 1;
    JournalPage one;
    REQUIRE(j.query(q, one).ok);
    REQUIRE(one.entries.size() == 1);
    CHECK(one.more);
    bool found = false;
    for (auto& [k, v] : one.entries[0].fields)
        if (k == "BROSYS_TEST" && v == g_tag) found = true;
    CHECK(found);
    CHECK_EQ(one.entries[0].message, "brosys journal test " + g_tag + " #" + std::to_string(kCount - 1));

    // A field name journald would never write is refused.
    q = tagged();
    q.matches.emplace_back("lower case", "x");
    CHECK(!j.query(q, bad).ok);
}

void test_paging(JournalService& j) {
    JournalQuery q = tagged();
    q.limit = 10;
    std::vector<int> seen;
    for (int pageNo = 0; pageNo < 3; ++pageNo) {
        JournalPage page;
        REQUIRE(j.query(q, page).ok);
        REQUIRE(page.entries.size() == 10);
        CHECK(page.more || pageNo == 2);
        const int first = kCount - 10 * (pageNo + 1);
        for (int i = 0; i < 10; ++i)
            CHECK_EQ(page.entries[i].message, "brosys journal test " + g_tag + " #" + std::to_string(first + i));
        CHECK_EQ(page.before, page.entries[0].cursor);
        q.before = page.before;
    }
    JournalPage last;
    REQUIRE(j.query(q, last).ok);
    CHECK(last.entries.empty());
    CHECK(!last.more);
}

void test_boots_and_values(JournalService& j) {
    std::string err;
    auto boots = j.boots(&err);
    CHECK(err.empty());
    REQUIRE(!boots.empty());
    CHECK_EQ(boots.back().offset, 0);
    JournalPage page;
    JournalQuery q = tagged();
    q.limit = 1;
    REQUIRE(j.query(q, page).ok && !page.entries.empty());
    CHECK_EQ(boots.back().id, page.entries[0].boot_id);
    for (size_t i = 1; i < boots.size(); ++i) CHECK(boots[i].first_us >= boots[i - 1].first_us);

    auto values = j.field_values("BROSYS_TEST", &err);
    CHECK(err.empty());
    bool found = false;
    for (auto& v : values) found = found || v == g_tag;
    CHECK(found);
    j.field_values("not a field", &err);
    CHECK(!err.empty());
}

std::vector<JournalEntry> collect(JournalService& j, uint64_t id, size_t want) {
    std::vector<JournalEntry> got;
    bstest::wait_until([&] {
        for (auto& b : j.events().drain()) {
            if (b.follow_id != id) continue;
            if (!b.error.empty()) std::fprintf(stderr, "follow error: %s\n", b.error.c_str());
            for (auto& e : b.entries) got.push_back(std::move(e));
        }
        return got.size() >= want;
    }, 10s, 20ms);
    return got;
}

void test_follow(JournalService& j) {
    JournalPage page;
    REQUIRE(j.query(tagged(), page).ok && !page.entries.empty());

    // From a cursor: the records after #19, then the new ones.
    JournalQuery fromCursor = tagged();
    fromCursor.after = page.entries[19].cursor;
    std::string err;
    const uint64_t a = j.follow(fromCursor, &err);
    REQUIRE(a != 0);

    // From the end: only what is written from now on, and only priority err.
    JournalQuery fromEnd = tagged();
    fromEnd.max_priority = 3;
    const uint64_t b = j.follow(fromEnd, &err);
    REQUIRE(b != 0);
    std::this_thread::sleep_for(300ms);  // both positioned before the writes

    for (int i = kCount; i < kCount + 6; ++i) send(i, i % 2 ? 6 : 3);

    // a: #20..#35 (but drain shares one queue, so collect both together).
    std::vector<JournalEntry> gotA, gotB;
    bstest::wait_until([&] {
        for (auto& batch : j.events().drain()) {
            if (!batch.error.empty()) std::fprintf(stderr, "follow error: %s\n", batch.error.c_str());
            auto& dst = batch.follow_id == a ? gotA : gotB;
            for (auto& e : batch.entries) dst.push_back(std::move(e));
        }
        return gotA.size() >= 16 && gotB.size() >= 3;
    }, 10s, 20ms);
    REQUIRE(gotA.size() == 16);
    for (int i = 0; i < 16; ++i)
        CHECK_EQ(gotA[i].message, "brosys journal test " + g_tag + " #" + std::to_string(20 + i));
    REQUIRE(gotB.size() == 3);
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(gotB[i].message, "brosys journal test " + g_tag + " #" + std::to_string(kCount + 2 * i));
        CHECK_EQ(gotB[i].priority, 3);
    }

    // Unfollowed: nothing more arrives for it.
    j.unfollow(a);
    send(kCount + 6, 3);
    auto more = collect(j, b, 1);
    CHECK_EQ(more.size(), size_t(1));
    for (auto& batch : j.events().drain()) CHECK(batch.follow_id != a);
    j.unfollow(b);
    j.unfollow(b);  // twice is harmless

    // From the end, matching nothing written yet: the first matching record
    // still arrives (a filtered tail with no record must not strand the reader).
    JournalQuery fresh;
    fresh.matches.emplace_back("BROSYS_TEST", g_tag + "-fresh");
    const uint64_t c = j.follow(fresh, &err);
    REQUIRE(c != 0);
    std::this_thread::sleep_for(300ms);
    sd_journal_send("MESSAGE=brosys journal test fresh", "BROSYS_TEST=%s-fresh", g_tag.c_str(), nullptr);
    auto freshGot = collect(j, c, 1);
    REQUIRE(freshGot.size() == 1);
    CHECK_EQ(freshGot[0].message, std::string("brosys journal test fresh"));
    j.unfollow(c);

    JournalQuery bogus = tagged();
    bogus.matches.emplace_back("bad name", "x");
    CHECK_EQ(j.follow(bogus, &err), uint64_t(0));
    CHECK(!err.empty());
}

void test_priority_names() {
    CHECK_EQ(parse_journal_priority("err"), 3);
    CHECK_EQ(parse_journal_priority("ERROR"), 3);
    CHECK_EQ(parse_journal_priority("warning"), 4);
    CHECK_EQ(parse_journal_priority("warn"), 4);
    CHECK_EQ(parse_journal_priority("debug"), 7);
    CHECK_EQ(parse_journal_priority("0"), 0);
    CHECK_EQ(parse_journal_priority("8"), -1);
    CHECK_EQ(parse_journal_priority("loud"), -1);
}

}  // namespace

int main() {
    test_priority_names();

    std::string err;
    auto journal = JournalService::create({}, &err);
    if (!journal) bstest::skip("test_journal", "no readable journal: " + err);

    std::random_device rd;
    char tag[17];
    std::snprintf(tag, sizeof tag, "%08x%08x", rd(), rd());
    g_tag = tag;
    for (int i = 0; i < kCount; ++i) send(i, i % 2 ? 6 : 3);

    // Records this process writes may not be readable by it (a journal
    // without per-user files that the user may not read): then it is
    // nothing to test against.
    if (!bstest::wait_until([&] { return count(*journal, tagged()) > 0; }, 10s, 50ms))
        bstest::skip("test_journal", "records written to the journal are not readable here");

    test_query(*journal);
    test_paging(*journal);
    test_boots_and_values(*journal);
    test_follow(*journal);
    return bstest::finish("test_journal");
}
