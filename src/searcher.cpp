#include "lumen/searcher.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

#include "lumen/document_loader.hpp"
#include "lumen/util.hpp"

namespace lumen {
namespace {

using ScoreList = std::vector<std::pair<DocId, double>>;

ScoreList unionLists(const ScoreList& a, const ScoreList& b) {
    ScoreList out;
    out.reserve(a.size() + b.size());
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (j >= b.size() || (i < a.size() && a[i].first < b[j].first)) out.push_back(a[i++]);
        else if (i >= a.size() || b[j].first < a[i].first) out.push_back(b[j++]);
        else {
            out.emplace_back(a[i].first, a[i].second + b[j].second);
            ++i;
            ++j;
        }
    }
    return out;
}

ScoreList intersectLists(const ScoreList& a, const ScoreList& b) {
    ScoreList out;
    out.reserve(std::min(a.size(), b.size()));
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].first < b[j].first) ++i;
        else if (b[j].first < a[i].first) ++j;
        else {
            out.emplace_back(a[i].first, a[i].second + b[j].second);
            ++i;
            ++j;
        }
    }
    return out;
}

const Posting* findPosting(const std::vector<Posting>& list, DocId doc) {
    auto it = std::lower_bound(list.begin(), list.end(), doc,
                               [](const Posting& p, DocId d) { return p.doc < d; });
    return (it != list.end() && it->doc == doc) ? &*it : nullptr;
}

struct Highlighter {
    std::unordered_set<std::string> exact;
    std::vector<std::string> prefixes;

    explicit Highlighter(const ParsedQuery& pq) {
        for (const auto& group : pq.groups)
            for (const auto& item : group)
                for (const auto& [term, off] : item.terms) {
                    (void)off;
                    if (item.kind == ItemKind::Prefix) prefixes.push_back(term);
                    else exact.insert(term);
                }
    }

    bool matches(const std::string& term) const {
        if (exact.count(term)) return true;
        for (const auto& p : prefixes)
            if (term.compare(0, p.size(), p) == 0) return true;
        return false;
    }
};

void appendText(std::string& out, std::string_view text, SnippetStyle style) {
    // Collapse runs of whitespace so snippets read as one line.
    std::string collapsed;
    collapsed.reserve(text.size());
    bool space = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
        } else {
            if (space) collapsed.push_back(' ');
            space = false;
            collapsed.push_back(c);
        }
    }
    if (space) collapsed.push_back(' ');
    out += style == SnippetStyle::Html ? htmlEscape(collapsed) : collapsed;
}

void appendHit(std::string& out, std::string_view word, SnippetStyle style) {
    switch (style) {
        case SnippetStyle::Html: out += "<mark>" + htmlEscape(word) + "</mark>"; break;
        case SnippetStyle::Ansi: out += "\033[1;33m" + std::string(word) + "\033[0m"; break;
        default: out += word; break;
    }
}

}  // namespace

Searcher::Searcher(const InvertedIndex& index, size_t cacheCapacity, Bm25Params params)
    : index_(index), params_(params), cacheCapacity_(cacheCapacity) {}

double Searcher::idf(size_t df) const {
    const double n = static_cast<double>(index_.liveDocuments());
    const double d = static_cast<double>(df);
    return std::log(1.0 + (n - d + 0.5) / (d + 0.5));
}

double Searcher::bm25(uint32_t tf, uint32_t docLen, double idfValue) const {
    const double avg = std::max(index_.avgDocLength(), 1.0);
    const double f = static_cast<double>(tf);
    const double norm = params_.k1 * (1.0 - params_.b + params_.b * static_cast<double>(docLen) / avg);
    return idfValue * (f * (params_.k1 + 1.0)) / (f + norm);
}

Searcher::ScoreList Searcher::scoreTerm(const std::string& term) const {
    ScoreList out;
    const auto* list = index_.postings(term);
    if (!list) return out;
    const double w = idf(list->size());
    out.reserve(list->size());
    for (const auto& p : *list) {
        const auto& d = index_.doc(p.doc);
        if (d.deleted) continue;
        out.emplace_back(p.doc, bm25(p.tf(), d.length, w));
    }
    return out;
}

Searcher::ScoreList Searcher::scorePrefix(const std::string& prefix) const {
    ScoreList out;
    for (const auto& term : index_.expandPrefix(prefix, 64)) out = unionLists(out, scoreTerm(term));
    return out;
}

