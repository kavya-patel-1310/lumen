#include "lumen/index.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

#include "lumen/util.hpp"

namespace lumen {
namespace {

constexpr char kMagic[8] = {'L', 'U', 'M', 'E', 'N', 'I', 'D', 'X'};

void putString(std::string& out, const std::string& s) {
    putVarint(out, s.size());
    out += s;
}

bool getString(const char*& p, const char* end, std::string& s) {
    uint64_t len = 0;
    if (!getVarint(p, end, len) || len > static_cast<uint64_t>(end - p)) return false;
    s.assign(p, static_cast<size_t>(len));
    p += len;
    return true;
}

bool fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

}  // namespace

DocId InvertedIndex::addDocument(DocInfo info, const std::vector<Token>& tokens) {
    // Group positions by term before taking the lock.
    std::unordered_map<std::string, std::vector<uint32_t>> local;
    std::unordered_map<std::string, const std::string*> surfaces;
    local.reserve(tokens.size() / 2 + 1);
    for (const auto& t : tokens) {
        auto& positions = local[t.term];
        positions.push_back(t.position);
        if (positions.size() == 1) surfaces[t.term] = &t.surface;
    }
    info.length = static_cast<uint32_t>(tokens.size());
    info.deleted = false;

    std::lock_guard<std::mutex> lock(writeMu_);
    const DocId id = static_cast<DocId>(docs_.size());
    totalLength_ += info.length;
    ++liveDocs_;
    pathToId_[info.path] = id;
    docs_.push_back(std::move(info));
    for (auto& [term, positions] : local) {
        auto [it, inserted] = terms_.try_emplace(term);
        it->second.push_back(Posting{id, std::move(positions)});
        if (inserted) {
            sortedDirty_ = true;
            surface_.emplace(term, *surfaces[term]);
        }
    }
    return id;
}

bool InvertedIndex::removeDocument(DocId id) {
    std::lock_guard<std::mutex> lock(writeMu_);
    if (id >= docs_.size() || docs_[id].deleted) return false;
    docs_[id].deleted = true;
    totalLength_ -= docs_[id].length;
    --liveDocs_;
    pathToId_.erase(docs_[id].path);
    return true;
}

std::optional<DocId> InvertedIndex::findByPath(const std::string& path) const {
    auto it = pathToId_.find(path);
    if (it == pathToId_.end()) return std::nullopt;
    return it->second;
}

const std::vector<Posting>* InvertedIndex::postings(const std::string& term) const {
    auto it = terms_.find(term);
    return it == terms_.end() ? nullptr : &it->second;
}

size_t InvertedIndex::documentFrequency(const std::string& term) const {
    const auto* p = postings(term);
    return p ? p->size() : 0;
}

double InvertedIndex::avgDocLength() const {
    return liveDocs_ == 0 ? 0.0 : static_cast<double>(totalLength_) / static_cast<double>(liveDocs_);
}

std::vector<std::string> InvertedIndex::expandPrefix(const std::string& prefix, size_t limit) const {
    std::lock_guard<std::mutex> lock(sortedMu_);
    if (sortedDirty_) {
        sortedTerms_.clear();
        sortedTerms_.reserve(terms_.size());
        for (const auto& kv : terms_) sortedTerms_.push_back(kv.first);
        std::sort(sortedTerms_.begin(), sortedTerms_.end());
        sortedDirty_ = false;
    }
    std::vector<std::string> out;
    for (auto it = std::lower_bound(sortedTerms_.begin(), sortedTerms_.end(), prefix);
         it != sortedTerms_.end() && it->compare(0, prefix.size(), prefix) == 0 && out.size() < limit; ++it) {
        out.push_back(*it);
    }
    return out;
}

IndexStats InvertedIndex::stats() const {
    IndexStats s;
    s.documents = liveDocs_;
    s.terms = terms_.size();
    s.totalTokens = totalLength_;
    s.avgDocLength = avgDocLength();
    for (const auto& kv : terms_) {
        s.postings += kv.second.size();
        for (const auto& p : kv.second) s.positions += p.positions.size();
    }
    for (const auto& d : docs_)
        if (!d.deleted) s.corpusBytes += d.size;
    return s;
}

void InvertedIndex::compact() {
    std::lock_guard<std::mutex> lock(writeMu_);
    if (liveDocs_ == docs_.size()) return;

    constexpr DocId kGone = std::numeric_limits<DocId>::max();
    std::vector<DocId> remap(docs_.size(), kGone);
    std::vector<DocInfo> newDocs;
    newDocs.reserve(liveDocs_);
    for (DocId i = 0; i < docs_.size(); ++i) {
        if (docs_[i].deleted) continue;
        remap[i] = static_cast<DocId>(newDocs.size());
        newDocs.push_back(std::move(docs_[i]));
    }

    for (auto it = terms_.begin(); it != terms_.end();) {
        auto& list = it->second;
        size_t w = 0;
        for (auto& p : list) {
            if (remap[p.doc] == kGone) continue;
            p.doc = remap[p.doc];  // remap is monotonic, so order is preserved
            list[w++] = std::move(p);
        }
        list.resize(w);
        if (list.empty()) {
            surface_.erase(it->first);
            it = terms_.erase(it);
            sortedDirty_ = true;
        } else {
            ++it;
        }
    }

    docs_ = std::move(newDocs);
    pathToId_.clear();
    for (DocId i = 0; i < docs_.size(); ++i) pathToId_[docs_[i].path] = i;
}

bool InvertedIndex::save(const std::string& file, std::string* error) {
    compact();

    std::string buf;
    buf.append(kMagic, sizeof(kMagic));
    putVarint(buf, kFormatVersion);

    putVarint(buf, docs_.size());
    for (const auto& d : docs_) {
        putString(buf, d.path);
        putString(buf, d.title);
        putVarint(buf, d.length);
        putVarint(buf, static_cast<uint64_t>(d.mtime));
        putVarint(buf, d.size);
    }

    putVarint(buf, terms_.size());
    for (const auto& [term, list] : terms_) {
        putString(buf, term);
        auto s = surface_.find(term);
        putString(buf, s == surface_.end() ? std::string() : s->second);
        putVarint(buf, list.size());
        DocId prevDoc = 0;
        for (const auto& p : list) {
            putVarint(buf, p.doc - prevDoc);  // delta-encoded doc ids
            prevDoc = p.doc;
            putVarint(buf, p.positions.size());
            uint32_t prevPos = 0;
            for (uint32_t pos : p.positions) {
                putVarint(buf, pos - prevPos);  // delta-encoded positions
                prevPos = pos;
            }
        }
    }

    const uint64_t checksum = fnv1a64(buf);
    for (int i = 0; i < 8; ++i) buf.push_back(static_cast<char>((checksum >> (8 * i)) & 0xFF));

    const std::string tmp = file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return fail(error, "cannot open " + tmp + " for writing");
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (!out) return fail(error, "write failed for " + tmp);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(file, ec);
        std::filesystem::rename(tmp, file, ec);
        if (ec) return fail(error, "cannot rename index file: " + ec.message());
    }
    return true;
}

