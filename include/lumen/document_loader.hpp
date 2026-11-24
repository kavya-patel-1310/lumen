#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

struct LoadedDocument {
    std::string path;   // absolute, normalized path
    std::string title;  // <title>, first markdown heading, or file name
    std::string text;   // plain text with markup stripped
    int64_t mtime = 0;
    uint64_t size = 0;
};

struct CrawlOptions {
    uint64_t maxFileBytes = 8ull * 1024 * 1024;  // skip anything larger
    bool includeHidden = false;                  // skip dot-files / dot-dirs by default
};

// Recursively finds indexable text files under the given roots.
std::vector<std::filesystem::path> crawl(const std::vector<std::string>& roots, const CrawlOptions& opts = {});

bool isIndexableExtension(const std::filesystem::path& p);

// Reads and extracts text from a file. Returns nullopt for unreadable or binary files.
std::optional<LoadedDocument> loadDocument(const std::filesystem::path& p, uint64_t maxBytes = 8ull * 1024 * 1024);

// Strips markup (HTML) and pulls out a title. Exposed for testing.
std::string extractText(const std::string& raw, const std::string& extension, std::string& titleOut);

int64_t fileMtime(const std::filesystem::path& p);
std::string normalizePath(const std::filesystem::path& p);

}  // namespace lumen
