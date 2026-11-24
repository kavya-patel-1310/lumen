#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lumen {

struct Token {
    std::string term;     // normalized (lowercased + stemmed) form used for indexing
    std::string surface;  // lowercased original word, used for spelling suggestions
    uint32_t position;    // word position in the document (stopwords still advance it)
    uint32_t start;       // byte offset of the word in the source text
    uint32_t end;         // one past the last byte
};

class Tokenizer {
public:
    // Splits text into words, lowercases, stems, and drops stopwords.
    // Positions count every word (including stopwords) so phrase queries such as
    // "state of the art" keep their gaps intact.
    static std::vector<Token> tokenize(std::string_view text, bool keepStopwords = false);

    // Normalizes a single word the same way tokenize() does. Returns "" for stopwords.
    static std::string normalize(std::string_view word);

    static bool isStopword(std::string_view lowerWord);

    static constexpr size_t kMaxTokenLength = 64;
};

}  // namespace lumen