bool InvertedIndex::load(const std::string& file, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail(error, "cannot open " + file);
    std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (buf.size() < sizeof(kMagic) + 8 || std::memcmp(buf.data(), kMagic, sizeof(kMagic)) != 0)
        return fail(error, "not a lumen index file");

    uint64_t stored = 0;
    for (int i = 0; i < 8; ++i)
        stored |= static_cast<uint64_t>(static_cast<uint8_t>(buf[buf.size() - 8 + i])) << (8 * i);
    const std::string_view body(buf.data(), buf.size() - 8);
    if (fnv1a64(body) != stored) return fail(error, "index checksum mismatch (file is corrupt)");

    const char* p = buf.data() + sizeof(kMagic);
    const char* end = buf.data() + buf.size() - 8;
    uint64_t version = 0, n = 0, v = 0;
    if (!getVarint(p, end, version) || version != kFormatVersion)
        return fail(error, "unsupported index version");

    std::vector<DocInfo> docs;
    if (!getVarint(p, end, n)) return fail(error, "truncated document table");
    docs.resize(static_cast<size_t>(n));
    uint64_t total = 0;
    for (auto& d : docs) {
        uint64_t len = 0, mtime = 0, size = 0;
        if (!getString(p, end, d.path) || !getString(p, end, d.title) || !getVarint(p, end, len) ||
            !getVarint(p, end, mtime) || !getVarint(p, end, size))
            return fail(error, "truncated document record");
        d.length = static_cast<uint32_t>(len);
        d.mtime = static_cast<int64_t>(mtime);
        d.size = size;
        total += len;
    }

    std::unordered_map<std::string, std::vector<Posting>> terms;
    std::unordered_map<std::string, std::string> surface;
    if (!getVarint(p, end, n)) return fail(error, "truncated term table");
    terms.reserve(static_cast<size_t>(n));
    for (uint64_t t = 0; t < n; ++t) {
        std::string term, surf;
        uint64_t df = 0;
        if (!getString(p, end, term) || !getString(p, end, surf) || !getVarint(p, end, df))
            return fail(error, "truncated term record");
        auto& list = terms[term];
        list.resize(static_cast<size_t>(df));
        uint64_t doc = 0;
        for (auto& posting : list) {
            uint64_t tf = 0;
            if (!getVarint(p, end, v) || !getVarint(p, end, tf)) return fail(error, "truncated postings");
            doc += v;
            if (doc >= docs.size()) return fail(error, "posting references unknown document");
            posting.doc = static_cast<DocId>(doc);
            posting.positions.resize(static_cast<size_t>(tf));
            uint64_t pos = 0;
            for (auto& x : posting.positions) {
                if (!getVarint(p, end, v)) return fail(error, "truncated positions");
                pos += v;
                x = static_cast<uint32_t>(pos);
            }
        }
        if (!surf.empty()) surface.emplace(term, std::move(surf));
    }

    std::lock_guard<std::mutex> lock(writeMu_);
    docs_ = std::move(docs);
    terms_ = std::move(terms);
    surface_ = std::move(surface);
    totalLength_ = total;
    liveDocs_ = docs_.size();
    pathToId_.clear();
    for (DocId i = 0; i < docs_.size(); ++i) pathToId_[docs_[i].path] = i;
    std::lock_guard<std::mutex> lock2(sortedMu_);
    sortedDirty_ = true;
    return true;
}

}  // namespace lumen