Searcher::ScoreList Searcher::scorePhrase(const QueryItem& item) const {
    ScoreList out;
    std::vector<const std::vector<Posting>*> lists;
    std::vector<double> idfs;
    for (const auto& [term, off] : item.terms) {
        (void)off;
        const auto* l = index_.postings(term);
        if (!l) return out;
        lists.push_back(l);
        idfs.push_back(idf(l->size()));
    }
    // Drive the intersection from the rarest term.
    size_t driver = 0;
    for (size_t i = 1; i < lists.size(); ++i)
        if (lists[i]->size() < lists[driver]->size()) driver = i;

    std::vector<const Posting*> hit(lists.size());
    for (const auto& dp : *lists[driver]) {
        const auto& d = index_.doc(dp.doc);
        if (d.deleted) continue;
        bool all = true;
        for (size_t i = 0; i < lists.size() && all; ++i) {
            hit[i] = (i == driver) ? &dp : findPosting(*lists[i], dp.doc);
            all = hit[i] != nullptr;
        }
        if (!all) continue;

        // Verify adjacency: for each position of term 0, every other term must
        // appear at the same relative offset as in the query.
        uint32_t matches = 0;
        for (uint32_t p0 : hit[0]->positions) {
            bool ok = true;
            for (size_t i = 1; i < lists.size() && ok; ++i) {
                const uint32_t want = p0 + (item.terms[i].second - item.terms[0].second);
                ok = std::binary_search(hit[i]->positions.begin(), hit[i]->positions.end(), want);
            }
            if (ok) ++matches;
        }
        if (matches == 0) continue;

        double score = 0.0;
        for (size_t i = 0; i < lists.size(); ++i) score += bm25(matches, d.length, idfs[i]);
        out.emplace_back(dp.doc, score * params_.phraseBoost);
    }
    return out;
}

Searcher::ScoreList Searcher::scoreItem(const QueryItem& item) const {
    switch (item.kind) {
        case ItemKind::Term: return scoreTerm(item.terms[0].first);
        case ItemKind::Prefix: return scorePrefix(item.terms[0].first);
        case ItemKind::Phrase: return scorePhrase(item);
    }
    return {};
}

size_t Searcher::editDistance(std::string_view a, std::string_view b, size_t cap) {
    // Optimal string alignment distance (Levenshtein + adjacent transpositions).
    const size_t n = a.size(), m = b.size();
    if ((n > m ? n - m : m - n) > cap) return cap + 1;
    std::vector<size_t> prev2(m + 1), prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        size_t rowMin = cur[0];
        for (size_t j = 1; j <= m; ++j) {
            const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                cur[j] = std::min(cur[j], prev2[j - 2] + 1);
            rowMin = std::min(rowMin, cur[j]);
        }
        if (rowMin > cap) return cap + 1;
        prev2.swap(prev);
        prev.swap(cur);
    }
    return prev[m];
}

std::string Searcher::suggest(std::string_view query, const ParsedQuery& pq) const {
    std::string rewritten(query);
    bool changed = false;
    for (const auto& group : pq.groups) {
        for (const auto& item : group) {
            if (item.kind != ItemKind::Term || index_.documentFrequency(item.terms[0].first) > 0) continue;
            const std::string word = toLowerAscii(item.raw);
            if (word.size() < 3) continue;
            const size_t cap = word.size() <= 4 ? 1 : 2;

            std::string best;
            size_t bestDist = cap + 1, bestDf = 0;
            for (const auto& [stem, surface] : index_.surfaceForms()) {
                const size_t d = editDistance(word, surface, cap);
                if (d > cap) continue;
                const size_t df = index_.documentFrequency(stem);
                if (d < bestDist || (d == bestDist && df > bestDf)) {
                    best = surface;
                    bestDist = d;
                    bestDf = df;
                }
            }
            if (best.empty()) continue;
            const size_t at = rewritten.find(item.raw);
            if (at == std::string::npos) continue;
            rewritten.replace(at, item.raw.size(), best);
            changed = true;
        }
    }
    return changed ? rewritten : std::string();
}

