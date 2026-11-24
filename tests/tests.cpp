// Self-contained unit tests (no external framework required).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "lumen/document_loader.hpp"
#include "lumen/index.hpp"
#include "lumen/query.hpp"
#include "lumen/searcher.hpp"
#include "lumen/server.hpp"
#include "lumen/stemmer.hpp"
#include "lumen/tokenizer.hpp"
#include "lumen/util.hpp"

namespace fs = std::filesystem;
using namespace lumen;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        ++g_checks;                                                                                       \
        auto va = (a);                                                                                    \
        auto vb = (b);                                                                                    \
        if (!(va == vb)) {                                                                                \
            ++g_failures;                                                                                 \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " #a " == " #b "  (" << va << " vs " \
                      << vb << ")\n";                                                                     \
        }                                                                                                 \
    } while (0)

struct TestCase {
    const char* name;
    std::function<void()> fn;
};
static std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
struct Register {
    Register(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
#define TEST(name)                               \
    static void name();                          \
    static Register reg_##name(#name, name);     \
    static void name()

// ---------- fixture: a tiny on-disk corpus ----------

struct Corpus {
    fs::path dir;
    Corpus() {
        dir = fs::temp_directory_path() / ("lumen_test_" + std::to_string(std::rand()));
        fs::create_directories(dir);
    }
    ~Corpus() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path write(const std::string& name, const std::string& body) const {
        const fs::path p = dir / name;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << body;
        return p;
    }
};

static void addFile(InvertedIndex& idx, const fs::path& p) {
    auto doc = loadDocument(p);
    if (!doc) return;
    DocInfo info;
    info.path = doc->path;
    info.title = doc->title;
    info.mtime = doc->mtime;
    info.size = doc->size;
    idx.addDocument(info, Tokenizer::tokenize(doc->text));
}

static std::vector<std::string> titles(const SearchResponse& r) {
    std::vector<std::string> t;
    for (const auto& h : r.hits) t.push_back(h.title);
    return t;
}

// ---------- tests ----------

TEST(porter_stemmer_reference_pairs) {
    const std::pair<const char*, const char*> cases[] = {
        {"caresses", "caress"}, {"ponies", "poni"},     {"cats", "cat"},          {"feed", "feed"},
        {"agreed", "agre"},     {"plastered", "plaster"}, {"motoring", "motor"},  {"sing", "sing"},
        {"hopping", "hop"},     {"falling", "fall"},    {"filing", "file"},       {"happy", "happi"},
        {"relational", "relat"}, {"conditional", "condit"}, {"generalization", "gener"},
        {"running", "run"},     {"connection", "connect"}, {"connected", "connect"}, {"searching", "search"},
        {"electricity", "electr"}, {"adjustable", "adjust"}, {"controll", "control"}, {"roll", "roll"}};
    for (const auto& [in, out] : cases) CHECK_EQ(porterStem(in), std::string(out));
    CHECK_EQ(porterStem("c++"), std::string("c++"));
    CHECK_EQ(porterStem("is"), std::string("is"));
}

TEST(tokenizer_positions_and_stopwords) {
    const auto toks = Tokenizer::tokenize("The Quick brown foxes, jumping over the lazy dog!");
    std::vector<std::string> terms;
    for (const auto& t : toks) terms.push_back(t.term);
    CHECK_EQ(terms.size(), size_t(7));  // "the" x2 removed
    CHECK_EQ(terms[0], std::string("quick"));
    CHECK_EQ(terms[2], std::string("fox"));
    CHECK_EQ(terms[3], std::string("jump"));
    CHECK_EQ(toks[0].position, 1u);  // "the" still consumed position 0
    CHECK_EQ(toks[0].surface, std::string("quick"));
    CHECK_EQ(std::string("Quick"), std::string("The Quick").substr(toks[0].start, toks[0].end - toks[0].start));
}

TEST(tokenizer_keeps_numbers_and_unicode) {
    const auto toks = Tokenizer::tokenize("HTTP2 on port 8080 — café naïve");
    std::vector<std::string> terms;
    for (const auto& t : toks) terms.push_back(t.term);
    CHECK_EQ(terms[0], std::string("http2"));
    CHECK_EQ(terms[1], std::string("port"));
    CHECK_EQ(terms[2], std::string("8080"));
    CHECK_EQ(terms[3], std::string("café"));
}

TEST(varint_roundtrip) {
    std::string buf;
    const uint64_t values[] = {0, 1, 127, 128, 300, 16384, 1u << 31, ~0ull};
    for (auto v : values) putVarint(buf, v);
    const char* p = buf.data();
    const char* end = buf.data() + buf.size();
    for (auto v : values) {
        uint64_t got = 0;
        CHECK(getVarint(p, end, got));
        CHECK_EQ(got, v);
    }
    CHECK(p == end);
}

TEST(query_parser) {
    auto q = parseQuery("rust \"zero cost abstractions\" cache OR buffer -python optim*");
    CHECK_EQ(q.groups.size(), size_t(4));
    CHECK(q.groups[0][0].kind == ItemKind::Term);
    CHECK(q.groups[1][0].kind == ItemKind::Phrase);
    CHECK_EQ(q.groups[1][0].terms.size(), size_t(3));
    CHECK_EQ(q.groups[2].size(), size_t(2));  // cache OR buffer
    CHECK(q.groups[3][0].kind == ItemKind::Prefix);
    CHECK_EQ(q.groups[3][0].terms[0].first, std::string("optim"));
    CHECK_EQ(q.excluded.size(), size_t(1));

    auto p = parseQuery("\"state of the art\"");
    CHECK_EQ(p.groups[0][0].terms.size(), size_t(2));
    CHECK_EQ(p.groups[0][0].terms[1].second, 3u);  // gap preserved across stopwords

    CHECK(parseQuery("the of and").empty());
    CHECK(parseQuery("   ").empty());
}

TEST(html_and_markdown_extraction) {
    std::string title;
    const auto text = extractText(
        "<html><head><title> My Page </title><style>.x{}</style></head><body><p>Hello &amp; welcome</p>"
        "<script>var secret=1;</script></body></html>",
        ".html", title);
    CHECK_EQ(title, std::string("My Page"));
    CHECK(text.find("Hello & welcome") != std::string::npos);
    CHECK(text.find("secret") == std::string::npos);
    CHECK(text.find(".x{}") == std::string::npos);

    std::string mdTitle;
    extractText("intro\n# Real Title\nbody", ".md", mdTitle);
    CHECK_EQ(mdTitle, std::string("Real Title"));
}

TEST(bm25_ranking_and_boolean_operators) {
    Corpus c;
    InvertedIndex idx;
    addFile(idx, c.write("a.txt", "rust ownership and borrowing make memory safety practical"));
    addFile(idx, c.write("b.txt", "rust rust rust: the rust book covers ownership"));
    addFile(idx, c.write("c.txt", "python garbage collection and memory management"));
    addFile(idx, c.write("d.txt", "c++ smart pointers manage memory with RAII"));
    Searcher s(idx, 0);
    SearchOptions o;
    o.snippets = SnippetStyle::None;

    auto r = s.search("rust", o);
    CHECK_EQ(r.totalMatches, size_t(2));
    CHECK_EQ(r.hits[0].title, std::string("b.txt"));  // higher term frequency wins

    r = s.search("memory -python", o);
    CHECK_EQ(r.totalMatches, size_t(2));
    for (const auto& t : titles(r)) CHECK(t != "c.txt");

    r = s.search("rust OR python", o);
    CHECK_EQ(r.totalMatches, size_t(3));

    r = s.search("rust memory", o);  // AND
    CHECK_EQ(r.totalMatches, size_t(1));
    CHECK_EQ(r.hits[0].title, std::string("a.txt"));
    CHECK(!r.relaxed);

    r = s.search("ownership pointers", o);  // no doc has both -> relaxed OR
    CHECK(r.relaxed);
    CHECK_EQ(r.totalMatches, size_t(3));

    r = s.search("manag*", o);
    CHECK_EQ(r.totalMatches, size_t(2));
}

TEST(phrase_queries_respect_order_and_gaps) {
    Corpus c;
    InvertedIndex idx;
    addFile(idx, c.write("1.txt", "the binary search tree is balanced"));
    addFile(idx, c.write("2.txt", "search the binary files quickly"));
    addFile(idx, c.write("3.txt", "this is the state of the art in compilers"));
    Searcher s(idx, 0);
    SearchOptions o;
    o.snippets = SnippetStyle::None;

    auto r = s.search("\"binary search\"", o);
    CHECK_EQ(r.totalMatches, size_t(1));
    CHECK_EQ(r.hits[0].title, std::string("1.txt"));
    CHECK_EQ(s.search("\"search binary\"", o).totalMatches, size_t(0));
    CHECK_EQ(s.search("\"state of the art\"", o).totalMatches, size_t(1));
    CHECK_EQ(s.search("state-of-the-art", o).totalMatches, size_t(1));
}

TEST(snippets_highlight_matches) {
    Corpus c;
    InvertedIndex idx;
    std::string filler;
    for (int i = 0; i < 200; ++i) filler += "lorem ipsum dolor ";
    addFile(idx, c.write("doc.txt", filler + " the quantum <computer> needs error correction " + filler));
    Searcher s(idx, 0);
    SearchOptions o;
    o.snippets = SnippetStyle::Html;
    const auto r = s.search("quantum computing", o);
    CHECK_EQ(r.hits.size(), size_t(1));
    const auto& snip = r.hits[0].snippet;
    CHECK(snip.find("the <mark>quantum</mark> &lt;") != std::string::npos);  // spacing preserved
    CHECK(snip.find("<mark>computer</mark>") != std::string::npos);  // stemmed match
    CHECK(snip.find("&lt;") != std::string::npos);                   // HTML escaped
    CHECK(snip.rfind("… ", 0) == 0);
}

TEST(spelling_suggestions) {
    Corpus c;
    InvertedIndex idx;
    addFile(idx, c.write("x.txt", "kubernetes deployment configuration"));
    addFile(idx, c.write("y.txt", "kubernetes cluster networking"));
    Searcher s(idx, 0);
    // Stemming already absorbs some typos: "kubernets" and "kubernetes" share a stem.
    CHECK_EQ(s.search("kubernets").totalMatches, size_t(2));

    const auto r = s.search("netwrking");
    CHECK_EQ(r.totalMatches, size_t(0));
    CHECK_EQ(r.suggestion, std::string("networking"));

    const auto r2 = s.search("kubernetes clustr");  // one good term, one typo -> relaxed + suggestion
    CHECK(r2.relaxed);
    CHECK_EQ(r2.suggestion, std::string("kubernetes cluster"));
    CHECK_EQ(Searcher::editDistance("kitten", "sitting", 5), size_t(3));
    CHECK_EQ(Searcher::editDistance("ab", "ba", 5), size_t(1));  // transposition
}

TEST(persistence_roundtrip_and_corruption_detection) {
    Corpus c;
    InvertedIndex idx;
    addFile(idx, c.write("one.md", "# Notes\nvector databases and embeddings"));
    addFile(idx, c.write("two.txt", "graph databases store relationships"));
    const std::string file = (c.dir / "test.idx").string();
    std::string err;
    CHECK(idx.save(file, &err));

    InvertedIndex loaded;
    CHECK(loaded.load(file, &err));
    CHECK_EQ(loaded.liveDocuments(), size_t(2));
    CHECK_EQ(loaded.stats().terms, idx.stats().terms);
    Searcher s(loaded, 0);
    SearchOptions o;
    o.snippets = SnippetStyle::None;
    const auto r = s.search("databases", o);
    CHECK_EQ(r.totalMatches, size_t(2));
    CHECK_EQ(s.search("\"vector databases\"", o).hits.at(0).title, std::string("Notes"));

    // Flip one byte -> checksum must catch it.
    {
        std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(20);
        f.put('\x7f');
    }
    InvertedIndex bad;
    CHECK(!bad.load(file, &err));
    CHECK(err.find("checksum") != std::string::npos);
}

TEST(delete_and_compaction) {
    Corpus c;
    InvertedIndex idx;
    addFile(idx, c.write("a.txt", "alpha shared"));
    addFile(idx, c.write("b.txt", "beta shared"));
    addFile(idx, c.write("c.txt", "gamma shared"));
    const auto id = idx.findByPath(normalizePath(c.dir / "b.txt"));
    CHECK(id.has_value());
    CHECK(idx.removeDocument(*id));
    CHECK(!idx.removeDocument(*id));

    Searcher s(idx, 0);
    SearchOptions o;
    o.snippets = SnippetStyle::None;
    CHECK_EQ(s.search("shared", o).totalMatches, size_t(2));
    CHECK_EQ(s.search("beta", o).totalMatches, size_t(0));

    idx.compact();
    CHECK_EQ(idx.docSlots(), size_t(2));
    CHECK(idx.postings("beta") == nullptr);
    CHECK_EQ(idx.doc(1).title, std::string("c.txt"));
    Searcher s2(idx, 0);
    CHECK_EQ(s2.search("gamma", o).hits.at(0).doc, DocId(1));
}

TEST(lru_cache) {
    Corpus c;
    InvertedIndex idx;
    addFile(idx, c.write("a.txt", "cached query result"));
    Searcher s(idx, 2);
    SearchOptions o;
    o.snippets = SnippetStyle::None;
    CHECK(!s.search("cached", o).cached);
    CHECK(s.search("cached", o).cached);
    s.search("query", o);
    s.search("result", o);  // evicts "cached"
    CHECK(!s.search("cached", o).cached);
}

TEST(http_routing) {
    Corpus c;
    InvertedIndex idx;
    addFile(idx, c.write("a.txt", "hello world from lumen"));
    Searcher s(idx, 0);
    HttpServer server(idx, s, "127.0.0.1", 0);

    HttpRequest req;
    CHECK(HttpServer::parseRequest("GET /api/search?q=hello%20world&limit=5 HTTP/1.1\r\nHost: x\r\n\r\n", req));
    CHECK_EQ(req.path, std::string("/api/search"));
    CHECK_EQ(req.query["q"], std::string("hello world"));
    auto resp = server.handle(req);
    CHECK_EQ(resp.status, 200);
    CHECK(resp.body.find("\"total\":1") != std::string::npos);
    CHECK(resp.body.find("<mark>hello</mark>") != std::string::npos);

    CHECK(HttpServer::parseRequest("GET /nope HTTP/1.1\r\n\r\n", req));
    CHECK_EQ(server.handle(req).status, 404);
    CHECK(HttpServer::parseRequest("POST /api/search HTTP/1.1\r\n\r\n", req));
    CHECK_EQ(server.handle(req).status, 405);
    CHECK(!HttpServer::parseRequest("garbage", req));

    HttpRequest doc;
    HttpServer::parseRequest("GET /api/doc?id=99 HTTP/1.1\r\n\r\n", doc);
    CHECK_EQ(server.handle(doc).status, 404);
}

TEST(crawler_filters_files) {
    Corpus c;
    c.write("keep.md", "text");
    c.write("src/main.cpp", "int main(){}");
    c.write(".git/config", "hidden");
    c.write("node_modules/x.js", "vendored");
    c.write("image.png", "not text");
    const auto files = crawl({c.dir.string()});
    CHECK_EQ(files.size(), size_t(2));
}

int main() {
    int failedTests = 0;
    for (const auto& t : registry()) {
        const int before = g_failures;
        t.fn();
        const bool ok = g_failures == before;
        if (!ok) ++failedTests;
        std::cout << (ok ? "[ PASS ] " : "[ FAIL ] ") << t.name << "\n";
    }
    std::cout << "\n" << registry().size() - failedTests << "/" << registry().size() << " tests passed, " << g_checks
              << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
