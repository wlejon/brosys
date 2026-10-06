#pragma once

#include "embed/embed.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace brosys::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

inline double numVal(Value v) {
    if (ev::isObject(v)) return 0.0;
    double d = ev::toDouble(v);
    return std::isnan(d) ? 0.0 : d;
}

inline int32_t i32Val(Value v) {
    const double d = numVal(v);
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return static_cast<int32_t>(d);
}

inline uint32_t u32Val(Value v) {
    const double d = numVal(v);
    if (d >= 4294967295.0) return UINT32_MAX;
    if (!(d > 0)) return 0;
    return static_cast<uint32_t>(d);
}

inline int64_t i64Val(Value v) {
    const double d = numVal(v);
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return static_cast<int64_t>(d);
}

inline uint64_t u64Val(Value v) {
    if (ev::isObject(v)) return 0;
    return ev::toUint64(v);
}

inline bool boolVal(Value v) {
    return ev::toBool(v);
}

inline std::string strVal(Value v) {
    if (ev::isUndefined(v) || ev::isNull(v) || ev::isSymbol(v)) return "";
    return ev::toUtf8(v);
}

inline double numAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0.0;
    return numVal(args[i]);
}

inline int32_t i32At(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0;
    return i32Val(args[i]);
}

inline uint32_t u32At(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0;
    return u32Val(args[i]);
}

inline int64_t i64At(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0;
    return i64Val(args[i]);
}

inline uint64_t u64At(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0;
    return u64Val(args[i]);
}

inline bool boolAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return false;
    return boolVal(args[i]);
}

inline std::string strAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return "";
    return strVal(args[i]);
}

inline Value argAt(std::span<const Value> args, size_t i) {
    return i < args.size() ? args[i] : ev::undefined();
}

inline bool hasArg(std::span<const Value> args, size_t i) {
    return i < args.size() && !ev::isUndefined(args[i]);
}

class ArgReader {
public:
    explicit ArgReader(std::span<const Value> args) : args_(args) {}

    double getDouble(size_t i, double def = 0.0) const {
        return hasArg(args_, i) ? numAt(args_, i) : def;
    }
    int32_t getInt(size_t i, int32_t def = 0) const {
        return hasArg(args_, i) ? i32At(args_, i) : def;
    }
    uint32_t getUint(size_t i, uint32_t def = 0) const {
        return hasArg(args_, i) ? u32At(args_, i) : def;
    }
    int64_t getInt64(size_t i, int64_t def = 0) const {
        return hasArg(args_, i) ? i64At(args_, i) : def;
    }
    bool getBool(size_t i, bool def = false) const {
        return hasArg(args_, i) ? boolAt(args_, i) : def;
    }
    std::string getString(size_t i, const std::string& def = "") const {
        return hasArg(args_, i) ? strAt(args_, i) : def;
    }
    Value get(size_t i) const {
        return argAt(args_, i);
    }
    bool has(size_t i) const {
        return hasArg(args_, i);
    }
    size_t count() const {
        return args_.size();
    }

private:
    std::span<const Value> args_;
};

} // namespace brosys::api
