#pragma once
// BM25 ranking, phrase/prefix/boolean evaluation, snippets, spelling
// suggestions and an LRU result cache. Thread-safe for concurrent queries.

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lumen/index.hpp"
#include "lumen/query.hpp"

namespace lumen {

enum class SnippetStyle { None, Plain, Html, Ansi };

struct SearchOptions {
    size_t limit = 10;
    size_t offset = 0;
    SnippetStyle snippets = SnippetStyle::Html;
    bool useCache = true;
};

struct SearchHit {
    DocId doc = 0;
    double score = 0.0;
    std::string path;
    std::string title;
    std::string snippet;
};

struct SearchResponse {
    std::vector<SearchHit> hits;
    size_t totalMatches = 0;
    double elapsedMicros = 0.0;
    bool relaxed = false;     // no doc matched every term, fell back to OR
    bool cached = false;
    std::string suggestion;   // "did you mean" rewrite, empty if none
};

struct Bm25Params {
    double k1 = 1.2;
    double b = 0.75;
    double phraseBoost = 1.5;
};

class Searcher {
public:
    explicit Searcher(const InvertedIndex& index, size_t cacheCapacity = 512, Bm25Params params = {});

    SearchResponse search(std::string_view query, const SearchOptions& opts = {}) const;
    void clearCache() const;

    // Exposed for testing.
    static size_t editDistance(std::string_view a, std::string_view b, size_t cap);

private:
    using ScoreList = std::vector<std::pair<DocId, double>>;  // sorted by DocId

    double idf(size_t df) const;
    double bm25(uint32_t tf, uint32_t docLen, double idfValue) const;
    ScoreList scoreTerm(const std::string& term) const;
    ScoreList scorePrefix(const std::string& prefix) const;
    ScoreList scorePhrase(const QueryItem& item) const;
    ScoreList scoreItem(const QueryItem& item) const;
    std::string suggest(std::string_view query, const ParsedQuery& pq) const;
    std::string makeSnippet(DocId doc, const ParsedQuery& pq, SnippetStyle style) const;

    const InvertedIndex& index_;
    Bm25Params params_;

    // LRU cache
    size_t cacheCapacity_;
    mutable std::mutex cacheMu_;
    mutable std::list<std::pair<std::string, SearchResponse>> lru_;
    mutable std::unordered_map<std::string, std::list<std::pair<std::string, SearchResponse>>::iterator> cache_;
};

}  // namespace lumen
