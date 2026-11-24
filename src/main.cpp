// lumen — a private, local-first full-text search engine.
//
//   lumen index <dir|file>... [--index FILE] [--threads N]
//   lumen search "<query>"    [--index FILE] [-n N]
//   lumen serve               [--index FILE] [--host 127.0.0.1] [--port 8080]
//   lumen stats               [--index FILE]
//   lumen bench               [--index FILE] [--queries N]

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "lumen/document_loader.hpp"
#include "lumen/index.hpp"
#include "lumen/searcher.hpp"
#include "lumen/server.hpp"
#include "lumen/thread_pool.hpp"
#include "lumen/util.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace lumen;

namespace {

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::string indexFile = "lumen.idx";
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    size_t threads = 0;
    size_t limit = 10;
    size_t queries = 2000;
};

void usage() {
    std::cout <<
R"(lumen — private full-text search engine

Usage:
  lumen index <dir|file>... [--index FILE] [--threads N]   Build or incrementally update an index
  lumen search "<query>"    [--index FILE] [-n N]          Search from the terminal
  lumen serve               [--index FILE] [--host H] [--port P]  Web UI + JSON API
  lumen stats               [--index FILE]                 Show index statistics
  lumen bench               [--index FILE] [--queries N]   Measure query latency

Query syntax:
  word1 word2        documents containing both words (BM25 ranked)
  "exact phrase"     words adjacent, in order
  cache OR buffer    either word
  -word              exclude documents containing word
  prefix*            any word starting with prefix

Default index file: ./lumen.idx
)";
}

bool parseArgs(int argc, char** argv, Args& a) {
    if (argc < 2) return false;
    a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        try {
            if (s == "--index" || s == "-i") a.indexFile = next("--index");
            else if (s == "--host") a.host = next("--host");
            else if (s == "--port" || s == "-p") a.port = static_cast<uint16_t>(std::stoi(next("--port")));
            else if (s == "--threads" || s == "-j") a.threads = std::stoul(next("--threads"));
            else if (s == "-n" || s == "--limit") a.limit = std::stoul(next("--limit"));
            else if (s == "--queries") a.queries = std::stoul(next("--queries"));
            else if (s == "-h" || s == "--help") return false;
            else a.positional.push_back(s);
        } catch (const std::exception&) {
            std::cerr << "invalid number for " << s << "\n";
            std::exit(2);
        }
    }
    return true;
}

std::string humanBytes(double b) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int u = 0;
    while (b >= 1024 && u < 3) {
        b /= 1024;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", b, units[u]);
    return buf;
}

bool loadIndex(InvertedIndex& idx, const std::string& file) {
    std::string err;
    if (!idx.load(file, &err)) {
        std::cerr << "error: " << err << "\n(run `lumen index <folder>` first)\n";
        return false;
    }
    return true;
}

