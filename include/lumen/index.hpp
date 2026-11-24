#pragma once
// Positional inverted index with tombstone deletes, compaction and a
// checksummed, varint-compressed on-disk format.

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lumen/tokenizer.hpp"

namespace lumen {

using DocId = uint32_t;

struct DocInfo {
    std::string path;
    std::string title;
    uint32_t length = 0;  // number of indexed tokens
    int64_t mtime = 0;
    uint64_t size = 0;
    bool deleted = false;
};

struct Posting {
    DocId doc = 0;
    std::vector<uint32_t> positions;  // sorted word positions
    uint32_t tf() const { return static_cast<uint32_t>(positions.size()); }
};

struct IndexStats {
    size_t documents = 0;
    size_t terms = 0;
    size_t postings = 0;
    uint64_t positions = 0;
    uint64_t totalTokens = 0;
    uint64_t corpusBytes = 0;
    double avgDocLength = 0.0;
};

class InvertedIndex {
public:
    // Thread-safe: tokenization happens outside the lock, only the merge is serialized.
    DocId addDocument(DocInfo info, const std::vector<Token>& tokens);
    bool removeDocument(DocId id);
    std::optional<DocId> findByPath(const std::string& path) const;

    // Read-side API (safe for concurrent readers once indexing is finished).
    const std::vector<Posting>* postings(const std::string& term) const;
    size_t documentFrequency(const std::string& term) const;
    const DocInfo& doc(DocId id) const { return docs_[id]; }
    size_t docSlots() const { return docs_.size(); }
    size_t liveDocuments() const { return liveDocs_; }
    double avgDocLength() const;
    std::vector<std::string> expandPrefix(const std::string& prefix, size_t limit) const;
    const std::unordered_map<std::string, std::string>& surfaceForms() const { return surface_; }
    IndexStats stats() const;

    // Drops tombstoned documents and renumbers ids densely.
    void compact();

    // Atomic save (write temp file, then rename). Compacts first.
    bool save(const std::string& file, std::string* error = nullptr);
    bool load(const std::string& file, std::string* error = nullptr);

    static constexpr uint32_t kFormatVersion = 1;

private:
    std::unordered_map<std::string, std::vector<Posting>> terms_;
    std::unordered_map<std::string, std::string> surface_;  // stem -> a readable surface form
    std::vector<DocInfo> docs_;
    std::unordered_map<std::string, DocId> pathToId_;
    uint64_t totalLength_ = 0;
    size_t liveDocs_ = 0;

    std::mutex writeMu_;
    mutable std::mutex sortedMu_;
    mutable std::vector<std::string> sortedTerms_;
    mutable bool sortedDirty_ = true;
};

}  // namespace lumen
