// CoreFoundation helpers shared by the macOS backends: an owning reference
// and typed reads that answer nullopt instead of trusting a value's type.
#pragma once

#include <CoreFoundation/CoreFoundation.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace brosys::mac {

// Owns one CF reference (Create / Copy rule); `retain` takes a Get-rule one.
template <class T>
class CFRef {
public:
    CFRef() = default;
    explicit CFRef(T ref) : ref_(ref) {}
    static CFRef retain(T ref) {
        if (ref) CFRetain(ref);
        return CFRef(ref);
    }
    CFRef(const CFRef& o) : ref_(o.ref_) {
        if (ref_) CFRetain(ref_);
    }
    CFRef(CFRef&& o) noexcept : ref_(std::exchange(o.ref_, nullptr)) {}
    CFRef& operator=(CFRef o) noexcept {
        std::swap(ref_, o.ref_);
        return *this;
    }
    ~CFRef() {
        if (ref_) CFRelease(ref_);
    }
    T get() const { return ref_; }
    explicit operator bool() const { return ref_ != nullptr; }

private:
    T ref_ = nullptr;
};

// UTF-8 of a CFString ("" for null or a non-string).
std::string to_utf8(CFTypeRef s);
CFRef<CFStringRef> make_string(const std::string& utf8);

// Typed dictionary reads: nullopt / null when the key is missing or holds another type.
CFTypeRef dict_value(CFDictionaryRef d, CFStringRef key);
std::optional<std::string> dict_string(CFDictionaryRef d, CFStringRef key);
std::optional<int64_t> dict_int(CFDictionaryRef d, CFStringRef key);
std::optional<double> dict_double(CFDictionaryRef d, CFStringRef key);
std::optional<bool> dict_bool(CFDictionaryRef d, CFStringRef key);
CFDictionaryRef dict_dict(CFDictionaryRef d, CFStringRef key);
CFArrayRef dict_array(CFDictionaryRef d, CFStringRef key);

std::vector<std::string> string_array(CFArrayRef a);  // non-strings skipped
std::vector<int64_t> int_array(CFArrayRef a);         // non-numbers skipped

// "what: OSStatus -1744" (with the four-character code when printable).
std::string os_status(const char* what, int32_t status);

}  // namespace brosys::mac
