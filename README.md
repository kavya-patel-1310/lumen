# Lumen — a private, local-first search engine in C++17

Lumen indexes the documents, notes and source code on your own machine and lets you search them with
ranked full-text search from the terminal or a web UI. Nothing leaves your computer. The server binds to
`127.0.0.1` by default, and the project uses no third-party libraries or cloud services.

```
$ lumen index ~/notes ~/projects
Found 7088 indexable files
Indexed 7088 new/changed files (83.4 MB) in 2.24s using 2 threads  [37.2 MB/s]
Index: 7088 docs, 131769 terms, 1634383 postings, 17.6 MB on disk (21% of corpus)

$ lumen search '"context manager" async'
36 results in 2.39 ms
1. contextlib.py  (21.023)
   /usr/lib/python3.12/contextlib.py
   … class AsyncExitStack(_BaseExitStack, AbstractAsyncContextManager): """Async context manager for …

$ lumen serve          # → http://127.0.0.1:8080
```

## Features

| Area | What it does |
|---|---|
| **Indexing** | Positional inverted index built by a multithreaded crawler (a thread pool tokenizes files; only the merge step takes a lock) |
| **Text processing** | Tokenizer that handles Unicode punctuation, a stopword filter, and a from-scratch **Porter stemmer** (`running → run`, `connection → connect`) |
| **Ranking** | **Okapi BM25** (k1 = 1.2, b = 0.75) with document-length normalization and a configurable boost for phrase matches |
| **Query language** | Implicit AND · `"exact phrases"` (checked against word positions) · `OR` · `-exclusion` · `prefix*` wildcards |
| **Smart fallbacks** | Falls back to OR ranking when no document has every term · *"Did you mean"* spelling suggestions using Damerau–Levenshtein distance |
| **Snippets** | Picks the text window that covers the most distinct query terms and highlights matches (HTML or ANSI terminal colors) |
| **Storage** | Custom binary format: delta + **varint-compressed** postings (about 21% of corpus size), **FNV-1a checksum** to catch corruption, atomic save (write to a temp file, then rename) |
| **Incremental updates** | Re-indexing only processes new or changed files (compared by mtime and size). Deleted files are marked with tombstones and **compacted** on save |
| **Server** | Dependency-free HTTP/1.1 server on raw sockets (POSIX and Winsock), a thread-pooled JSON API, an embedded single-page UI, and security headers (CSP, nosniff) |
| **Performance** | Postings are kept sorted so AND queries use a linear merge, phrase matching starts from the rarest term, top-k uses partial sort, and a thread-safe **LRU query cache** sits in front |
| **Quality** | 15 unit-test suites (132 assertions), clean under AddressSanitizer + UBSan, built with `-Wall -Wextra -Wpedantic` and no warnings, CI on Linux, macOS and Windows |

## Architecture

```
            ┌──────────────┐   paths    ┌──────────────────────────┐
  folders ─▶│   Crawler    │──────────▶ │  ThreadPool (N workers)  │
            │ (filters by  │            │  load → strip markup →   │
            │  ext, hidden,│            │  tokenize → stem         │
            │  size, binary)│           └────────────┬─────────────┘
            └──────────────┘                         │ per-doc term→positions
                                                     ▼  (merge under one mutex)
                                       ┌──────────────────────────┐
                                       │      InvertedIndex       │
                                       │ term → [doc, positions]  │◀──▶ lumen.idx
                                       │ docs, tombstones, stems  │     (varint + checksum)
                                       └────────────┬─────────────┘
                                                    │ read-only, concurrent
          query ─▶ QueryParser ─▶ Searcher ─────────┘
                    (AND/OR/NOT,   BM25 · phrase · prefix · suggest
                     phrase, *)    snippets · LRU cache
                                       │
                         ┌─────────────┴─────────────┐
                         ▼                           ▼
                    CLI (ANSI)              HttpServer → JSON API + Web UI
```

### Source layout

