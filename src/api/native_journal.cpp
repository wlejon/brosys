// bro.sys.journal: the systemd journal, queried and followed off the page's
// thread (docs/sys-api.js in bro).
#include "api.h"
#include "arg_reader.h"
#include "async_job.h"
#include "object_builder.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace brosys::api {

namespace {

struct FollowCallback {
    std::shared_ptr<ev::Persistent> onEntries;
    std::shared_ptr<ev::Persistent> onError;
};

std::map<uint64_t, FollowCallback> g_follows;

// A number of ms since the epoch, or a Date, as microseconds (0 when absent).
uint64_t timeUs(Value v) {
    if (ev::isUndefined(v) || ev::isNull(v)) return 0;
    double ms = 0;
    if (ev::isNumber(v)) {
        ms = ev::toDouble(v);
    } else if (ev::isObject(v)) {
        ev::Persistent obj(v);
        ev::Persistent getTime(ev::getProperty(obj.get(), "getTime"));
        if (!ev::isFunction(getTime.get())) return 0;
        ev::CallResult r = ev::call(getTime.get(), obj.get(), {});
        if (r.thrown || !ev::isNumber(r.value)) return 0;
        ms = ev::toDouble(r.value);
    }
    if (!(ms > 0)) return 0;
    return static_cast<uint64_t>(ms * 1000.0);
}

// One string, or each string of an array, appended to `out`.
void appendStrings(Value v, std::vector<std::string>& out) {
    if (ev::isUndefined(v) || ev::isNull(v)) return;
    if (isArray(v)) {
        ev::Persistent arr(v);
        const uint32_t n = u32Val(ev::getProperty(arr.get(), "length"));
        for (uint32_t i = 0; i < n; ++i) {
            std::string s = strVal(ev::getElement(arr.get(), i));
            if (!s.empty()) out.push_back(std::move(s));
        }
        return;
    }
    std::string s = strVal(v);
    if (!s.empty()) out.push_back(std::move(s));
}

// The query options object (docs/sys-api.js: JournalQueryOptions). Returns
// "" or why the options are invalid.
std::string readQuery(Value optsIn, JournalQuery& q) {
    if (ev::isUndefined(optsIn) || ev::isNull(optsIn)) return {};
    if (!ev::isObject(optsIn)) return "journal options must be an object";
    ev::Persistent opts(optsIn);
    auto prop = [&](const char* name) { return ev::getProperty(opts.get(), name); };

    appendStrings(prop("unit"), q.units);
    appendStrings(prop("units"), q.units);

    {
        ev::Persistent p(prop("priority"));
        if (ev::isNumber(p.get())) {
            q.max_priority = i32Val(p.get());
            if (q.max_priority < 0 || q.max_priority > 7) return "priority must be 0..7";
        } else if (ev::isString(p.get())) {
            q.max_priority = parse_journal_priority(strVal(p.get()));
            if (q.max_priority < 0) return "unknown priority \"" + strVal(p.get()) + "\"";
        }
    }
    {
        ev::Persistent b(prop("boot"));
        if (ev::isNumber(b.get())) q.boot = std::to_string(i32Val(b.get()));
        else if (ev::isBool(b.get())) q.boot = ev::toBool(b.get()) ? "current" : "";
        else q.boot = strVal(b.get());
    }
    q.since_us = timeUs(prop("since"));
    q.until_us = timeUs(prop("until"));
    {
        ev::Persistent m(prop("match"));
        if (ev::isObject(m.get())) {
            for (const std::string& field : objectKeys(m.get())) {
                std::vector<std::string> values;
                ev::Persistent v(ev::getProperty(m.get(), field));
                if (ev::isNumber(v.get())) values.push_back(std::to_string(i64Val(v.get())));
                else appendStrings(v.get(), values);
                for (auto& value : values) q.matches.emplace_back(field, value);
            }
        }
    }
    q.text = strVal(prop("text"));
    q.before = strVal(prop("before"));
    q.after = strVal(prop("after"));
    {
        ev::Persistent l(prop("limit"));
        if (ev::isNumber(l.get())) {
            const double d = ev::toDouble(l.get());
            q.limit = d >= 1 ? static_cast<size_t>(d > 100000 ? 100000 : d) : 1;
        }
    }
    q.all_fields = boolVal(prop("allFields"));
    return {};
}

Value buildEntry(const JournalEntry& e) {
    ObjectBuilder b;
    b.set("cursor", e.cursor);
    b.set("time", static_cast<double>(e.realtime_us) / 1000.0);
    b.set("realtimeUs", static_cast<double>(e.realtime_us));
    b.set("monotonicUs", static_cast<double>(e.monotonic_us));
    b.set("bootId", e.boot_id);
    if (e.priority >= 0) b.set("priority", e.priority); else b.setNull("priority");
    b.set("message", e.message);
    b.set("unit", e.unit);
    b.set("identifier", e.identifier);
    b.set("pid", e.pid);
    b.set("hostname", e.hostname);
    b.set("transport", e.transport);
    if (!e.fields.empty()) {
        ObjectBuilder f;
        for (auto& [k, v] : e.fields) f.set(k, v);
        b.set("fields", f.build());
    }
    return b.build();
}

Value buildEntries(const std::vector<JournalEntry>& entries) {
    ArrayBuilder arr(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        ev::Persistent v(buildEntry(entries[i]));
        arr.set(static_cast<uint32_t>(i), v.get());
    }
    return arr.build();
}

const char* unavailableReason() {
    static std::string reason;
    std::string err;
    if (!getJournalService(&err)) {
        reason = err.empty() ? "the systemd journal is unavailable" : err;
        return reason.c_str();
    }
    return nullptr;
}

Value rejected(const std::string& why) {
    return runAsync([why] { return why; }, nullptr);
}

Value doQuery(Value, std::span<const Value> args) {
    auto q = std::make_shared<JournalQuery>();
    std::string why = readQuery(argAt(args, 0), *q);
    if (!why.empty()) return rejected(why);
    if (const char* r = unavailableReason()) return rejected(r);
    JournalService* svc = getJournalService();
    auto page = std::make_shared<JournalPage>();
    return runAsync(
        [svc, q, page] {
            Result r = svc->query(*q, *page);
            return r.ok ? std::string() : r.error;
        },
        [page] {
            ObjectBuilder b;
            ev::Persistent entries(buildEntries(page->entries));
            b.set("entries", entries.get());
            if (page->before.empty()) b.setNull("before"); else b.set("before", page->before);
            b.set("more", page->more);
            return b.build();
        });
}

Value doBoots(Value, std::span<const Value>) {
    if (const char* r = unavailableReason()) return rejected(r);
    JournalService* svc = getJournalService();
    auto boots = std::make_shared<std::vector<JournalBoot>>();
    return runAsync(
        [svc, boots] {
            std::string err;
            *boots = svc->boots(&err);
            return err;
        },
        [boots] {
            ArrayBuilder arr(boots->size());
            for (size_t i = 0; i < boots->size(); ++i) {
                const JournalBoot& bt = (*boots)[i];
                ObjectBuilder b;
                b.set("id", bt.id);
                b.set("offset", bt.offset);
                b.set("first", static_cast<double>(bt.first_us) / 1000.0);
                b.set("last", static_cast<double>(bt.last_us) / 1000.0);
                ev::Persistent v(b.build());
                arr.set(static_cast<uint32_t>(i), v.get());
            }
            return arr.build();
        });
}

Value doValues(Value, std::span<const Value> args) {
    const std::string field = strAt(args, 0);
    if (field.empty()) return rejected("values needs a field name");
    if (const char* r = unavailableReason()) return rejected(r);
    JournalService* svc = getJournalService();
    auto values = std::make_shared<std::vector<std::string>>();
    return runAsync(
        [svc, field, values] {
            std::string err;
            *values = svc->field_values(field, &err);
            return err;
        },
        [values] {
            ArrayBuilder arr(values->size());
            for (size_t i = 0; i < values->size(); ++i) arr.set(static_cast<uint32_t>(i), (*values)[i]);
            return arr.build();
        });
}

void closeFollow(uint64_t id) {
    auto it = g_follows.find(id);
    if (it == g_follows.end()) return;
    g_follows.erase(it);
    if (JournalService* svc = getJournalService()) svc->unfollow(id);
}

// follow(options, onEntries) -> { id, close() }
Value doFollow(Value, std::span<const Value> args) {
    ev::Persistent opts(argAt(args, 0));
    ev::Persistent cb(argAt(args, 1));
    if (!ev::isFunction(cb.get())) return ev::throwTypeError("journal.follow needs a callback for the entries");
    JournalQuery q;
    std::string why = readQuery(opts.get(), q);
    if (!why.empty()) return ev::throwTypeError(why);
    if (const char* r = unavailableReason()) return ev::throwError(r);
    std::string err;
    const uint64_t id = getJournalService()->follow(q, &err);
    if (!id) return ev::throwError("journal.follow failed: " + err);

    FollowCallback fc;
    fc.onEntries = std::make_shared<ev::Persistent>(cb.get());
    if (ev::isObject(opts.get())) {
        ev::Persistent onError(ev::getProperty(opts.get(), "onError"));
        if (ev::isFunction(onError.get())) fc.onError = std::make_shared<ev::Persistent>(onError.get());
    }
    g_follows[id] = std::move(fc);

    ObjectBuilder handle;
    handle.set("id", static_cast<double>(id));
    handle.def("close", 0, [id](Value, std::span<const Value>) {
        closeFollow(id);
        return ev::undefined();
    });
    return handle.build();
}

}  // namespace