std::string Searcher::makeSnippet(DocId id, const ParsedQuery& pq, SnippetStyle style) const {
    constexpr size_t kWindow = 32;
    const auto loaded = loadDocument(index_.doc(id).path);
    if (!loaded) return {};
    const std::string& text = loaded->text;
    const auto tokens = Tokenizer::tokenize(text, /*keepStopwords=*/true);
    if (tokens.empty()) return {};

    const Highlighter hl(pq);
    std::vector<size_t> hits;
    for (size_t i = 0; i < tokens.size() && hits.size() < 5000; ++i)
        if (hl.matches(tokens[i].term)) hits.push_back(i);

    // Pick the window that covers the most distinct query terms (ties: most hits).
    size_t bestStart = 0, bestDistinct = 0, bestCount = 0;
    for (size_t h = 0; h < hits.size(); ++h) {
        const size_t start = hits[h] > 4 ? hits[h] - 4 : 0;
        std::unordered_set<std::string_view> distinct;
        size_t count = 0;
        for (size_t k = h; k < hits.size() && hits[k] < start + kWindow; ++k) {
            distinct.insert(tokens[hits[k]].term);
            ++count;
        }
        if (distinct.size() > bestDistinct || (distinct.size() == bestDistinct && count > bestCount)) {
            bestStart = start;
            bestDistinct = distinct.size();
            bestCount = count;
        }
    }
    const size_t endTok = std::min(tokens.size(), bestStart + kWindow);

    std::string out;
    if (bestStart > 0) out += "… ";
    size_t cursor = tokens[bestStart].start;
    for (size_t i = bestStart; i < endTok; ++i) {
        const auto& t = tokens[i];
        appendText(out, std::string_view(text).substr(cursor, t.start - cursor), style);
        const std::string_view word = std::string_view(text).substr(t.start, t.end - t.start);
        if (hl.matches(t.term)) appendHit(out, word, style);
        else appendText(out, word, style);
        cursor = t.end;
    }
    if (endTok < tokens.size()) out += " …";
    return out;
}

SearchResponse Searcher::search(std::string_view query, const SearchOptions& opts) const {
    const Timer timer;
    const std::string key = std::string(query) + '\x1f' + std::to_string(opts.limit) + '\x1f' +
                            std::to_string(opts.offset) + '\x1f' + std::to_string(static_cast<int>(opts.snippets));
    if (opts.useCache && cacheCapacity_ > 0) {
        std::lock_guard<std::mutex> lock(cacheMu_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second);
            SearchResponse r = it->second->second;
            r.cached = true;
            r.elapsedMicros = timer.micros();
            return r;
        }
    }

    SearchResponse resp;
    const ParsedQuery pq = parseQuery(query);
    if (pq.empty()) {
        resp.elapsedMicros = timer.micros();
        return resp;
    }

    // Score every OR-group, then AND them together (rarest group first).
    std::vector<ScoreList> groupLists;
    for (const auto& group : pq.groups) {
        ScoreList g;
        for (const auto& item : group) g = unionLists(g, scoreItem(item));
        groupLists.push_back(std::move(g));
    }
    std::vector<size_t> order(groupLists.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return groupLists[a].size() < groupLists[b].size(); });

    ScoreList combined = groupLists[order[0]];
    for (size_t i = 1; i < order.size(); ++i) combined = intersectLists(combined, groupLists[order[i]]);

    if (combined.empty() && groupLists.size() > 1) {
        resp.relaxed = true;
        for (const auto& g : groupLists) combined = unionLists(combined, g);
    }

    if (!pq.excluded.empty() && !combined.empty()) {
        std::unordered_set<DocId> banned;
        for (const auto& item : pq.excluded)
            for (const auto& [doc, s] : scoreItem(item)) {
                (void)s;
                banned.insert(doc);
            }
        combined.erase(std::remove_if(combined.begin(), combined.end(),
                                      [&](const auto& e) { return banned.count(e.first) > 0; }),
                       combined.end());
    }

    resp.totalMatches = combined.size();
    const size_t want = std::min(combined.size(), opts.offset + opts.limit);
    const auto byScore = [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    };
    std::partial_sort(combined.begin(), combined.begin() + static_cast<std::ptrdiff_t>(want), combined.end(), byScore);

    for (size_t i = opts.offset; i < want; ++i) {
        const auto& d = index_.doc(combined[i].first);
        SearchHit hit;
        hit.doc = combined[i].first;
        hit.score = combined[i].second;
        hit.path = d.path;
        hit.title = d.title;
        if (opts.snippets != SnippetStyle::None) hit.snippet = makeSnippet(hit.doc, pq, opts.snippets);
        resp.hits.push_back(std::move(hit));
    }

    if (resp.totalMatches == 0 || resp.relaxed) resp.suggestion = suggest(query, pq);
    resp.elapsedMicros = timer.micros();

    if (opts.useCache && cacheCapacity_ > 0) {
        std::lock_guard<std::mutex> lock(cacheMu_);
        if (!cache_.count(key)) {
            lru_.emplace_front(key, resp);
            cache_[key] = lru_.begin();
            if (lru_.size() > cacheCapacity_) {
                cache_.erase(lru_.back().first);
                lru_.pop_back();
            }
        }
    }
    return resp;
}

void Searcher::clearCache() const {
    std::lock_guard<std::mutex> lock(cacheMu_);
    lru_.clear();
    cache_.clear();
}

}  // namespace lumen
