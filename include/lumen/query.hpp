#pragma once
// Query language:
//   rust async          -> both terms required (ranked AND, relaxes to OR if empty)
//   "binary search"     -> exact phrase (positional match)
//   cache OR buffer     -> either term
//   -deprecated         -> exclude documents containing the term
//   optim*              -> prefix match over the vocabulary

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lumen {

enum class ItemKind { Term, Prefix, Phrase };

struct QueryItem {
    ItemKind kind = ItemKind::Term;
    // (normalized term, position offset relative to the first word).
    // Term: one entry. Prefix: one entry holding the raw lowercase prefix. Phrase: 2+ entries.
    std::vector<std::pair<std::string, uint32_t>> terms;
    std::string raw;  // what the user typed, used for "did you mean" rewriting
};

struct ParsedQuery {
    std::vector<std::vector<QueryItem>> groups;  // AND of OR-groups
    std::vector<QueryItem> excluded;
    bool empty() const { return groups.empty(); }
};

ParsedQuery parseQuery(std::string_view query);

}  // namespace lumen
