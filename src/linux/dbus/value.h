// A D-Bus value of any type, with its signature, for reading and writing
// sd-bus messages without per-call C plumbing.
//
//   Value::str("x"), Value::u32(1), Value::variant(v),
//   Value::array("s", {...}), Value::dict("s", "v", {{k, v}, ...}),
//   Value::structure({...}), Value::bytes({...})
//
// Containers keep their children in `items`: array elements, struct fields,
// or a dict entry's {key, value}. A variant boxes one child. "ay" is stored
// as raw bytes (images are large).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

typedef struct sd_bus_message sd_bus_message;

namespace brosys::dbus {

// An owned, shared unix fd ('h'). Closed when the last copy goes away.
class UnixFd {
public:
    UnixFd() = default;
    static UnixFd adopt(int fd);  // takes ownership
    static UnixFd dup(int fd);    // duplicates (F_DUPFD_CLOEXEC)
    int get() const { return fd_ ? *fd_ : -1; }
    bool valid() const { return get() >= 0; }
    int release();  // gives up ownership (only when this is the last copy)
    bool operator==(const UnixFd& o) const { return get() == o.get(); }

private:
    std::shared_ptr<int> fd_;
};

struct Value {
    using Bytes = std::vector<uint8_t>;
    using Items = std::vector<Value>;
    using Box = std::shared_ptr<Value>;
    using Data = std::variant<std::monostate, bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                              uint64_t, double, std::string, UnixFd, Bytes, Items, Box>;

    std::string sig;  // one complete type
    Data data;

    // ---- construction
    static Value boolean(bool b) { return {"b", b}; }
    static Value byte(uint8_t v) { return {"y", v}; }
    static Value i16(int16_t v) { return {"n", v}; }
    static Value u16(uint16_t v) { return {"q", v}; }
    static Value i32(int32_t v) { return {"i", v}; }
    static Value u32(uint32_t v) { return {"u", v}; }
    static Value i64(int64_t v) { return {"x", v}; }
    static Value u64(uint64_t v) { return {"t", v}; }
    static Value dbl(double v) { return {"d", v}; }
    static Value str(std::string s) { return {"s", std::move(s)}; }
    static Value obj(std::string path) { return {"o", std::move(path)}; }
    static Value signature(std::string s) { return {"g", std::move(s)}; }
    static Value fd(UnixFd f) { return {"h", std::move(f)}; }
    static Value bytes(Bytes b) { return {"ay", std::move(b)}; }
    static Value variant(Value inner);
    static Value array(const std::string& element_sig, Items elements);
    static Value strings(const std::vector<std::string>& v);  // "as"
    static Value dict(const std::string& key_sig, const std::string& value_sig,
                      std::vector<std::pair<Value, Value>> entries);
    static Value structure(Items fields);
    // a{sv} from name/value pairs (values are wrapped in variants).
    static Value vardict(std::vector<std::pair<std::string, Value>> entries);

    // ---- inspection
    char type() const { return sig.empty() ? '\0' : sig[0]; }
    bool is_array() const { return type() == 'a'; }
    bool is_dict() const { return sig.size() > 1 && sig[0] == 'a' && sig[1] == '{'; }

    // Looks through variants (recursively).
    const Value& unwrap() const;

    // Typed reads; return the fallback on a type mismatch. Integer reads
    // accept any integer type (and bool / byte) in range.
    std::optional<int64_t> to_int() const;
    std::optional<uint64_t> to_uint() const;
    std::optional<double> to_double() const;  // also from integers
    std::optional<bool> to_bool() const;
    const std::string* to_string() const;     // s / o / g
    std::string as_string(std::string fallback = std::string()) const;
    int64_t as_int(int64_t fallback = 0) const { return to_int().value_or(fallback); }
    uint64_t as_uint(uint64_t fallback = 0) const { return to_uint().value_or(fallback); }
    double as_double(double fallback = 0) const { return to_double().value_or(fallback); }
    bool as_bool(bool fallback = false) const { return to_bool().value_or(fallback); }
    std::vector<std::string> as_strings() const;  // as / ao / ag
    const Bytes* as_bytes() const;                // ay
    const Items& items() const;                   // array elements / struct fields / entry {k, v}; empty otherwise
    int as_fd() const;                            // -1 when not 'h'

    // For a dict (a{s?}): the value of `key`, unwrapped; nullptr when absent.
    const Value* lookup(std::string_view key) const;

    // GVariant-like text, for diagnostics and pass-through hints.
    std::string to_text() const;

    bool operator==(const Value& o) const;
};

using Args = std::vector<Value>;

// Signature of a sequence of values ("su a{sv}" without spaces).
std::string signature_of(const Args& args);

// Reads every remaining value of `m` (at the current read position).
// Returns false and sets *error on a malformed message.
bool read_all(sd_bus_message* m, Args& out, std::string* error);
// Reads the next complete value.
bool read_value(sd_bus_message* m, Value& out, std::string* error);
// Appends values; returns a negative errno on failure.
int append_all(sd_bus_message* m, const Args& args);
int append_value(sd_bus_message* m, const Value& v);

// Splits a signature into its complete types ("sa{sv}u" -> {"s", "a{sv}", "u"}).
std::vector<std::string> split_signature(std::string_view sig);

}  // namespace brosys::dbus
