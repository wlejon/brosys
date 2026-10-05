#include "linux/dbus/value.h"

#include <systemd/sd-bus.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <unistd.h>

namespace brosys::dbus {

// ---------------------------------------------------------------- UnixFd

UnixFd UnixFd::adopt(int fd) {
    UnixFd f;
    if (fd >= 0)
        f.fd_ = std::shared_ptr<int>(new int(fd), [](int* p) {
            if (*p >= 0) ::close(*p);
            delete p;
        });
    return f;
}

UnixFd UnixFd::dup(int fd) {
    if (fd < 0) return {};
    return adopt(::fcntl(fd, F_DUPFD_CLOEXEC, 3));
}

int UnixFd::release() {
    if (!fd_ || fd_.use_count() != 1) return -1;
    int fd = *fd_;
    *fd_ = -1;
    fd_.reset();
    return fd;
}

// ---------------------------------------------------------------- signatures

static size_t complete_type_length(std::string_view sig, size_t pos) {
    if (pos >= sig.size()) return 0;
    char c = sig[pos];
    if (c == 'a') {
        size_t n = complete_type_length(sig, pos + 1);
        return n ? n + 1 : 0;
    }
    if (c == '(' || c == '{') {
        char close = c == '(' ? ')' : '}';
        size_t p = pos + 1;
        while (p < sig.size() && sig[p] != close) {
            size_t n = complete_type_length(sig, p);
            if (!n) return 0;
            p += n;
        }
        if (p >= sig.size()) return 0;
        return p - pos + 1;
    }
    return 1;
}

std::vector<std::string> split_signature(std::string_view sig) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < sig.size()) {
        size_t n = complete_type_length(sig, pos);
        if (!n) break;
        out.emplace_back(sig.substr(pos, n));
        pos += n;
    }
    return out;
}

std::string signature_of(const Args& args) {
    std::string s;
    for (auto& a : args) s += a.sig;
    return s;
}

// ---------------------------------------------------------------- construction

Value Value::variant(Value inner) { return {"v", std::make_shared<Value>(std::move(inner))}; }

Value Value::array(const std::string& element_sig, Items elements) {
    if (element_sig == "y") {
        Bytes b;
        b.reserve(elements.size());
        for (auto& e : elements) b.push_back(static_cast<uint8_t>(e.as_uint()));
        return bytes(std::move(b));
    }
    return {"a" + element_sig, std::move(elements)};
}

Value Value::strings(const std::vector<std::string>& v) {
    Items items;
    items.reserve(v.size());
    for (auto& s : v) items.push_back(str(s));
    return {"as", std::move(items)};
}

Value Value::dict(const std::string& key_sig, const std::string& value_sig,
                  std::vector<std::pair<Value, Value>> entries) {
    std::string entry_sig = "{" + key_sig + value_sig + "}";
    Items items;
    items.reserve(entries.size());
    for (auto& [k, v] : entries) items.push_back(Value{entry_sig, Items{std::move(k), std::move(v)}});
    return {"a" + entry_sig, std::move(items)};
}

Value Value::structure(Items fields) {
    std::string s = "(";
    for (auto& f : fields) s += f.sig;
    s += ")";
    return {std::move(s), std::move(fields)};
}

Value Value::vardict(std::vector<std::pair<std::string, Value>> entries) {
    std::vector<std::pair<Value, Value>> e;
    e.reserve(entries.size());
    for (auto& [k, v] : entries) e.emplace_back(str(k), v.sig == "v" ? std::move(v) : variant(std::move(v)));
    return dict("s", "v", std::move(e));
}

// ---------------------------------------------------------------- inspection

const Value& Value::unwrap() const {
    const Value* v = this;
    while (auto* box = std::get_if<Box>(&v->data)) {
        if (!*box) break;
        v = box->get();
    }
    return *v;
}

std::optional<int64_t> Value::to_int() const {
    const Value& v = unwrap();
    return std::visit(
        [](auto&& x) -> std::optional<int64_t> {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, bool>) return x ? 1 : 0;
            else if constexpr (std::is_same_v<T, uint64_t>) {
                if (x > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
                return static_cast<int64_t>(x);
            } else if constexpr (std::is_integral_v<T>) return static_cast<int64_t>(x);
            else return std::nullopt;
        },
        v.data);
}