void installJournal(ObjectBuilder& sys) {
    ObjectBuilder journal;
#if !defined(__linux__)
    journal.set("available", false);
    journal.set("reason", std::string("the systemd journal exists only on Linux"));
#endif
    journal.def("query", 1, doQuery);
    journal.def("boots", 0, doBoots);
    journal.def("values", 1, doValues);
    journal.def("follow", 2, doFollow);

    ArrayBuilder names(8);
    const char* kNames[] = {"emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"};
    for (uint32_t i = 0; i < 8; ++i) names.set(i, std::string(kNames[i]));
    journal.set("priorities", names.build());

    sys.set("journal", journal.build());
}

void tickJournal() {
    JournalService* svc = getJournalService();
    if (!svc) return;
    for (auto& batch : svc->events().drain()) {
        auto it = g_follows.find(batch.follow_id);
        if (it == g_follows.end()) continue;
        FollowCallback fc = it->second;  // the callback may close the follow
        if (!batch.entries.empty() && fc.onEntries) {
            ev::Persistent arr(buildEntries(batch.entries));
            const Value arg = arr.get();
            ev::catchThrow([&]() {
                return ev::call(fc.onEntries->get(), ev::undefined(), std::span<const Value>(&arg, 1)).value;
            });
        }
        if (!batch.error.empty() && fc.onError) {
            ev::Persistent msg(makeError(batch.error));
            const Value arg = msg.get();
            ev::catchThrow([&]() {
                return ev::call(fc.onError->get(), ev::undefined(), std::span<const Value>(&arg, 1)).value;
            });
        }
        if (batch.ended) closeFollow(batch.follow_id);
    }
}

void shutdownJournal() {
    JournalService* svc = getJournalService();
    for (auto& [id, fc] : g_follows)
        if (svc) svc->unfollow(id);
    g_follows.clear();
}

}  // namespace brosys::api
