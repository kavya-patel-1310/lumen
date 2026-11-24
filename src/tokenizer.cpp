#include "lumen/tokenizer.hpp"

#include <unordered_set>

#include "lumen/stemmer.hpp"
#include "lumen/util.hpp"

namespace lumen {
namespace {

const std::unordered_set<std::string_view>& stopwords() {
    static const std::unordered_set<std::string_view> words = {
        "a", "an", "and", "are", "as", "at", "be", "but", "by", "for", "if", "in", "into", "is", "it",
        "no", "not", "of", "on", "or", "such", "that", "the", "their", "then", "there", "these", "they",
        "this", "to", "was", "will", "with", "from", "has", "have", "had", "were", "been", "its", "than",
        "so", "can", "do", "does", "did", "which", "who", "whom", "what", "when", "where", "why", "how",
        "all", "any", "each", "our", "we", "you", "your", "he", "she", "him", "her", "i", "me", "my"};
    return words;
}

inline bool isWordByte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
}

// Byte length of a Unicode punctuation/space sequence starting at i, or 0.
// Covers NBSP, « », General Punctuation (U+2000–U+206F: dashes, quotes, ellipsis)
// and CJK symbols/punctuation (U+3000–U+303F).
inline size_t unicodeSeparatorLen(std::string_view s, size_t i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    const auto b1 = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
    if (b0 == 0xC2 && (b1 == 0xA0 || b1 == 0xAB || b1 == 0xBB || b1 == 0xB7)) return 2;
    if (i + 2 < s.size() && ((b0 == 0xE2 && (b1 == 0x80 || b1 == 0x81)) || (b0 == 0xE3 && b1 == 0x80))) return 3;
    return 0;
}

inline bool allAlpha(const std::string& s) {
    for (char c : s)
        if (c < 'a' || c > 'z') return false;
    return true;
}

}  // namespace

bool Tokenizer::isStopword(std::string_view lowerWord) { return stopwords().count(lowerWord) > 0; }

std::string Tokenizer::normalize(std::string_view word) {
    std::string lower = toLowerAscii(word);
    if (lower.empty() || lower.size() > kMaxTokenLength || isStopword(lower)) return {};
    return allAlpha(lower) ? porterStem(lower) : lower;
}

std::vector<Token> Tokenizer::tokenize(std::string_view text, bool keepStopwords) {
    std::vector<Token> tokens;
    tokens.reserve(text.size() / 6);
    uint32_t position = 0;
    size_t i = 0;
    const size_t n = text.size();
    auto wordAt = [&](size_t k) {
        return isWordByte(static_cast<unsigned char>(text[k])) && unicodeSeparatorLen(text, k) == 0;
    };
    while (i < n) {
        while (i < n && !wordAt(i)) {
            const size_t sep = unicodeSeparatorLen(text, i);
            i += sep ? sep : 1;
        }
        if (i >= n) break;
        const size_t start = i;
        while (i < n && wordAt(i)) ++i;
        const size_t len = i - start;
        const uint32_t pos = position++;
        if (len > kMaxTokenLength) continue;

        std::string lower = toLowerAscii(text.substr(start, len));
        const bool stop = isStopword(lower);
        if (stop && !keepStopwords) continue;

        Token t;
        t.term = (stop || !allAlpha(lower)) ? lower : porterStem(lower);
        t.surface = std::move(lower);
        t.position = pos;
        t.start = static_cast<uint32_t>(start);
        t.end = static_cast<uint32_t>(i);
        tokens.push_back(std::move(t));
    }
    return tokens;
}

}  // namespace lumen