int cmdIndex(const Args& a) {
    if (a.positional.empty()) {
        std::cerr << "usage: lumen index <dir|file>...\n";
        return 2;
    }
    const Timer total;
    InvertedIndex idx;
    if (fs::exists(a.indexFile)) {
        std::string err;
        if (idx.load(a.indexFile, &err)) {
            std::cout << "Loaded existing index (" << idx.liveDocuments() << " docs) — updating incrementally\n";
        } else {
            std::cerr << "warning: ignoring unreadable index (" << err << "), rebuilding\n";
        }
    }

    const auto files = crawl(a.positional);
    std::cout << "Found " << files.size() << " indexable files\n";

    // Decide which files are new or changed (by mtime + size).
    std::vector<fs::path> todo;
    std::unordered_set<std::string> seen;
    size_t unchanged = 0, updated = 0;
    for (const auto& f : files) {
        const std::string path = normalizePath(f);
        seen.insert(path);
        std::error_code ec;
        const auto size = fs::file_size(f, ec);
        if (auto id = idx.findByPath(path)) {
            const auto& d = idx.doc(*id);
            if (d.mtime == fileMtime(f) && d.size == size) {
                ++unchanged;
                continue;
            }
            idx.removeDocument(*id);
            ++updated;
        }
        todo.push_back(f);
    }

    // Remove documents that disappeared from the crawled roots.
    std::vector<std::string> rootPaths;
    for (const auto& r : a.positional) rootPaths.push_back(normalizePath(r));
    size_t removed = 0;
    for (DocId id = 0; id < idx.docSlots(); ++id) {
        const auto& d = idx.doc(id);
        if (d.deleted || seen.count(d.path)) continue;
        const bool underRoot = std::any_of(rootPaths.begin(), rootPaths.end(),
                                           [&](const std::string& r) { return d.path.rfind(r, 0) == 0; });
        if (underRoot && idx.removeDocument(id)) ++removed;
    }

    // Parallel load + tokenize; the index merge is the only serialized step.
    const size_t threads = a.threads ? a.threads : std::max(1u, std::thread::hardware_concurrency());
    std::atomic<size_t> done{0}, bytes{0}, skipped{0};
    const Timer indexTimer;
    {
        ThreadPool pool(threads);
        std::mutex printMu;
        for (const auto& f : todo) {
            pool.submit([&, f] {
                auto doc = loadDocument(f);
                if (!doc) {
                    ++skipped;
                    return;
                }
                auto tokens = Tokenizer::tokenize(doc->text);
                bytes += doc->size;
                DocInfo info;
                info.path = std::move(doc->path);
                info.title = std::move(doc->title);
                info.mtime = doc->mtime;
                info.size = doc->size;
                idx.addDocument(std::move(info), tokens);
                const size_t n = ++done;
                if (n % 500 == 0) {
                    std::lock_guard<std::mutex> lock(printMu);
                    std::cout << "\r  indexed " << n << "/" << todo.size() << std::flush;
                }
            });
        }
        pool.wait();
    }
    const double secs = indexTimer.seconds();
    if (todo.size() >= 500) std::cout << "\r" << std::string(40, ' ') << "\r";

    const Timer saveTimer;
    std::string err;
    if (!idx.save(a.indexFile, &err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    std::error_code ec;
    const auto idxSize = fs::file_size(a.indexFile, ec);
    const auto s = idx.stats();

    std::printf("Indexed %zu new/changed files (%s) in %.2fs using %zu threads", done.load(),
                humanBytes(static_cast<double>(bytes.load())).c_str(), secs, threads);
    if (secs > 0 && bytes > 0) std::printf("  [%.1f MB/s]", bytes.load() / 1048576.0 / secs);
    std::printf("\n  unchanged: %zu   updated: %zu   removed: %zu   skipped (binary/unreadable): %zu\n", unchanged,
                updated, removed, skipped.load());
    std::printf("Index: %zu docs, %zu terms, %zu postings, %s on disk (%.0f%% of corpus), saved in %.0f ms\n",
                s.documents, s.terms, s.postings, humanBytes(static_cast<double>(idxSize)).c_str(),
                s.corpusBytes ? 100.0 * static_cast<double>(idxSize) / static_cast<double>(s.corpusBytes) : 0.0,
                saveTimer.millis());
    std::printf("Total time: %.2fs  ->  %s\n", total.seconds(), a.indexFile.c_str());
    return 0;
}

int cmdSearch(const Args& a) {
    if (a.positional.empty()) {
        std::cerr << "usage: lumen search \"<query>\"\n";
        return 2;
    }
    std::string query;
    for (const auto& p : a.positional) query += (query.empty() ? "" : " ") + p;

    InvertedIndex idx;
    if (!loadIndex(idx, a.indexFile)) return 1;
    Searcher searcher(idx);
    SearchOptions opts;
    opts.limit = a.limit;
    opts.snippets = SnippetStyle::Ansi;
    const auto r = searcher.search(query, opts);

    std::printf("\033[2m%zu results in %.2f ms%s\033[0m\n", r.totalMatches, r.elapsedMicros / 1000.0,
                r.relaxed ? " (partial matches)" : "");
    if (!r.suggestion.empty()) std::printf("Did you mean: \033[1m%s\033[0m ?\n", r.suggestion.c_str());
    for (size_t i = 0; i < r.hits.size(); ++i) {
        const auto& h = r.hits[i];
        std::printf("\n\033[1;34m%zu. %s\033[0m  \033[2m(%.3f)\033[0m\n   \033[32m%s\033[0m\n   %s\n", i + 1,
                    h.title.c_str(), h.score, h.path.c_str(), h.snippet.c_str());
    }
    return 0;
}

int cmdServe(const Args& a) {
    InvertedIndex idx;
    if (!loadIndex(idx, a.indexFile)) return 1;
    Searcher searcher(idx);
    HttpServer server(idx, searcher, a.host, a.port, a.threads ? a.threads : 8);
    if (a.host != "127.0.0.1" && a.host != "localhost")
        std::cerr << "warning: listening on " << a.host << " exposes your index to the network\n";
    return server.run();
}

int cmdStats(const Args& a) {
    const Timer t;
    InvertedIndex idx;
    if (!loadIndex(idx, a.indexFile)) return 1;
    const double loadMs = t.millis();
    const auto s = idx.stats();
    std::error_code ec;
    const auto idxSize = fs::file_size(a.indexFile, ec);
    std::printf("Index file      %s (%s, loaded in %.0f ms)\n", a.indexFile.c_str(),
                humanBytes(static_cast<double>(idxSize)).c_str(), loadMs);
    std::printf("Documents       %zu\n", s.documents);
    std::printf("Corpus size     %s\n", humanBytes(static_cast<double>(s.corpusBytes)).c_str());
    std::printf("Unique terms    %zu\n", s.terms);
    std::printf("Postings        %zu\n", s.postings);
    std::printf("Positions       %llu\n", static_cast<unsigned long long>(s.positions));
    std::printf("Avg doc length  %.1f tokens\n", s.avgDocLength);
    return 0;
}

int cmdBench(const Args& a) {
    InvertedIndex idx;
    if (!loadIndex(idx, a.indexFile)) return 1;
    Searcher searcher(idx, 0);

    // Sample query words from the vocabulary, favouring moderately common terms.
    std::vector<std::string> pool;
    for (const auto& [stem, surface] : idx.surfaceForms()) {
        const size_t df = idx.documentFrequency(stem);
        if (df >= 3 && surface.size() >= 4) pool.push_back(surface);
    }
    if (pool.empty()) {
        std::cerr << "index too small to benchmark\n";
        return 1;
    }
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> pick(0, pool.size() - 1), words(1, 3), kind(0, 9);

    std::vector<std::string> queries;
    for (size_t i = 0; i < a.queries; ++i) {
        std::string q;
        const size_t k = kind(rng);
        if (k == 0) q = "\"" + pool[pick(rng)] + " " + pool[pick(rng)] + "\"";
        else if (k == 1) q = pool[pick(rng)].substr(0, 4) + "*";
        else {
            const size_t n = words(rng);
            for (size_t w = 0; w < n; ++w) q += (w ? " " : "") + pool[pick(rng)];
        }
        queries.push_back(q);
    }

    auto runBench = [&](SnippetStyle style, size_t count) {
        SearchOptions opts;
        opts.limit = 10;
        opts.snippets = style;
        opts.useCache = false;
        std::vector<double> lat;
        lat.reserve(count);
        const Timer wall;
        for (size_t i = 0; i < count; ++i) lat.push_back(searcher.search(queries[i % queries.size()], opts).elapsedMicros);
        const double secs = wall.seconds();
        std::sort(lat.begin(), lat.end());
        auto pct = [&](double p) { return lat[std::min(lat.size() - 1, static_cast<size_t>(p * lat.size()))] / 1000.0; };
        double sum = 0;
        for (double l : lat) sum += l;
        std::printf("  %-22s n=%-6zu mean %.3f ms   p50 %.3f   p95 %.3f   p99 %.3f   (%.0f qps)\n",
                    style == SnippetStyle::None ? "ranking only" : "ranking + snippets", count,
                    sum / lat.size() / 1000.0, pct(0.50), pct(0.95), pct(0.99), count / secs);
    };

    std::printf("Benchmark over %zu docs, %zu terms (single thread, cache disabled)\n", idx.liveDocuments(),
                idx.stats().terms);
    runBench(SnippetStyle::None, queries.size());
    runBench(SnippetStyle::Plain, std::min<size_t>(queries.size(), 300));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Enable UTF-8 output and ANSI colors in the Windows console.
    SetConsoleOutputCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    Args a;
    if (!parseArgs(argc, argv, a) || a.command == "help") {
        usage();
        return argc < 2 ? 1 : 0;
    }
    if (a.command == "index") return cmdIndex(a);
    if (a.command == "search") return cmdSearch(a);
    if (a.command == "serve") return cmdServe(a);
    if (a.command == "stats") return cmdStats(a);
    if (a.command == "bench") return cmdBench(a);
    std::cerr << "unknown command: " << a.command << "\n\n";
    usage();
    return 2;
}