std::optional<uint64_t> Value::to_uint() const {
    const Value& v = unwrap();
    return std::visit(
        [](auto&& x) -> std::optional<uint64_t> {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, bool>) return x ? 1u : 0u;
            else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
                if (x < 0) return std::nullopt;
                return static_cast<uint64_t>(x);
            } else if constexpr (std::is_integral_v<T>) return static_cast<uint64_t>(x);
            else return std::nullopt;
        },
        v.data);
}

std::optional<double> Value::to_double() const {
    const Value& v = unwrap();
    if (auto* d = std::get_if<double>(&v.data)) return *d;
    if (auto i = v.to_int()) return static_cast<double>(*i);
    if (auto u = v.to_uint()) return static_cast<double>(*u);
    return std::nullopt;
}

std::optional<bool> Value::to_bool() const {
    const Value& v = unwrap();
    if (auto* b = std::get_if<bool>(&v.data)) return *b;
    if (auto i = v.to_int()) return *i != 0;
    return std::nullopt;
}

const std::string* Value::to_string() const { return std::get_if<std::string>(&unwrap().data); }

std::string Value::as_string(std::string fallback) const {
    auto* s = to_string();
    return s ? *s : std::move(fallback);
}

std::vector<std::string> Value::as_strings() const {
    std::vector<std::string> out;
    for (auto& i : unwrap().items())
        if (auto* s = i.to_string()) out.push_back(*s);
    return out;
}

const Value::Bytes* Value::as_bytes() const { return std::get_if<Bytes>(&unwrap().data); }

const Value::Items& Value::items() const {
    static const Items empty;
    auto* it = std::get_if<Items>(&data);
    return it ? *it : empty;
}

int Value::as_fd() const {
    auto* f = std::get_if<UnixFd>(&unwrap().data);
    return f ? f->get() : -1;
}

const Value* Value::lookup(std::string_view key) const {
    const Value& d = unwrap();
    if (!d.is_dict()) return nullptr;
    for (auto& entry : d.items()) {
        auto& kv = entry.items();
        if (kv.size() != 2) continue;
        if (auto* k = kv[0].to_string(); k && *k == key) return &kv[1].unwrap();
    }
    return nullptr;
}

bool Value::operator==(const Value& o) const {
    if (sig != o.sig || data.index() != o.data.index()) return false;
    if (auto* a = std::get_if<Box>(&data)) {
        auto* b = std::get_if<Box>(&o.data);
        if (!*a || !*b) return !*a && !*b;
        return **a == **b;
    }
    return data == o.data;
}

static void append_quoted(std::string& out, const std::string& s) {
    out += '\'';
    for (char c : s) {
        if (c == '\'' || c == '\\') out += '\\';
        out += c;
    }
    out += '\'';
}

std::string Value::to_text() const {
    std::string out;
    char buf[64];
    std::visit(
        [&](auto&& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) out += "()";
            else if constexpr (std::is_same_v<T, bool>) out += x ? "true" : "false";
            else if constexpr (std::is_same_v<T, double>) {
                std::snprintf(buf, sizeof buf, "%g", x);
                out += buf;
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (sig == "o") out += "objectpath ";
                append_quoted(out, x);
            } else if constexpr (std::is_same_v<T, UnixFd>) {
                std::snprintf(buf, sizeof buf, "handle %d", x.get());
                out += buf;
            } else if constexpr (std::is_same_v<T, Bytes>) {
                std::snprintf(buf, sizeof buf, "bytes[%zu]", x.size());
                out += buf;
            } else if constexpr (std::is_same_v<T, Items>) {
                bool dict = is_dict();
                bool strct = type() == '(';
                bool entry = type() == '{';
                out += dict || entry ? "{" : strct ? "(" : "[";
                for (size_t i = 0; i < x.size(); ++i) {
                    if (i) out += entry ? ": " : ", ";
                    if (dict) {
                        auto& kv = x[i].items();
                        if (kv.size() == 2) out += kv[0].to_text() + ": " + kv[1].to_text();
                    } else {
                        out += x[i].to_text();
                    }
                }
                out += dict || entry ? "}" : strct ? ")" : "]";
            } else if constexpr (std::is_same_v<T, Box>) {
                out += "<" + (x ? x->to_text() : std::string()) + ">";
            } else if constexpr (std::is_unsigned_v<T>) {
                std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(x));
                out += buf;
            } else {
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(x));
                out += buf;
            }
        },
        data);
    return out;
}

