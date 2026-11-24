#pragma once
// Small shared helpers: timing, varint coding, escaping, hashing.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace lumen {

class Timer {
public:
    Timer() : start_(std::chrono::steady_clock::now()) {}
    void reset() { start_ = std::chrono::steady_clock::now(); }
    double micros() const {
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start_).count();
    }
    double millis() const { return micros() / 1000.0; }
    double seconds() const { return micros() / 1e6; }

private:
    std::chrono::steady_clock::time_point start_;
};

// LEB128-style variable-length integer encoding used for compressed postings.
inline void putVarint(std::string& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

inline bool getVarint(const char*& p, const char* end, uint64_t& v) {
    v = 0;
    int shift = 0;
    while (p < end && shift < 64) {
        const auto b = static_cast<uint8_t>(*p++);
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
    }
    return false;
}

inline uint64_t fnv1a64(std::string_view data) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

inline char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = asciiLower(c);
    return out;
}

inline std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

inline std::string htmlEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

}  // namespace lumen
