#include "mac/cf.h"

namespace brosys::mac {

std::string to_utf8(CFTypeRef v) {
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return {};
    auto s = static_cast<CFStringRef>(v);
    if (const char* fast = CFStringGetCStringPtr(s, kCFStringEncodingUTF8)) return fast;
    CFIndex len = CFStringGetLength(s);
    CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(max), '\0');
    CFIndex used = 0;
    CFStringGetBytes(s, CFRangeMake(0, len), kCFStringEncodingUTF8, '?', false,
                     reinterpret_cast<UInt8*>(out.data()), max, &used);
    out.resize(static_cast<size_t>(used));
    return out;
}

CFRef<CFStringRef> make_string(const std::string& utf8) {
    return CFRef<CFStringRef>(CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(utf8.data()),
                                                      static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8,
                                                      false));
}

CFTypeRef dict_value(CFDictionaryRef d, CFStringRef key) {
    if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) return nullptr;
    return CFDictionaryGetValue(d, key);
}

std::optional<std::string> dict_string(CFDictionaryRef d, CFStringRef key) {
    CFTypeRef v = dict_value(d, key);
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return std::nullopt;
    return to_utf8(v);
}

std::optional<int64_t> dict_int(CFDictionaryRef d, CFStringRef key) {
    CFTypeRef v = dict_value(d, key);
    if (!v) return std::nullopt;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) return CFBooleanGetValue(static_cast<CFBooleanRef>(v)) ? 1 : 0;
    if (CFGetTypeID(v) != CFNumberGetTypeID()) return std::nullopt;
    int64_t n = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &n)) return std::nullopt;
    return n;
}

std::optional<double> dict_double(CFDictionaryRef d, CFStringRef key) {
    CFTypeRef v = dict_value(d, key);
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return std::nullopt;
    double x = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberDoubleType, &x)) return std::nullopt;
    return x;
}

std::optional<bool> dict_bool(CFDictionaryRef d, CFStringRef key) {
    CFTypeRef v = dict_value(d, key);
    if (!v) return std::nullopt;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) return CFBooleanGetValue(static_cast<CFBooleanRef>(v)) != 0;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        int64_t n = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &n);
        return n != 0;
    }
    return std::nullopt;
}

CFDictionaryRef dict_dict(CFDictionaryRef d, CFStringRef key) {
    CFTypeRef v = dict_value(d, key);
    return v && CFGetTypeID(v) == CFDictionaryGetTypeID() ? static_cast<CFDictionaryRef>(v) : nullptr;
}

CFArrayRef dict_array(CFDictionaryRef d, CFStringRef key) {
    CFTypeRef v = dict_value(d, key);
    return v && CFGetTypeID(v) == CFArrayGetTypeID() ? static_cast<CFArrayRef>(v) : nullptr;
}

std::vector<std::string> string_array(CFArrayRef a) {
    std::vector<std::string> out;
    if (!a) return out;
    for (CFIndex i = 0, n = CFArrayGetCount(a); i < n; ++i) {
        CFTypeRef v = CFArrayGetValueAtIndex(a, i);
        if (v && CFGetTypeID(v) == CFStringGetTypeID()) out.push_back(to_utf8(v));
    }
    return out;
}

std::vector<int64_t> int_array(CFArrayRef a) {
    std::vector<int64_t> out;
    if (!a) return out;
    for (CFIndex i = 0, n = CFArrayGetCount(a); i < n; ++i) {
        CFTypeRef v = CFArrayGetValueAtIndex(a, i);
        int64_t x = 0;
        if (v && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &x))
            out.push_back(x);
    }
    return out;
}

std::string os_status(const char* what, int32_t status) {
    std::string s = std::string(what) + ": OSStatus " + std::to_string(status);
    auto u = static_cast<uint32_t>(status);
    char cc[4] = {char(u >> 24), char(u >> 16), char(u >> 8), char(u)};
    bool printable = true;
    for (char c : cc)
        if (c < 0x20 || c > 0x7e) printable = false;
    if (printable) s += std::string(" ('") + std::string(cc, 4) + "')";
    return s;
}

}  // namespace brosys::mac