// ---------------------------------------------------------------- reading

static bool fail(std::string* error, const char* what, int r) {
    if (error) *error = std::string(what) + ": " + std::strerror(-r);
    return false;
}

template <class T>
static bool read_basic(sd_bus_message* m, char type, Value& out, std::string* error) {
    T v{};
    int r = sd_bus_message_read_basic(m, type, &v);
    if (r < 0) return fail(error, "read basic", r);
    out.data = v;
    return true;
}

bool read_value(sd_bus_message* m, Value& out, std::string* error) {
    char type = 0;
    const char* contents = nullptr;
    int r = sd_bus_message_peek_type(m, &type, &contents);
    if (r < 0) return fail(error, "peek type", r);
    if (r == 0) {
        if (error) *error = "no more values";
        return false;
    }
    switch (type) {
        case 'y': out.sig = "y"; return read_basic<uint8_t>(m, type, out, error);
        case 'b': {
            int v = 0;
            r = sd_bus_message_read_basic(m, 'b', &v);
            if (r < 0) return fail(error, "read bool", r);
            out.sig = "b";
            out.data = v != 0;
            return true;
        }
        case 'n': out.sig = "n"; return read_basic<int16_t>(m, type, out, error);
        case 'q': out.sig = "q"; return read_basic<uint16_t>(m, type, out, error);
        case 'i': out.sig = "i"; return read_basic<int32_t>(m, type, out, error);
        case 'u': out.sig = "u"; return read_basic<uint32_t>(m, type, out, error);
        case 'x': out.sig = "x"; return read_basic<int64_t>(m, type, out, error);
        case 't': out.sig = "t"; return read_basic<uint64_t>(m, type, out, error);
        case 'd': out.sig = "d"; return read_basic<double>(m, type, out, error);
        case 's':
        case 'o':
        case 'g': {
            const char* s = nullptr;
            r = sd_bus_message_read_basic(m, type, &s);
            if (r < 0) return fail(error, "read string", r);
            out.sig = std::string(1, type);
            out.data = std::string(s ? s : "");
            return true;
        }
        case 'h': {
            int fd = -1;
            r = sd_bus_message_read_basic(m, 'h', &fd);
            if (r < 0) return fail(error, "read fd", r);
            out.sig = "h";
            out.data = UnixFd::dup(fd);  // the message owns its fd
            return true;
        }
        case 'a': {
            out.sig = std::string("a") + (contents ? contents : "");
            if (contents && std::strcmp(contents, "y") == 0) {
                const void* p = nullptr;
                size_t n = 0;
                r = sd_bus_message_read_array(m, 'y', &p, &n);
                if (r < 0) return fail(error, "read byte array", r);
                auto* b = static_cast<const uint8_t*>(p);
                out.data = Value::Bytes(b, b + n);
                return true;
            }
            r = sd_bus_message_enter_container(m, 'a', contents);
            if (r < 0) return fail(error, "enter array", r);
            Value::Items items;
            while ((r = sd_bus_message_at_end(m, false)) == 0) {
                Value v;
                if (!read_value(m, v, error)) return false;
                items.push_back(std::move(v));
            }
            if (r < 0) return fail(error, "array end", r);
            r = sd_bus_message_exit_container(m);
            if (r < 0) return fail(error, "exit array", r);
            out.data = std::move(items);
            return true;
        }
        case 'r':
        case 'e': {
            out.sig = (type == 'r' ? "(" : "{") + std::string(contents ? contents : "") + (type == 'r' ? ")" : "}");
            r = sd_bus_message_enter_container(m, type, contents);
            if (r < 0) return fail(error, "enter struct", r);
            Value::Items items;
            while ((r = sd_bus_message_at_end(m, false)) == 0) {
                Value v;
                if (!read_value(m, v, error)) return false;
                items.push_back(std::move(v));
            }
            if (r < 0) return fail(error, "struct end", r);
            r = sd_bus_message_exit_container(m);
            if (r < 0) return fail(error, "exit struct", r);
            out.data = std::move(items);
            return true;
        }
        case 'v': {
            r = sd_bus_message_enter_container(m, 'v', contents);
            if (r < 0) return fail(error, "enter variant", r);
            auto inner = std::make_shared<Value>();
            if (!read_value(m, *inner, error)) return false;
            r = sd_bus_message_exit_container(m);
            if (r < 0) return fail(error, "exit variant", r);
            out.sig = "v";
            out.data = std::move(inner);
            return true;
        }
        default:
            if (error) *error = std::string("unsupported type '") + type + "'";
            return false;
    }
}

