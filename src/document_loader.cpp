#include "lumen/document_loader.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_set>

#include "lumen/util.hpp"

namespace fs = std::filesystem;

namespace lumen {
namespace {

const std::unordered_set<std::string>& textExtensions() {
    static const std::unordered_set<std::string> exts = {
        ".txt", ".md", ".markdown", ".rst", ".org", ".tex", ".html", ".htm", ".xml", ".csv", ".tsv",
        ".log", ".json", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf",
        ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".py", ".js", ".ts", ".jsx", ".tsx", ".java",
        ".kt", ".go", ".rs", ".rb", ".php", ".cs", ".swift", ".scala", ".sh", ".bash", ".zsh", ".sql",
        ".lua", ".r", ".m", ".pl", ".css", ".scss", ".vue", ".svelte", ".dart", ".hs", ".ex", ".exs"};
    return exts;
}

bool looksBinary(const std::string& data) {
    const size_t probe = std::min<size_t>(data.size(), 4096);
    return std::find(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(probe), '\0') !=
           data.begin() + static_cast<std::ptrdiff_t>(probe);
}

bool iequalsAt(const std::string& s, size_t pos, std::string_view needle) {
    if (pos + needle.size() > s.size()) return false;
    for (size_t i = 0; i < needle.size(); ++i)
        if (asciiLower(s[pos + i]) != needle[i]) return false;
    return true;
}

size_t ifind(const std::string& s, std::string_view needle, size_t from) {
    for (size_t i = from; i + needle.size() <= s.size(); ++i)
        if (iequalsAt(s, i, needle)) return i;
    return std::string::npos;
}

std::string decodeEntity(const std::string& ent) {
    if (ent == "amp") return "&";
    if (ent == "lt") return "<";
    if (ent == "gt") return ">";
    if (ent == "quot") return "\"";
    if (ent == "apos" || ent == "#39") return "'";
    if (ent == "nbsp") return " ";
    return " ";
}

std::string trim(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string stripHtml(const std::string& raw, std::string& title) {
    const size_t t0 = ifind(raw, "<title", 0);
    if (t0 != std::string::npos) {
        const size_t gt = raw.find('>', t0);
        const size_t t1 = gt == std::string::npos ? gt : ifind(raw, "</title>", gt);
        if (t1 != std::string::npos) title = trim(raw.substr(gt + 1, t1 - gt - 1));
    }

    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        const char c = raw[i];
        if (c == '<') {
            // Drop script/style blocks entirely.
            for (std::string_view tag : {std::string_view("script"), std::string_view("style")}) {
                if (iequalsAt(raw, i + 1, tag)) {
                    const std::string close = "</" + std::string(tag);
                    const size_t endTag = ifind(raw, close, i);
                    i = endTag == std::string::npos ? raw.size() : endTag;
                    break;
                }
            }
            const size_t gt = raw.find('>', i);
            i = gt == std::string::npos ? raw.size() : gt + 1;
            out.push_back(' ');
        } else if (c == '&') {
            const size_t semi = raw.find(';', i);
            if (semi != std::string::npos && semi - i <= 8) {
                out += decodeEntity(raw.substr(i + 1, semi - i - 1));
                i = semi + 1;
            } else {
                out.push_back(c);
                ++i;
            }
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

}  // namespace

bool isIndexableExtension(const fs::path& p) {
    return textExtensions().count(toLowerAscii(p.extension().string())) > 0;
}

int64_t fileMtime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return static_cast<int64_t>(t.time_since_epoch().count());
}

std::string normalizePath(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(p, ec), ec);
    if (ec) abs = fs::absolute(p);
    return abs.lexically_normal().string();
}

std::vector<fs::path> crawl(const std::vector<std::string>& roots, const CrawlOptions& opts) {
    std::vector<fs::path> files;
    for (const auto& root : roots) {
        std::error_code ec;
        if (fs::is_regular_file(root, ec)) {
            files.emplace_back(root);
            continue;
        }
        if (!fs::is_directory(root, ec)) continue;

        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::path& p = it->path();
            const std::string name = p.filename().string();
            if (!opts.includeHidden && !name.empty() && name[0] == '.') {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (it->is_directory(ec)) {
                if (name == "node_modules" || name == "__pycache__" || name == "build" || name == "target") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!it->is_regular_file(ec) || !isIndexableExtension(p)) continue;
            if (it->file_size(ec) > opts.maxFileBytes || ec) continue;
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

std::string extractText(const std::string& raw, const std::string& extension, std::string& titleOut) {
    const std::string ext = toLowerAscii(extension);
    if (ext == ".html" || ext == ".htm" || ext == ".xml") return stripHtml(raw, titleOut);

    if (ext == ".md" || ext == ".markdown") {
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t eol = raw.find('\n', pos);
            if (eol == std::string::npos) eol = raw.size();
            std::string line = trim(raw.substr(pos, eol - pos));
            if (line.rfind("# ", 0) == 0) {
                titleOut = trim(line.substr(2));
                break;
            }
            pos = eol + 1;
        }
    }
    return raw;
}

std::optional<LoadedDocument> loadDocument(const fs::path& p, uint64_t maxBytes) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string raw;
    in.seekg(0, std::ios::end);
    const auto size = static_cast<uint64_t>(in.tellg());
    if (size > maxBytes) return std::nullopt;
    in.seekg(0, std::ios::beg);
    raw.resize(static_cast<size_t>(size));
    if (size > 0 && !in.read(raw.data(), static_cast<std::streamsize>(size))) return std::nullopt;
    if (looksBinary(raw)) return std::nullopt;

    LoadedDocument doc;
    doc.path = normalizePath(p);
    doc.size = size;
    doc.mtime = fileMtime(p);
    doc.text = extractText(raw, p.extension().string(), doc.title);
    if (doc.title.empty()) doc.title = p.filename().string();
    if (doc.title.size() > 200) doc.title.resize(200);
    return doc;
}

}  // namespace lumen