```
include/lumen/
  tokenizer.hpp       word splitting, positions, byte offsets
  stemmer.hpp         Porter stemming algorithm
  document_loader.hpp crawler, HTML/Markdown text extraction, binary detection
  index.hpp           inverted index, tombstones, compaction, persistence
  query.hpp           query language parser
  searcher.hpp        BM25 scoring, boolean evaluation, snippets, suggestions, LRU cache
  server.hpp          HTTP server + routing
  thread_pool.hpp     fixed-size worker pool
  util.hpp            varint, FNV-1a, escaping, timers
src/                  implementations + CLI (main.cpp) + embedded web UI
tests/tests.cpp       unit tests (self-contained, no framework needed)
```

## Build

Needs a C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+) and CMake 3.16+.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure      # or ./build/lumen_tests
```

On Windows, open the folder in Visual Studio (it detects CMake automatically), or run the same commands in
a Developer PowerShell. The binary ends up at `build\Release\lumen.exe`.

Sanitizer build: `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLUMEN_SANITIZE=ON`

## Usage

```bash
lumen index ~/Documents ~/code        # build or update ./lumen.idx (run again any time; only changes are processed)
lumen search "binary search -python"  # terminal search with highlighted snippets
lumen search 'optim*' -n 20
lumen serve --port 8080               # web UI at http://127.0.0.1:8080
lumen stats                           # index statistics
lumen bench --queries 5000            # latency benchmark (p50/p95/p99, QPS)
```

All commands accept `--index FILE` to use a different index file.

### HTTP API

| Endpoint | Description |
|---|---|
| `GET /api/search?q=…&limit=10&offset=0` | Ranked results: `total`, `tookMs`, `relaxed`, `suggestion`, `hits[{id, score, title, path, snippet}]` |
| `GET /api/doc?id=N` | Plain text of an indexed document (only serves files already in the index) |
| `GET /api/stats` | Document, term and posting counts |
| `GET /api/health` | Liveness check |

## Benchmarks

Measured on a 2-vCPU cloud VM using 7,088 real files (Python standard library, `/usr/include`, and
`/usr/share/doc`: 83.4 MB in total, 9.2 M tokens).

| Metric | Result |
|---|---|
| Full index build | **2.24 s** (37 MB/s, 2 threads) |
| Incremental re-index, nothing changed | **0.63 s** (load + stat 7,088 files + save) |
| Index size on disk | **17.6 MB** (21% of corpus, including word positions) |
| Query latency, ranking only | mean **0.04 ms**, p99 **0.28 ms** (~24k QPS, single thread) |
| Query latency, with snippets | p50 **3.6 ms** (each snippet re-reads its source file from disk) |
| Concurrent HTTP | 400 requests from 16 parallel clients, 0 errors |

Reproduce with `lumen index <dirs>` followed by `lumen bench`.

## Design notes

- **Why positions are counted through stopwords:** stopwords are left out of the index, but they still take
  up a word position. So `"state of the art"` is stored as `state@0, art@3`, and phrase matching still
  checks the gap correctly.
- **Concurrency model:** worker threads do the expensive part (file I/O, markup stripping, tokenizing,
  stemming) with no shared state. Each worker hands a finished `term → positions` map to the index, which
  takes a mutex only for the merge. Document IDs are assigned under that same lock, so every posting list
  stays sorted without a separate sorting step.
- **Deletes:** removing a document marks it with a tombstone (O(1)). Search skips tombstoned documents,
  and `save()` compacts the index: document IDs are renumbered in order, so posting lists stay sorted.
- **On-disk format:** `magic | version | docs | terms{term, surface, df, [Δdoc, tf, Δpos…]} | fnv1a64`.
  Every read is bounds-checked, so a truncated or corrupted file is rejected instead of crashing the
  program.

## Roadmap

- PDF/DOCX text extraction
- Filesystem watcher (inotify / FSEvents / ReadDirectoryChangesW) for live updates
- Memory-mapped segments with background merging (LSM-style) for corpora larger than RAM
- Fuzzy matching with a BK-tree; embedding-based semantic re-ranking

## License

MIT