bool read_all(sd_bus_message* m, Args& out, std::string* error) {
    int r;
    while ((r = sd_bus_message_at_end(m, true)) == 0) {
        Value v;
        if (!read_value(m, v, error)) return false;
        out.push_back(std::move(v));
    }
    if (r < 0) return fail(error, "message end", r);
    return true;
}

// ---------------------------------------------------------------- writing

static std::string inner_of(const std::string& sig) {
    // "(su)" -> "su", "{sv}" -> "sv", "as" -> "s"
    if (sig.empty()) return {};
    if (sig[0] == 'a') return sig.substr(1);
    return sig.substr(1, sig.size() - 2);
}

int append_value(sd_bus_message* m, const Value& v) {
    char t = v.type();
    switch (t) {
        case 'b': {
            int b = v.as_bool() ? 1 : 0;
            return sd_bus_message_append_basic(m, 'b', &b);
        }
        case 'y': {
            uint8_t x = static_cast<uint8_t>(v.as_uint());
            return sd_bus_message_append_basic(m, 'y', &x);
        }
        case 'n': {
            int16_t x = static_cast<int16_t>(v.as_int());
            return sd_bus_message_append_basic(m, 'n', &x);
        }
        case 'q': {
            uint16_t x = static_cast<uint16_t>(v.as_uint());
            return sd_bus_message_append_basic(m, 'q', &x);
        }
        case 'i': {
            int32_t x = static_cast<int32_t>(v.as_int());
            return sd_bus_message_append_basic(m, 'i', &x);
        }
        case 'u': {
            uint32_t x = static_cast<uint32_t>(v.as_uint());
            return sd_bus_message_append_basic(m, 'u', &x);
        }
        case 'x': {
            int64_t x = v.as_int();
            return sd_bus_message_append_basic(m, 'x', &x);
        }
        case 't': {
            uint64_t x = v.as_uint();
            return sd_bus_message_append_basic(m, 't', &x);
        }
        case 'd': {
            double x = v.as_double();
            return sd_bus_message_append_basic(m, 'd', &x);
        }
        case 's':
        case 'o':
        case 'g': {
            auto* s = std::get_if<std::string>(&v.data);
            return sd_bus_message_append_basic(m, t, s ? s->c_str() : "");
        }
        case 'h': {
            int fd = v.as_fd();
            return sd_bus_message_append_basic(m, 'h', &fd);
        }
        case 'a': {
            std::string inner = inner_of(v.sig);
            if (auto* b = std::get_if<Value::Bytes>(&v.data)) {
                if (inner != "y") return -EINVAL;
                return sd_bus_message_append_array(m, 'y', b->data(), b->size());
            }
            int r = sd_bus_message_open_container(m, 'a', inner.c_str());
            if (r < 0) return r;
            for (auto& e : v.items()) {
                if (e.sig != inner) return -EINVAL;
                if ((r = append_value(m, e)) < 0) return r;
            }
            return sd_bus_message_close_container(m);
        }
        case '(':
        case '{': {
            std::string inner = inner_of(v.sig);
            int r = sd_bus_message_open_container(m, t == '(' ? 'r' : 'e', inner.c_str());
            if (r < 0) return r;
            for (auto& e : v.items())
                if ((r = append_value(m, e)) < 0) return r;
            return sd_bus_message_close_container(m);
        }
        case 'v': {
            auto* box = std::get_if<Value::Box>(&v.data);
            if (!box || !*box) return -EINVAL;
            int r = sd_bus_message_open_container(m, 'v', (*box)->sig.c_str());
            if (r < 0) return r;
            if ((r = append_value(m, **box)) < 0) return r;
            return sd_bus_message_close_container(m);
        }
        default:
            return -EINVAL;
    }
}

int append_all(sd_bus_message* m, const Args& args) {
    for (auto& a : args) {
        int r = append_value(m, a);
        if (r < 0) return r;
    }
    return 0;
}

}  // namespace brosys::dbus
