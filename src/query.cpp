#include "lumen/query.hpp"

#include <cctype>

#include "lumen/tokenizer.hpp"
#include "lumen/util.hpp"

namespace lumen {
namespace {

struct Unit {
    std::string text;
    bool quoted = false;
    bool negated = false;
    bool isOr = false;
};

std::vector<Unit> lex(std::string_view q) {
    std::vector<Unit> units;
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && std::isspace(static_cast<unsigned char>(q[i]))) ++i;
        if (i >= q.size()) break;
        Unit u;
        if (q[i] == '-' && i + 1 < q.size() && !std::isspace(static_cast<unsigned char>(q[i + 1]))) {
            u.negated = true;
            ++i;
        }
        if (q[i] == '"') {
            const size_t close = q.find('"', i + 1);
            const size_t stop = close == std::string_view::npos ? q.size() : close;
            u.text = std::string(q.substr(i + 1, stop - i - 1));
            u.quoted = true;
            i = close == std::string_view::npos ? q.size() : close + 1;
        } else {
            const size_t start = i;
            while (i < q.size() && !std::isspace(static_cast<unsigned char>(q[i]))) ++i;
            u.text = std::string(q.substr(start, i - start));
            if (!u.negated && (u.text == "OR" || u.text == "|")) u.isOr = true;
        }
        units.push_back(std::move(u));
    }
    return units;
}

bool toItem(const Unit& u, QueryItem& item) {
    item.raw = u.text;
    // Prefix query: a single bare word ending in '*'.
    if (!u.quoted && u.text.size() >= 3 && u.text.back() == '*') {
        std::string prefix = toLowerAscii(u.text.substr(0, u.text.size() - 1));
        bool simple = true;
        for (char c : prefix)
            if (!std::isalnum(static_cast<unsigned char>(c))) simple = false;
        if (simple) {
            item.kind = ItemKind::Prefix;
            item.terms = {{prefix, 0}};
            return true;
        }
    }

    const auto tokens = Tokenizer::tokenize(u.text);
    if (tokens.empty()) return false;
    const uint32_t base = tokens.front().position;
    for (const auto& t : tokens) item.terms.emplace_back(t.term, t.position - base);
    // "state-of-the-art" or a quoted string with 2+ words becomes a phrase.
    item.kind = item.terms.size() > 1 ? ItemKind::Phrase : ItemKind::Term;
    return true;
}

}  // namespace

ParsedQuery parseQuery(std::string_view query) {
    ParsedQuery pq;
    bool pendingOr = false;
    for (const auto& u : lex(query)) {
        if (u.isOr) {
            pendingOr = !pq.groups.empty();
            continue;
        }
        QueryItem item;
        if (!toItem(u, item)) continue;
        if (u.negated) {
            pq.excluded.push_back(std::move(item));
        } else if (pendingOr) {
            pq.groups.back().push_back(std::move(item));
        } else {
            pq.groups.push_back({std::move(item)});
        }
        pendingOr = false;
    }
    return pq;
}

}  // namespace lumen
